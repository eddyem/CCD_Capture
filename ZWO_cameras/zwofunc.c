/*
 * This file is part of the CCD_Capture project.
 * Copyright 2022 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <usefull_macros.h>
#include <ASICamera2.h>

#include "ccdcapture.h"

extern cc_Camera camera;
extern cc_Focuser focuser;
extern cc_Wheel wheel;

// remove all these after removing stubs!
// VVV
static const int filtermax = 5;
static const float focmaxpos = 10.;
static int filterpos = 0;
static float focuserpos = 1.;
// AAA

static int curbin = 1;
static ASI_BOOL isdark = ASI_FALSE;
static struct{
    float maxgain;
    float mingain;
    float maxbright;
    float minbright;
    int maxbin;
} extrvalues = {0}; // extremal values

static double starttime = 0.;   // time when exposure started
static float exptime = 0.;      // exposition time

static ASI_CAMERA_INFO caminfo = {0};

// setters and getters of some parameters
static int zwo_setfloat(float f, ASI_CONTROL_TYPE t){
    DBG("Try to set float %f, type %d", f, t);
    long val = (long) f;
    if(ASI_SUCCESS != ASISetControlValue(caminfo.CameraID, t, val, ASI_FALSE)){
        DBG("FAILED");
        return FALSE;
    }
    return TRUE;
}
static int zwo_getfloat(float *f, ASI_CONTROL_TYPE t){
    if(!f) return FALSE;
    long val; ASI_BOOL aut = ASI_FALSE;
    if(ASI_SUCCESS != ASIGetControlValue(caminfo.CameraID, t, &val, &aut))
        return FALSE;
    if(aut) DBG("VALUE IS AUTO!!!");
    *f = (float) val;
    return TRUE;
}

static int asi_checkcam(){
    camera.Ndevices = ASIGetNumOfConnectedCameras();
    DBG("found %d ZWO's", camera.Ndevices);
    if(camera.Ndevices) return TRUE;
    return FALSE;
}

static int campoll(cc_capture_status *st, float *remain){
    if(!st) return FALSE;
    ASI_EXPOSURE_STATUS s;
    if(ASI_SUCCESS != ASIGetExpStatus(caminfo.CameraID, &s)){
        DBG("Can't get exp status");
        return FALSE;
    }
    switch(s){
        case ASI_EXP_IDLE:
            *st = CAPTURE_NO;
        break;
        case ASI_EXP_WORKING:
            *st = CAPTURE_PROCESS;
        break;
        case ASI_EXP_SUCCESS:
            *st = CAPTURE_READY;
        break;
        default: // failed
            DBG("Failed: %d", s);
            //*st = CAPTURE_ABORTED;
             *st = CAPTURE_READY;
    }
    if(remain){
        float diff = exptime - (sl_dtime() - starttime);
        if(diff < 0.) diff = 0.;
        *remain = diff;
    }
    return TRUE;
}

static int camcapt(cc_IMG *ima){
    if(!ima || !ima->data) return FALSE;
    unsigned char *d = (unsigned char *)ima->data;
    long image_size = ima->h * ima->w * 2;
    if(ASI_SUCCESS != ASIGetDataAfterExp(caminfo.CameraID, d, image_size)){
        WARNX("Couldn't read exposure data\n");
        return FALSE;
    }
    ima->bitpix = 16;
    return TRUE;
}

static void camcancel(){
    ASI_EXPOSURE_STATUS s;
    if(ASI_SUCCESS == ASIGetExpStatus(caminfo.CameraID, &s) && s == ASI_EXP_WORKING){
        ASIStopExposure(caminfo.CameraID);
    }
}

static int setframetype(int l){
    if(l) isdark = ASI_FALSE;
    else isdark = ASI_TRUE;
    return TRUE;
}

static int startcapt(){
    camcancel();
    //red("ISDARK = %s\n", isdark ? "true" : "false");
    if(ASI_SUCCESS == ASIStartExposure(caminfo.CameraID, isdark)){
        starttime = sl_dtime();
        return TRUE;
    }
    return FALSE;
}

static void asi_closecam(){
    FNAME();
    if(caminfo.CameraID){
        camcancel();
        ASICloseCamera(caminfo.CameraID);
        caminfo.CameraID = 0;
    }
}

static int setdevno(int n){
    if(n > camera.Ndevices - 1 || n < 0) return FALSE;
    asi_closecam();
    if(ASI_SUCCESS != ASIGetCameraProperty(&caminfo, n)) return FALSE;
    DBG("cc_Camera #%d, name: %s, ID: %d", n, caminfo.Name, caminfo.CameraID);
    DBG("WxH: %ldx%ld, %s", caminfo.MaxWidth, caminfo.MaxHeight, caminfo.IsColorCam == ASI_TRUE ? "color" : "monochrome");
    DBG("Pixel size: %1.1f mkm; gain: %1.2f e/ADU", caminfo.PixelSize, caminfo.ElecPerADU);
    int *sup = caminfo.SupportedBins;
    while(*sup){
        extrvalues.maxbin = *sup++;
    }
    camera.pixX = camera.pixY = (float)caminfo.PixelSize / 1e6; // um -> m
    camera.array = (cc_frameformat){.w = caminfo.MaxWidth, .h = caminfo.MaxHeight, .xoff = 0, .yoff = 0};
    camera.field = camera.array; // initial setup (will update later)
    if(ASI_SUCCESS != ASIOpenCamera(caminfo.CameraID)){
        WARNX("Can't open device for camera %s", caminfo.Name);
        return FALSE;
    }
    if(ASI_SUCCESS != ASIInitCamera(caminfo.CameraID)){
        WARNX("Can't init device for camera %s", caminfo.Name);
        asi_closecam();
        return FALSE;
    }
    // get binning
    int imtype;
    if(ASI_SUCCESS == ASIGetROIFormat(caminfo.CameraID, &camera.field.w, &camera.field.h, &curbin, &imtype)){
        ASIGetStartPos(caminfo.CameraID, &camera.field.xoff, &camera.field.yoff);
        DBG("FIELD: %dx%d, offset %dx%d, binning %d", camera.field.w, camera.field.h, camera.field.xoff, camera.field.yoff, curbin);
    }
    int ncontrols = 0;
    if(ASI_SUCCESS == ASIGetNumOfControls(caminfo.CameraID, &ncontrols)){
        ASI_CONTROL_CAPS asicon;
        for(int i = 0; i < ncontrols; ++i){
            if(ASI_SUCCESS == ASIGetControlCaps(caminfo.CameraID, i, &asicon)){
#ifdef EBUG
                green("Control #%d: '%s' ", i, asicon.Name);
#endif
                long val;
                ASI_BOOL aut;
                if(ASI_SUCCESS == ASIGetControlValue(caminfo.CameraID, asicon.ControlType, &val, &aut)){
#ifdef EBUG
                    printf("curval: %ld%s, ", val, aut ? " (auto)": "");
#endif
                    switch(asicon.ControlType){ // get extremal values of brightness and gain
                        case ASI_GAIN:
                            extrvalues.maxgain = (float) asicon.MaxValue;
                            extrvalues.mingain = (float) asicon.MinValue;
                        break;
                        case ASI_BRIGHTNESS:
                            extrvalues.maxbright = (float) asicon.MaxValue;
                            extrvalues.minbright = (float) asicon.MinValue;
                        break;
                        default:
                        break;
                    }
                }
#ifdef EBUG
                printf("min/max: %ld/%ld; def: %ld, writeable: %d, descr: %s\n",
                       asicon.MinValue, asicon.MaxValue, asicon.DefaultValue, asicon.IsWritable,
                       asicon.Description);
#endif
            }
        }
        //red("Bit: %d\n", caminfo.BitDepth);
        //caminfo.BitDepth = 10;
        //red("Bit: %d\n", caminfo.BitDepth);
    }
    return TRUE;
}

static int camsetbrig(float b){
    if(b < extrvalues.minbright || b > extrvalues.maxbright){
        WARNX(_("Brightness should be from %g to %g"), extrvalues.minbright, extrvalues.maxbright);
        return FALSE;
    }
    return zwo_setfloat(b, ASI_BRIGHTNESS);
}

static int camgetbrig(float *b){
    if(!b) return FALSE;
    return zwo_getfloat(b, ASI_BRIGHTNESS);
}

static int camsetexp(float t){
    if(!zwo_setfloat(t*1e6, ASI_EXPOSURE)) return FALSE;
    exptime = t;
    return TRUE;
}

static int camsetgain(float g){
    if(g < extrvalues.mingain || g > extrvalues.maxgain){
        WARNX(_("Gain should be from %g to %g"), extrvalues.mingain, extrvalues.maxgain);
        return FALSE;
    }
    return zwo_setfloat(g, ASI_GAIN);
}

static int camgetgain(float *g){
    if(!g) return FALSE;
    return zwo_getfloat(g, ASI_GAIN);
}

static int camsett(float t){
    if(caminfo.IsCoolerCam == ASI_FALSE){
        DBG("Cooling unsupported");
        return FALSE;
    }
    if(!zwo_setfloat(1., ASI_FAN_ON)){
        DBG("Can't set fan on");
        return FALSE;
    }
    float f;
    if(zwo_getfloat(&f, ASI_FAN_ON)){
        DBG("FAN: %g", f);
    }
    if(!zwo_setfloat(1., ASI_COOLER_ON)){
        DBG("Can't set cooler on");
        return FALSE;
    }
    if(!zwo_setfloat(t, ASI_TARGET_TEMP)){
        DBG("Can't set target temperature");
        return FALSE;
    }
    if(zwo_getfloat(&f, ASI_TARGET_TEMP)){
        DBG("Ttarg = %g", f);
    }
    if(!zwo_getfloat(&f, ASI_COOLER_ON)) return FALSE;
    DBG("COOLERON = %g", f);
    /*
#ifdef EBUG
    double t0 = dtime();
    float c, p, tn;
    while(dtime() - t0 < 1200.){
        usleep(100000); // without this first ASI_FAN_ON will show false data
        green("%.1f ", dtime()-t0);
        zwo_getfloat(&f, ASI_FAN_ON);
        zwo_getfloat(&t, ASI_TARGET_TEMP);
        zwo_getfloat(&c, ASI_COOLER_ON);
        zwo_getfloat(&tn, ASI_TEMPERATURE);
        zwo_getfloat(&p, ASI_COOLER_POWER_PERC);
        printf("fan: %g, t: %g, cooler: %g, perc: %g, tnow: %g\n", f, t, c, p, tn/10.);
    }
#endif
    */
    return TRUE;
}

static int camgett(float *t){
    if(!t) return FALSE;
    float curt;
    if(!zwo_getfloat(&curt, ASI_TEMPERATURE)) return FALSE;
    *t = curt / 10.;
    return TRUE;
}
// get  Tbody & Thot unsupported
static int gett(_U_ float *t){
    return FALSE;
}

static int camsetbin(int h, int v){
    DBG("set bin %dx%d", h, v);
    if(h != v){
        WARNX(_("BinX and BinY should be equal, take h"));
        //return FALSE;
    }
    if(h > extrvalues.maxbin){
        WARNX(_("Maximal binning value is %d"), extrvalues.maxbin);
    }
    if(zwo_setfloat(1., ASI_HARDWARE_BIN)){
        curbin = h;
        return TRUE;
    }
    return FALSE;
}

// unsupported, but return TRUE if have mechanical shutter
static int camshutter(_U_ cc_shutter_op s){
    if(!caminfo.MechanicalShutter) return FALSE;
    return TRUE;
}

// set ROI
static int camsetgeom(cc_frameformat *f){ // w,h, xoff, yoff
    if(!f) return FALSE;
    int imtype;
    DBG("w: %g, h: %g, bin: %d", (double)f->w / curbin, (double)f->h / curbin, curbin);
    if(ASI_SUCCESS != ASISetROIFormat(caminfo.CameraID, f->w/curbin, f->h/curbin, curbin, ASI_IMG_RAW16)){
        DBG(_("Can't set geometry"));
        return FALSE;
    }
    if(ASI_SUCCESS != ASIGetROIFormat(caminfo.CameraID, &f->w, &f->h, &curbin, &imtype)){
        DBG(_("Can't get geometry"));
        return FALSE;
    }
    DBG("curformat: w=%d, h=%d, bin=%d", f->w, f->h, curbin);
    DBG("w=%d, h=%d, bin=%d", f->w, f->h, curbin);
    if(ASI_SUCCESS != ASISetStartPos(caminfo.CameraID, f->xoff/curbin, f->yoff/curbin)){
        DBG("Can't set start pos");
        return FALSE;
    }
    if(ASI_SUCCESS != ASIGetStartPos(caminfo.CameraID, &f->xoff, &f->yoff)){
        DBG("Can't get start pos");
        return FALSE;
    }
    DBG("curstartpos: x=%d, y=%d", f->xoff, f->yoff);
    camera.geometry = *f;
    return TRUE;
}

static int camgetnam(char *n, int l){
    strncpy(n, caminfo.Name, l);
    return TRUE;
}

static int camgmg(float *mg){ // get max gain
    if(mg) *mg = extrvalues.maxgain;
    return TRUE;
}

static int camggl(cc_frameformat *max, cc_frameformat *step){ // get geometry limits
    DBG("array: %dx%d, off: %dx%d", camera.array.w, camera.array.h, camera.array.xoff, camera.array.yoff);
    if(max) *max = camera.array;
    if(step) *step = (cc_frameformat){1,1,1,1};
    return TRUE;
}

static int camgetbin(int *binh, int *binv){
    if(binh) *binh = curbin;
    if(binv) *binv = curbin;
    return TRUE;
}

static int setfspd(int spd){ // set fast speed (0..3): 0 - 40% bandwidthovrl, 3 - 100%
    float bw = 40.;
    if(spd > 2) bw = 100.;
    else if(spd > 0) bw += 20. * spd;
    DBG("set BANDWIDTH to %g", bw);
    zwo_setfloat(1, ASI_HIGH_SPEED_MODE);
    if(ASI_SUCCESS != zwo_setfloat(bw, ASI_BANDWIDTHOVERLOAD)){
        DBG("Can't set");
        return FALSE;
    }
    return TRUE;
}

static int camgetio(_U_ int *io){ // not supported
    return FALSE;
}

static int camfan(_U_ cc_fan_speed spd){ // not supported, just turn it on/off
    switch(spd){
        case FAN_OFF:
            if(!zwo_setfloat(0., ASI_FAN_ON)){
                DBG("Can't set fan off");
                return FALSE;
            }
        break;
        default: // turn ON
            if(!zwo_setfloat(1., ASI_FAN_ON)){
                DBG("Can't set fan on");
                return FALSE;
            }
    }
    return TRUE;
}

/* --------------- FOCUSER --------------- */
static int focsetpos(_U_ int a, float n){
    if(n < 0. || n > focmaxpos) return FALSE;
    focuserpos = n;
    return TRUE;
}

static int fochome(_U_ int a){
    focuserpos = 0.;
    return TRUE;
}

static int focgetnam(char *n, int l){
    strncpy(n, "Dummy focuser", l);
    return TRUE;
}

static int focpos(float *p){
    if(p) *p = focuserpos;
    return TRUE;
}

static int focMp(float *p){
    if(p) *p = focmaxpos;
    return TRUE;
}

static int focmp(float *p){
    if(p) *p = 0.;
    return TRUE;
}

/* --------------- WHEEL --------------- */
static int whlsetpos(int n){
    if(n > filtermax || n < 0) return FALSE;
    filterpos = n;
    return TRUE;
}

static int whlgetpos(int *n){
    if(n) *n = filterpos;
    return TRUE;
}

static int whlgmp(int *n){
    if(n) *n = filtermax;
    return TRUE;
}

static int whlgetnam(char *n, int l){
    strncpy(n, "Dummy filter wheel", l);
    return TRUE;
}

static int stub(){
    return FALSE;
}
static void vstub(){
    FNAME();
    return;
}
static int istub(_U_ int N){
    return FALSE;
}

/*
 * Global objects: camera, focuser and wheel
 */
__attribute__ ((visibility("default"))) cc_Camera camera = {
    .check = asi_checkcam,
    .close = asi_closecam,
    .pollcapture = campoll,
    .capture = camcapt,
    .cancel = camcancel,
    .startexposition = startcapt,
    // setters:
    .setDevNo = setdevno,
    .setbrightness = camsetbrig,
    .setexp = camsetexp,
    .setgain = camsetgain,
    .setT = camsett,
    .setbin = camsetbin,
    .setnflushes = istub,
    .shuttercmd = camshutter,
    .confio = istub,
    .setio = istub,
    .setframetype = setframetype,
    .setbitdepth = istub, // there's no ways in documentation to SET bit depth
    .setfastspeed = setfspd,
    .setgeometry = camsetgeom,
    .setfanspeed = camfan,
    // getters:
    .getbrightness = camgetbrig,
    .getModelName = camgetnam,
    .getgain = camgetgain,
    .getmaxgain = camgmg,
    .getgeomlimits = camggl,
    .getTcold = camgett,
    .getThot = gett,
    .getTbody = gett,
    .getbin = camgetbin,
    .getio = camgetio,
};

__attribute__ ((visibility("default"))) cc_Focuser focuser = {
    .check = stub,
    .close = vstub,
    // setters:
    .setDevNo = setdevno,
    .setAbsPos = focsetpos,
    .home = fochome,
    // getters:
    .getModelName = focgetnam,
    .getTbody = gett,
    .getPos = focpos,
    .getMaxPos = focMp,
    .getMinPos = focmp,
};

__attribute__ ((visibility("default"))) cc_Wheel wheel = {
    .check = stub,
    .close = vstub,
    // setters
    .setDevNo = setdevno,
    .setPos = whlsetpos,
    // getters
    .getModelName = whlgetnam,
    .getTbody = gett,
    .getPos = whlgetpos,
    .getMaxPos = whlgmp,
};
