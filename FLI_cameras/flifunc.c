/*
 * This file is part of the FLI_control project.
 * Copyright 2020 Edward V. Emelianov <edward.emelianoff@gmail.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <fitsio.h>
#include <libfli.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "flifunc.h"

#define LIBVERSIZ 1024

#ifndef FLIUSB_VENDORID
#define FLIUSB_VENDORID 0xf18
#endif
#ifndef FLIUSB_PROLINE_ID
#define FLIUSB_PROLINE_ID 0x0a
#endif
#ifndef FLIUSB_FILTER_ID
#define FLIUSB_FILTER_ID 0x07
#endif
#ifndef FLIUSB_FOCUSER_ID
#define FLIUSB_FOCUSER_ID 0x06
#endif

// wheel position in steps = WHEEL_POS0STPS + WHEEL_STEPPOS*N
#define WHEEL_POS0STPS  (239)
#define WHEEL_STEPPOS   (48)
// 1mm == FOCSCALE steps of focuser
#define FOCSCALE        (10000.)

#define TRYFUNC(f, ...)             \
do{ if((fli_err = f(__VA_ARGS__)))  \
        WARNX(#f "() failed");      \
}while(0)

#define TRYFITS(f, ...)                     \
do{ int status = 0;                         \
    f(__VA_ARGS__, &status);                \
    if (status){                            \
        fits_report_error(stderr, status);  \
        return -1;}                         \
}while(0)
#define WRITEKEY(...)                           \
do{ int status = 0;                             \
    fits_write_key(__VA_ARGS__, &status);       \
    if(status) fits_report_error(stderr, status);\
}while(0)

typedef struct{
    flidomain_t domain;
    char *dname;
    char *name;
}cam_t;

static char camname[BUFSIZ] = {0}, whlname[BUFSIZ], focname[BUFSIZ];
static long fli_err, tmpl;
static cam_t *camz = NULL, *whlz = NULL, *focz = NULL;
static flidev_t camdev, whldev, focdev;
static capture_status capStatus = CAPTURE_NO;
static int curhbin = 1, curvbin = 1;
static long filterpos = -1, filtermax = -1;  // filter position
static long focuserpos = -1, focmaxpos = -1; // focuser position

static int fli_init(){
    char libver[LIBVERSIZ]; // FLI library version
     TRYFUNC(FLISetDebugLevel, NULL /* "NO HOST" */, FLIDEBUG_NONE);
     if(fli_err) return FALSE;
     TRYFUNC(FLIGetLibVersion, libver, LIBVERSIZ);
     if(!fli_err) DBG("Library version '%s'", libver);
     return TRUE;
}

static int findcams(flidomain_t domain, cam_t **cam){
    char **tmplist;
    int numcams = 0;
    TRYFUNC(FLIList, domain, &tmplist);
    if(tmplist && tmplist[0]){
        int i, cams = 0;
        for(i = 0; tmplist[i]; i++) cams++;
        if((*cam = realloc(*cam, (numcams + cams) * sizeof(cam_t))) == NULL)
            ERR("realloc() failed");
        for(i = 0; tmplist[i]; i++){
            int j;
            cam_t *tmpcam = *cam + i;
            for (j = 0; tmplist[i][j] != '\0'; j++)
                if (tmplist[i][j] == ';'){
                    tmplist[i][j] = '\0';
                    break;
                }
            tmpcam->domain = domain;
            tmpcam->name = strdup(tmplist[i]);
            switch (domain & 0xff){
            case FLIDOMAIN_PARALLEL_PORT:
                tmpcam->dname = "parallel port";
                break;
            case FLIDOMAIN_USB:
                tmpcam->dname = "USB";
                break;
            case FLIDOMAIN_SERIAL:
                tmpcam->dname = "serial";
                break;
            case FLIDOMAIN_INET:
                tmpcam->dname = "inet";
                break;
            default:
                tmpcam->dname = "Unknown domain";
                break;
            }
            DBG("found: %s @ %s", tmpcam->name, tmpcam->dname);
        }
        numcams += cams;
    }
    TRYFUNC(FLIFreeList, tmplist);
    return numcams;
}

static int fli_findCCD(){
    DBG("Try to find FLI cameras .. ");
    if(!fli_init()){
        DBG("FLI not found");
        return FALSE;
    }
    if(!camz){ // build cameras list
        FLIcam.Ndevices = findcams(FLIDOMAIN_USB | FLIDEVICE_CAMERA, &camz);
        if(!FLIcam.Ndevices){
            DBG("No cameras");
            return FALSE;
        }
        for(int i = 0; i < FLIcam.Ndevices; i++){
            DBG("Camera '%s', domain %s", camz[i].name, camz[i].dname);
        }
    }
    return TRUE;
}
static int fli_setActiceCam(int n){
    if(!camz && !fli_findCCD()) return FALSE;
    if(n >= FLIcam.Ndevices){
        return FALSE;
    }
    FLIClose(camdev);
    TRYFUNC(FLIOpen, &camdev, camz[n].name, camz[n].domain);
    if(fli_err){
        return FALSE;
    }
    TRYFUNC(FLIGetModel, camdev, camname, BUFSIZ);
#ifdef EBUG
    if(!fli_err) DBG("Model: %s", camname);
    TRYFUNC(FLIGetHWRevision, camdev, &tmpl);
    if(!fli_err) DBG("HW revision: %ld", tmpl);
    TRYFUNC(FLIGetFWRevision, camdev, &tmpl);
    if(!fli_err) DBG("SW revision: %ld", tmpl);
#endif
    double x,y;
    TRYFUNC(FLIGetPixelSize, camdev, &x, &y);
    if(!fli_err){
        DBG("Pixel size: %g x %g", x,y);
        FLIcam.pixX = (float)x;
        FLIcam.pixY = (float)y;
    }
    long x0, x1, y0, y1;
    TRYFUNC(FLIGetVisibleArea, camdev, &x0, &y0, &x1, &y1);
    if(!fli_err){
        DBG("Field of view: (%ld, %ld)(%ld, %ld)", x0, y0, x1, y1);
        FLIcam.field = (frameformat){.w = x1 - x0, .h = y1 - y0, .xoff = x0, .yoff = y0};
    }
    TRYFUNC(FLIGetArrayArea, camdev, &x0, &y0, &x1, &y1);
    if(!fli_err){
        DBG("Array field: (%ld, %ld)(%ld, %ld)", x0, y0, x1, y1);
        FLIcam.array = (frameformat){.w = x1 - x0, .h = y1 - y0, .xoff = x0, .yoff = y0};
    }
    return TRUE;
}

static int fli_geomlimits(frameformat *l, frameformat *s){
    if(l) *l = FLIcam.array;
    if(s) *s = (frameformat){.w = 1, .h = 1, .xoff = 1, .yoff = 1};
    return TRUE;
}

static int fli_findFocuser(){
    DBG("Try to find FLI focusers .. ");
    if(!fli_init()){
        DBG("FLI not found");
        return FALSE;
    }
    if(!focz){
        FLIfocus.Ndevices = findcams(FLIDOMAIN_USB | FLIDEVICE_FOCUSER, &focz);
        if(!FLIfocus.Ndevices){
            DBG("No focusers");
            return FALSE;
        }
        for(int i = 0; i < FLIfocus.Ndevices; i++){
            DBG("Focuser '%s', domain %s", focz[i].name, focz[i].dname);
        }
    }
    return TRUE;
}
static int fli_setActiceFocuser(int n){
    if(!focz && !fli_findFocuser()) return FALSE;
    if(n >= FLIfocus.Ndevices) return FALSE;
    FLIClose(focdev);
    int OK = FALSE;
    for(int i = 0; i < FLIfocus.Ndevices; ++i){
        DBG("Try %s", focz[i].name);
        TRYFUNC(FLIOpen, &focdev, focz[i].name, focz[i].domain);
        if(fli_err) continue;
        TRYFUNC(FLIGetModel, focdev, focname, BUFSIZ);
        DBG("MODEL '%s'", focname);
        if(fli_err) continue;
        if(!strcasestr(focname, "focuser")){ // not a focuser?
            DBG("Not a focuser");
            TRYFUNC(FLIClose, focdev);
            continue;
        }
        if(n-- == 0){
            OK = TRUE; break;
        }
    }
    if(!OK){
        DBG("Not found");
        return FALSE;
    }
    DBG("Focuser: %s", focname);
#ifdef EBUG
    TRYFUNC(FLIGetHWRevision, focdev, &tmpl);
    if(!fli_err) DBG("HW revision: %ld", tmpl);
    TRYFUNC(FLIGetFWRevision, focdev, &tmpl);
    if(!fli_err) DBG("SW revision: %ld", tmpl);
#endif
    TRYFUNC(FLIGetStepperPosition, focdev, &focuserpos);
    TRYFUNC(FLIGetFocuserExtent, focdev, &focmaxpos);
    DBG("Curpos: %ld, maxpos: %ld", focuserpos, focmaxpos);
    return TRUE;
}

static int fli_fgetmodel(char *model, int l){
    strncpy(model, focname, l);
    return TRUE;
}

static int fli_fgett(float *t){
    if(!t) return FALSE;
    double d;
    if(FLIReadTemperature(focdev, FLI_TEMPERATURE_INTERNAL, &d)) return FALSE;
    *t = (float) d;
    return TRUE;
}

static int fli_fgetpos(float *p){
    if(!p) return FALSE;
    TRYFUNC(FLIGetStepperPosition, focdev, &focuserpos);
    if(fli_err) return FALSE;
    *p = focuserpos / FOCSCALE;
    return TRUE;
}

static int fli_fgetmaxpos(float *p){
    if(!p) return FALSE;
    *p = focmaxpos / FOCSCALE;
    return TRUE;
}

static int fli_fgetminpos(float *p){
    if(!p) return FALSE;
    *p = 0.;
    return TRUE;
}

static int fli_fhome(int async){
    if(async) TRYFUNC(FLIHomeDevice, focdev);
    else TRYFUNC(FLIHomeFocuser, focdev);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_fgoto(int async, float pos){
    long tagpos = pos * FOCSCALE;
    if(tagpos > focmaxpos) return FALSE;
    DBG("tagpos: %ld, focuserpos: %ld", tagpos, focuserpos);
    tagpos -= focuserpos;
    if(labs(tagpos) < 2) return TRUE;
    DBG("tagpos: %ld", tagpos);
    if(async) TRYFUNC(FLIStepMotorAsync, focdev, tagpos);
    else TRYFUNC(FLIStepMotor, focdev, tagpos);
    return TRUE;
}

static int fli_findWheel(){
    if(whlz) return TRUE;
    DBG("Try to find FLI filter wheels .. ");
    if(!fli_init()){
        DBG("FLI not found");
        return FALSE;
    }
    FLIwheel.Ndevices = findcams(FLIDOMAIN_USB | FLIDEVICE_FILTERWHEEL, &whlz);
    if(!FLIwheel.Ndevices){
        DBG("No wheels");
        return FALSE;
    }
    for(int i = 0; i < FLIwheel.Ndevices; i++){
        DBG("Wheel '%s', domain %s", whlz[i].name, whlz[i].dname);
    }
    return TRUE;
}

static int fli_wgetpos(int *p);

static int fli_setActiceWheel(int n){
    if(!whlz && !fli_findWheel()) return FALSE;
    if(n >= FLIwheel.Ndevices) return FALSE;
    FLIClose(whldev);
    int OK = FALSE;
    for(int i = 0; i < FLIfocus.Ndevices; ++i){
        DBG("Try %s", whlz[i].name);
        TRYFUNC(FLIOpen, &whldev, whlz[i].name, whlz[i].domain);
        if(fli_err) continue;
        TRYFUNC(FLIGetFilterCount, whldev, &filtermax);
        if(fli_err || filtermax < 2){ // not a wheel
            DBG("Not a wheel");
            TRYFUNC(FLIClose, whldev);
            continue;
        }
        if(n-- == 0){
            OK = TRUE; break;
        }
    }
    if(!OK){
        DBG("Not found");
        return FALSE;
    }
    TRYFUNC(FLIGetModel, whldev, whlname, BUFSIZ);
    DBG("Wheel: %s", whlname);
#ifdef EBUG
    TRYFUNC(FLIGetHWRevision, whldev, &tmpl);
    if(!fli_err) DBG("HW revision: %ld", tmpl);
    TRYFUNC(FLIGetFWRevision, whldev, &tmpl);
    if(!fli_err) DBG("SW revision: %ld", tmpl);
#endif
    --filtermax; // max position number
    int tmp;
    fli_wgetpos(&tmp);
    DBG("Cur position: %ld, max position: %ld", filterpos, filtermax);
    return TRUE;
}

static int fli_wgetname(char *x, int n){
    strncpy(x, whlname, n);
    return TRUE;
}

static int fli_wgetmaxpos(int *p){
    if(!p) return FALSE;
    *p = filtermax;
    return TRUE;
}

static int fli_wgetpos(int *p){
    if(!p) return FALSE;
    //TRYFUNC(FLIGetFilterPos, whldev, &filterpos); - wrong position!
    //if(fli_err){
    //    DBG("FLIGetFilterPos - ERROR!");
        TRYFUNC(FLIGetStepperPosition, whldev, &tmpl);
        if(fli_err) return FALSE;
        if(tmpl < 0) tmpl = -tmpl;
        int pos = (tmpl - WHEEL_POS0STPS+WHEEL_STEPPOS/2)/WHEEL_STEPPOS;
        DBG("pos = %d", pos);
        if(pos > -1 && pos <= filtermax) filterpos = pos;
        else return FALSE;
    //}
    *p = filterpos;
    return TRUE;
}

static int fli_wsetpos(int p){
    if(p == filterpos) return TRUE;
    if(p > filtermax || p < 0) return FALSE;
    TRYFUNC(FLISetFilterPos, whldev, (long)p);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_wgett(float *t){
    if(!t) return FALSE;
    double d;
    if(FLIReadTemperature(whldev, FLI_TEMPERATURE_INTERNAL, &d)) return FALSE;
    *t = (float) d;
    return TRUE;
}

static int fli_pollcapt(capture_status *st, float *remain){
    static int errctr = 0;
    if(capStatus == CAPTURE_READY){
        DBG("Capture ends");
        goto retn;
    }
    if(capStatus == CAPTURE_NO){ // start capture
        errctr = 0;
        DBG("Start exposition");
        TRYFUNC(FLIExposeFrame, camdev);
        if(fli_err){
            TRYFUNC(FLICancelExposure, camdev);
            if(st) *st = CAPTURE_CANTSTART;
            return FALSE;
        }
        capStatus = CAPTURE_PROCESS;
    }
    if(capStatus == CAPTURE_PROCESS){
        TRYFUNC(FLIGetExposureStatus, camdev, &tmpl);
        if(fli_err){
            if(++errctr > 3){
                if(st) *st = CAPTURE_ABORTED;
                TRYFUNC(FLICancelExposure, camdev);
                capStatus = CAPTURE_NO;
                return FALSE;
            }
            goto retn;
        }
        if(remain) *remain = tmpl/1000.;
        DBG("remained: %g", tmpl/1000.);
        if(tmpl == 0){
            if(st) *st = CAPTURE_READY;
            capStatus = CAPTURE_NO;
            return TRUE;
        }
    }else{ // some error
        if(st) *st = CAPTURE_ABORTED;
        capStatus = CAPTURE_NO;
    }
retn:
    if(st) *st = capStatus;
    return TRUE;
}

static int fli_capt(IMG *ima){
    if(!ima || !ima->data) return FALSE;
    for(int row = 0; row < ima->h; row++){
        TRYFUNC(FLIGrabRow, camdev, &ima->data[row * ima->w], ima->w);
        if(fli_err) return FALSE;
    }
    return TRUE;
}

static int fli_modelname(char *buf, int bufsz){
    strncpy(buf, camname, bufsz);
    return TRUE;
}

static int fli_setbin(int binh, int binv){
    TRYFUNC(FLISetHBin, camdev, binh);
    if(fli_err) return FALSE;
    curhbin = binh;
    TRYFUNC(FLISetVBin, camdev, binv);
    if(fli_err) return FALSE;
    curvbin = binv;
    return TRUE;
}

static int fli_getbin(int *h, int *v){
    if(h) *h = curhbin;
    if(v) *v = curvbin;
    return TRUE;
}

static int fli_setgeometry(frameformat *f){
    if(!f) return FALSE;
    TRYFUNC(FLISetImageArea, camdev, f->xoff, f->yoff,
            f->xoff + f->w/curhbin, f->yoff + f->h/curvbin);
    if(fli_err) return FALSE;
    FLIcam.geometry = *f;
    return TRUE;
}

static int fli_setnflushes(int n){
    if(n < 0) return FALSE;
    if(n){
        TRYFUNC(FLIControlBackgroundFlush, camdev, FLI_BGFLUSH_START);
        TRYFUNC(FLISetNFlushes, camdev, n);
    }
    else TRYFUNC(FLIControlBackgroundFlush, camdev, FLI_BGFLUSH_STOP);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_settemp(float t){
    TRYFUNC(FLISetTemperature, camdev, t);
    if(fli_err) return FALSE;
    return TRUE;
}

typedef enum{
    T_COLD,
    T_BODY,
    T_HOT
} temptype;

static int fli_gettemp(temptype type, float *t){
    double d;
    switch(type){
        case T_COLD:
            TRYFUNC(FLIGetTemperature, camdev, &d);
        break;
        case T_BODY:
            TRYFUNC(FLIReadTemperature, camdev, FLI_TEMPERATURE_EXTERNAL, &d);
        break;
        default:
            TRYFUNC(FLIReadTemperature, camdev, FLI_TEMPERATURE_INTERNAL, &d);
    }
    if(fli_err) return FALSE;
    *t = d;
    return TRUE;
}

static int fli_getTcold(float *t){
    return fli_gettemp(T_COLD, t);
}
static int fli_getTbody(float *t){
    return fli_gettemp(T_BODY, t);
}
static int fli_getThot(float *t){
    return fli_gettemp(T_HOT, t);
}

static void fli_cancel(){
    TRYFUNC(FLICancelExposure, camdev);
    TRYFUNC(FLIEndExposure, camdev);
}

static int fli_shutter(shutter_op cmd){
    flishutter_t shtr = FLI_SHUTTER_CLOSE;
    switch(cmd){
        case SHUTTER_OPEN:
            shtr = FLI_SHUTTER_OPEN;
        break;
        case SHUTTER_CLOSE:
        break;
        case SHUTTER_OPENATHIGH:
            shtr = FLI_SHUTTER_EXTERNAL_EXPOSURE_CONTROL|FLI_SHUTTER_EXTERNAL_TRIGGER_HIGH;
        break;
        case SHUTTER_OPENATLOW:
            shtr = FLI_SHUTTER_EXTERNAL_EXPOSURE_CONTROL|FLI_SHUTTER_EXTERNAL_TRIGGER_HIGH;
        break;
        default:
            return FALSE;
    }
    TRYFUNC(FLIControlShutter, camdev, shtr);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_confio(int io){
    TRYFUNC(FLIConfigureIOPort, camdev, io);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_getio(int *io){
    if(!io) return FALSE;
    long lio = (long)*io;
    TRYFUNC(FLIReadIOPort, camdev, &lio);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setio(int io){
    TRYFUNC(FLIWriteIOPort, camdev, (long)io);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setexp(float t){
    long e = (long)(t*1000.);
    TRYFUNC(FLISetExposureTime, camdev, e);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setframetype(int t){
    fliframe_t frametype = t ? FLI_FRAME_TYPE_NORMAL : FLI_FRAME_TYPE_DARK;
    TRYFUNC(FLISetFrameType, camdev, frametype);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setbitdepth(int i){
    flibitdepth_t depth = i ? FLI_MODE_16BIT : FLI_MODE_8BIT;
    TRYFUNC(FLISetBitDepth, camdev, depth);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setfastspeed(int fast){
    flimode_t mode = fast ? 0 : 1;
    TRYFUNC(FLISetCameraMode, camdev, mode);
    if(fli_err) return FALSE;
    return TRUE;
}

static int fli_setfanspd(fan_speed s){
    long sp = (s == FAN_OFF) ? FLI_FAN_SPEED_OFF : FLI_FAN_SPEED_ON;
    TRYFUNC(FLISetFanSpeed, camdev, sp);
    if(fli_err) return FALSE;
    return TRUE;
}

static void camt_free(cam_t **c, int n, flidev_t dev){
    if(!c || !*c) return;
    TRYFUNC(FLIClose, dev);
    for(int i = 0; i < n; ++i)
        FREE((*c)[i].name);
    FREE(*c);
}

static void fli_closecam(){
    DBG("CAMERA CLOSE");
    camt_free(&camz, FLIcam.Ndevices, camdev);
}
static void fli_closefocuser(){
    DBG("FOCUSER CLOSE");
    camt_free(&focz, FLIfocus.Ndevices, focdev);
}
static void fli_closewheel(){
    DBG("WHEEL CLOSE");
    camt_free(&whlz, FLIwheel.Ndevices, whldev);
}

static int fli_ffalse(_U_ float f){ return FALSE; }
static int fli_fpfalse(_U_ float *f){ return FALSE; }

/*
 * Global objects: camera, focuser and wheel
 */
Camera FLIcam = {
    .check = fli_findCCD,
    .setDevNo = fli_setActiceCam,
    .close = fli_closecam,
    .pollcapture = fli_pollcapt,
    .capture = fli_capt,
    .cancel = fli_cancel,

    .setbin = fli_setbin,
    .setgeometry = fli_setgeometry,
    .setnflushes = fli_setnflushes,
    .setT = fli_settemp,
    .setio = fli_setio,
    .setexp = fli_setexp,
    .setframetype = fli_setframetype,
    .setbitdepth = fli_setbitdepth,
    .setfastspeed = fli_setfastspeed,
    .setfanspeed = fli_setfanspd,
    .shuttercmd = fli_shutter,
    .confio = fli_confio,

    .getModelName = fli_modelname,
    .getbin = fli_getbin,
    .getTcold = fli_getTcold,
    .getThot = fli_getThot,
    .getTbody = fli_getTbody,
    .getio = fli_getio,
    .getgeomlimits = fli_geomlimits,

    .setbrightness = fli_ffalse,
    .setgain = fli_ffalse,
    .getmaxgain = fli_fpfalse,
};
Focuser FLIfocus = {
    .check = fli_findFocuser,
    .setDevNo = fli_setActiceFocuser,
    .close = fli_closefocuser,
    .getModelName = fli_fgetmodel,
    .getTbody = fli_fgett,
    .getPos = fli_fgetpos,
    .getMaxPos = fli_fgetmaxpos,
    .getMinPos = fli_fgetminpos,
    .home = fli_fhome,
    .setAbsPos = fli_fgoto,
};
Wheel FLIwheel = {
    .check = fli_findWheel,
    .setDevNo = fli_setActiceWheel,
    .close = fli_closewheel,
    .getModelName = fli_wgetname,
    .getMaxPos = fli_wgetmaxpos,
    .getPos = fli_wgetpos,
    .setPos = fli_wsetpos,
    .getTbody = fli_wgett,
};
