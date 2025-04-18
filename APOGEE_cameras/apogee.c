/*
 * This file is part of the CCD_Capture project.
 * Copyright 2023 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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


#include <libapogee.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <usb.h>
#include <usefull_macros.h>


#include "ccdcapture.h"
//#include "omp.h"


extern cc_Camera camera;
static int ncameras = 0;
static int isopened = FALSE;
static int osw = 0; // overscan width
static int hbin = 1, vbin = 1;
static int is16bit = 1;
static int isobject = 0;
static int maxbinv = 0, maxbinh = 0; // max binning
static char camname[BUFSIZ] = {0};
static double expt[2] = {0.}; // min/max exposition time
static double exptime = 0.; // actual exposition time
static double tstart = 0.; // exposure start time
static char whynot[BUFSIZ]; // temporary buffer for error messages
static int imW = 0, imH = 0; // size of output image
static int pid = -1, vid = -1;
static int isexposuring = 0;

static void disconnect(){
    FNAME();
    if(!isopened) return;
    ApnGlueReset();
    //ApnGlueExpAbort();
    ApnGlueClose();
    isopened = FALSE;
}

static void cancel(){
    FNAME();
    if(isexposuring){
        isexposuring = 0;
        DBG("Abort exposition");
        ApnGlueStopExposure();
        //ApnGlueExpAbort();
    }
    //uint16_t b1;
    //ApnGlueReadPixels(&b1, 1, NULL);
    ApnGlueReset();
    DBG("OK");
}

static int ndev(){
    ncameras = 1;
    if(ApnGlueOpen(ncameras)) ncameras = 0;
    else ApnGlueClose();
    DBG("Found %d cameras", ncameras);
    camera.Ndevices = ncameras;
    return ncameras;
}
/*
static void reset_usb_port(){
    if(vid < 0 || pid < 0) return;
    int fd, rc;
    char buf[FILENAME_MAX*3], *d = NULL, *f = NULL;
    struct usb_bus *bus;
    struct usb_device *dev;
    int found = 0;
    usb_init();
    usb_find_busses();
    usb_find_devices();
    for(bus = usb_busses; bus && !found; bus = bus->next) {
        for(dev = bus->devices; dev && !found; dev = dev->next) {
            if (dev->descriptor.idVendor == vid && dev->descriptor.idProduct == pid){
                found = 1;
                d = bus->dirname;
                f = dev->filename;
            }
        }
    }
    if(!found){
        ERR(_("Device not found"));
        return;
    }
    DBG("found camera device, reseting");
    snprintf(buf, sizeof(buf), "/dev/bus/usb/%s/%s", d,f);
    fd = open(buf, O_WRONLY);
    if(fd < 0){
        ERR("Can't open device file %s: %s", buf, strerror(errno));
        return;
    }
    WARNX("Resetting USB device %s", buf);
    rc = ioctl(fd, USBDEVFS_RESET, 0);
    if(rc < 0){
        perror("Error in ioctl");
        return;
    }
    close(fd);
}*/

static int setdevno(int n){
    FNAME();
    if(n > ncameras - 1) return FALSE;
    if(ApnGlueOpen(n)) return FALSE;
    //ApnGlueExpAbort();
    ApnGluePowerResume();
    ApnGlueReset();
    char *msg = ApnGlueGetInfo(&pid, &vid);
    DBG("CAMERA msg:\n%s\n", msg);
    char *f = strstr(msg, "Model: ");
    if(f){
        f += strlen("Model: ");
        char *e = strchr(f, '\n');
        size_t l = (e) ? (size_t)(e-f) : strlen(f);
        if(l >= BUFSIZ) l = BUFSIZ - 1;
        snprintf(camname, l, "%s", f);
    }
    ApnGlueGetMaxValues(expt, &camera.array.w, &camera.array.h, &osw, NULL, &maxbinh, &maxbinv, NULL, NULL);
    DBG("MAX format: W/H: %d/%d; osw=%d, binh/v=%d/%d", camera.array.w, camera.array.h, osw, maxbinh, maxbinv);
    double x, y;
    ApnGlueGetGeom(&x, &y);
    camera.pixX = x, camera.pixY = y;
    camera.field.w = camera.array.w - osw;
    camera.field.h = camera.array.h;
    camera.geometry = camera.array;
    DBG("Pixel size W/H: %g/%g; field w/h: %d/%d", x, y, camera.field.w, camera.field.h);
    ;
    return TRUE;
}

static int modelname(char *buf, int bufsz){
    strncpy(buf, camname, bufsz);
    return TRUE;
}

static int shutter(cc_shutter_op cmd){
    int op = (cmd == SHUTTER_OPEN) ? 1 : 0;
    ApnGlueOpenShutter(op);
    return TRUE;
}

static int geometrylimits(cc_frameformat *l, cc_frameformat *s){
    if(l) *l = camera.array;
    if(s) *s = (cc_frameformat){.w = 1, .h = 1, .xoff = 1, .yoff = 1};
    return TRUE;
}

static int sett(float t){
    ApnGlueSetTemp(t);
    return TRUE;
}

static int setfanspd(cc_fan_speed s){
    ApnGlueSetFan((int) s);
    return TRUE;
}

static int preflash(int n){
    if(n > 0) n = 1; else n = 0;
    ApnGluePreFlash(n);
    return TRUE;
}

// 0 - 12 bit, 1 - 16bit
static int setbitdepth(int i){
    DBG("set bit depth %d", i);
    Apn_Resolution res = (i) ? Apn_Resolution_SixteenBit : Apn_Resolution_TwelveBit;
    ApnGlueSetDatabits(res);
    is16bit = i;
    return TRUE;
}

static int getbitdepth(uint8_t *d){
    if(!d) return TRUE;
    *d = (is16bit) ? 16 : 12;
    return TRUE;
}

static int setfastspeed(int fast){
    DBG("set fast speed %d", fast);
    unsigned short spd = (fast) ? AdcSpeed_Fast : AdcSpeed_Normal;
    ApnGlueSetSpeed(spd);
    return TRUE;
}

static int setgeometry(cc_frameformat *f){
    if(!f) return FALSE;
    if(f->xoff > camera.field.w - 1) f->xoff = camera.field.w - 1;
    if(f->yoff > camera.field.h - 1) f->yoff = camera.field.h - 1;
    if(f->w - f->xoff > camera.array.w) f->w = camera.array.w - f->xoff;
    if(f->h - f->yoff > camera.array.h) f->h = camera.array.h - f->yoff;
    int ow = (f->w > camera.field.w) ? f->w - camera.field.w: 0;
    if(ow < 0) ow = 0;
    if(f->xoff && f->w + f->xoff >= camera.field.w){
        f->w = camera.field.w - f->xoff;
        ow = 0;
    }
    //f->w -= ow;
    DBG("f->w=%d, camfw=%d, camaw=%d", f->w, camera.field.w, camera.array.w);
    DBG("ow=%d", ow);
    if(ApnGlueSetExpGeom(f->w, f->h, ow, 0, hbin, vbin,
                        f->xoff, f->yoff, &imW, &imH, whynot)){
        WARNX("Can't set geometry: %s", whynot);
        imW = f->w;
        imH = f->h;
    }else{
        camera.geometry = *f;
    }
    DBG("ApnGlueSetExpGeom(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)",
        f->w, f->h, ow, 0, hbin, vbin, f->xoff, f->yoff, imW, imH);
    return TRUE;
}

static int setbin(int binh, int binv){
    DBG("set bin v/h: %d/%d", binv, binh);
    if(binh > maxbinh || binv > maxbinv) return FALSE;
    hbin = binh; vbin = binv;
    return TRUE;
}

static int tcold(float *t){
    if(!t) return FALSE;
    double dt;
    ApnGlueGetTemp(&dt);
    *t = dt;
    return TRUE;
}

static int thot(float *t){
    if(t) *t = ApnGlueGetHotTemp();
    return TRUE;
}

static int startexp(){
    tstart = sl_dtime();
    DBG("Start exposition %g seconds (isobject=%d)", exptime, isobject);
    CCDerr r = ApnGlueStartExp(&exptime, isobject);
    if(ALTA_OK != r){
        /*reset_usb_port();
        r = ApnGlueStartExp(&exptime, isobject);
        if(ALTA_OK != r){*/
            //uint16_t b[4096];
            //ApnGlueReadPixels(b, 4096, NULL);
            ApnGlueReset();
            DBG("Error starting exp: %d", (int)r);
            return FALSE;
        //}
    }
    isexposuring = 1;
    return TRUE;
}

static int frametype(int islight){
    DBG("set frame type %d", islight);
    isobject = islight;
    return TRUE;
}

static int setexp(float t){
    DBG("set exp %g, min: %g, max: %g", t, expt[0], expt[1]);
    if(t < expt[0] || t > expt[1]) return FALSE; // too big or too small exptime
    exptime = t;
    return TRUE;
}

static int getbin(int *h, int *v){
    DBG("get bin v/h: %d/%d", vbin, hbin);
    if(h) *h = hbin;
    if(v) *v = vbin;
    return TRUE;
}

static int pollcapt(cc_capture_status *st, float *remain){
    if(st) *st = CAPTURE_PROCESS;
    if(ApnGlueExpDone()){
        if(st) *st = CAPTURE_READY;
        isexposuring = 0;
        DBG("Capture ready");
        if(remain) *remain = 0.f;
        return TRUE;
    }else DBG("Capture in process");
    double d = exptime - (sl_dtime() - tstart);
    DBG("Poll capture, tremain=%g", d);
    if(d < -5.){ // capture error?
        WARNX("Abort capture");
        ApnGlueExpAbort();
        //ApnGlueStopExposure();
        DBG("ABORTED!");
        if(*st) *st = CAPTURE_ABORTED;
        return FALSE;
    }
    if(d < 0.) d = 0.1;
    DBG("Poll capture, tremain=%g", d);
    if(remain) *remain = d;
    return TRUE;
}

static int capture(cc_IMG *ima){
    FNAME();
    if(!ima || !ima->data) return FALSE;
    DBG("ApnGlueReadPixels(%dx%d=%d)", imW, imH, imW * imH);
    if(ApnGlueReadPixels((uint16_t*)ima->data, imW * imH, whynot)){
        WARNX("Can't read image: %s", whynot);
        return FALSE;
    }
    ima->bitpix = is16bit ? 16 : 12;
    return TRUE;
}


static int ffalse(_U_ float f){ return FALSE; }
static int fpfalse(_U_ float *f){ return FALSE; }
static int ifalse(_U_ int i){ return FALSE; }
//static int vtrue(){ return TRUE; }
static int ipfalse(_U_ int *i){ return FALSE; }

/*
 * Global objects: camera, focuser and wheel
 */
cc_Camera camera = {
    .check = ndev,
    .close = disconnect,
    .pollcapture = pollcapt,
    .capture = capture,
    .cancel = cancel,
    .startexposition = startexp,
    // setters:
    .setDevNo = setdevno,
    .setbrightness = ffalse,
    .setexp = setexp,
    .setgain = ffalse,
    .setT = sett,
    .setbin = setbin,
    .setnflushes = preflash,
    .shuttercmd = shutter,
    .confio = ifalse,
    .setio = ifalse,
    .setframetype = frametype, // set DARK or NORMAL: no shutter -> no darks
    .setbitdepth = setbitdepth,
    .setfastspeed = setfastspeed,
    .setgeometry = setgeometry,
    .setfanspeed = setfanspd,
    // getters:
    .getbrightness = fpfalse,
    .getbitpix = getbitdepth,
    .getModelName = modelname,
    .getgain = fpfalse,
    .getmaxgain = fpfalse,
    .getgeomlimits = geometrylimits,
    .getTcold = tcold,
    .getThot = thot,
    .getTbody = fpfalse,
    .getbin = getbin,
    .getio = ipfalse,
};
