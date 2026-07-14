/*
 * This file is part of the CCD_Capture project.
 * Copyright 2026 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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

#include <float.h>
#include <pthread.h>
#include <string.h>
#define TOUPCAM_HRESULT_ERRORCODE_NEEDED
#include <toupcam.h>
#include <usefull_macros.h>

#include "ccdcapture.h"

extern cc_Camera camera;
extern cc_Focuser focuser;
extern cc_Wheel wheel;

// flags for image processing
typedef enum{
    IM_SLEEP,
    IM_STARTED,
    IM_READY,
    IM_ERROR
} imstate_t;

// devices
static ToupcamDeviceV2 g_dev[TOUPCAM_MAX] = {0};
static struct{
    ToupcamDeviceV2* dev;       // device
    HToupcam hcam;              // hcam for all functions
    unsigned long long flags;   // flags (read on connect)
    pthread_mutex_t mutex;      // lock mutex for `data` writing/reading
    void* data;                 // image data
    size_t imsz;                // size of current image in bytes
    imstate_t state;            // current state
    uint64_t imseqno;           // number of image from connection
    uint64_t lastcapno;         // last captured image number
    uint8_t bytepix;            // bytes per pixel
    int curbin;                 // current binning
} toupcam = {0};

static int camgetbp(uint8_t *bp);

// exptime and starting of exposition
static double exptime = 0., starttime = 0.;

#define TCHECK()    do{if(!toupcam.hcam) return FALSE;}while(0)

// return constant string with error code description
static const char *errcode(int ecode){
    switch(ecode){
#ifndef STR
#define STR(par)  # par
#endif
#define ECASE(code)  case code: return STR(code)
        ECASE(S_OK);
        ECASE(S_FALSE);
        ECASE(E_UNEXPECTED);
        ECASE(E_NOTIMPL);
        ECASE(E_NOINTERFACE);
        ECASE(E_ACCESSDENIED);
        ECASE(E_OUTOFMEMORY);
        ECASE(E_INVALIDARG);
        ECASE(E_POINTER);
        ECASE(E_FAIL);
        ECASE(E_WRONG_THREAD);
        ECASE(E_GEN_FAILURE);
        ECASE(E_BUSY);
        ECASE(E_PENDING);
        ECASE(E_TIMEOUT);
        ECASE(E_UNREACH);
#undef ECASE
#if 0
        case 0: return "S_OK";
        case 1: return "S_FALSE";
        case 0x8000ffff: return "E_UNEXPECTED";
        case 0x80004001: return "E_NOTIMPL";
        case 0x80004002: return "E_NOINTERFACE";
        case 0x80070005: return "E_ACCESSDENIED";
        case 0x8007000e: return "E_OUTOFMEMORY";
        case 0x80070057: return "E_INVALIDARG";
        case 0x80004003: return "E_POINTER";
        case 0x80004005: return "E_FAIL";
        case 0x8001010e: return "E_WRONG_THREAD";
        case 0x8007001f: return "E_GEN_FAILURE";
        case 0x800700aa: return "E_BUSY";
        case 0x8000000a: return "E_PENDING";
        case 0x8001011f: return "E_TIMEOUT";
        case 0x80072743: return "E_UNREACH";
#endif
        default: return "Unknown error";
    }
}

/**
 * @brief camcancel - camera.cancel - cancel exposition
 */
static void camcancel(){
    if(!toupcam.hcam) return;
    int e = Toupcam_Trigger(toupcam.hcam, 0); // stop triggering
    if(e < 0 && e != E_UNEXPECTED) WARNX(_("Can't stop image triggering: %s"), errcode(e));
    e = Toupcam_Stop(toupcam.hcam);
    if(e < 0) WARNX("Can't stop: %s", errcode(e));
    toupcam.state = IM_SLEEP;
}

// close camera device
static void camclose(){
    camcancel();
    if(toupcam.hcam){
        DBG("Close camera");
        Toupcam_Close(toupcam.hcam);
        toupcam.hcam = NULL;
    }
    if(toupcam.data){
        DBG("Free image data");
        free(toupcam.data);
        toupcam.data = NULL;
    }
}

/**
 * @brief initcam - init camera
 * @return FALSE if failed
 */
static int initcam(){
    camclose();
    unsigned N = Toupcam_EnumV2(g_dev);
    if(0 == N){
        DBG("Found 0 toupcams");
        return FALSE;
    }
    camera.Ndevices = (int) N;
    return TRUE;
}

/**
 * @brief setdevno - camera.setDevNo - set active device number
 * @param n - device no
 * @return FALSE if no such device or failed
 */
static int setdevno(int n){
    if(n < 0 || n >= camera.Ndevices) return FALSE;
    camclose(); // close if was opened
    toupcam.dev = &g_dev[n];
    toupcam.hcam = Toupcam_Open(g_dev[n].id);
    if(!toupcam.hcam){
        DBG("Can't open %dth camera", n);
        return FALSE;
    }
    DBG("Opened %s", toupcam.dev->displayname);
    DBG("Clear ROI");
    Toupcam_put_Roi(toupcam.hcam, 0, 0, 0, 0); // clear ROI
    // now fill camera geometry
    DBG("Get geometry");
    Toupcam_get_Size(toupcam.hcam, &camera.geometry.w, &camera.geometry.h);
    DBG("size (wxh): %dx%d", camera.geometry.w, camera.geometry.h);
    unsigned int xoff, yoff, h, w;
    DBG("Get ROI");
    Toupcam_get_Roi(toupcam.hcam, &xoff, &yoff, &w, &h);
    DBG("off (x/y): %d/%d; wxh: %dx%d", xoff, yoff, w, h);
    camera.array.xoff = camera.field.xoff = xoff;
    camera.array.yoff = camera.field.yoff = yoff;
    camera.array.w = camera.field.w = w;
    camera.array.h = camera.field.h = h;
    DBG("Get pixel size");
    Toupcam_get_PixelSize(toupcam.hcam, 0, &camera.pixX, &camera.pixY);
    DBG("pixsize (x/y): %g/%g", camera.pixX, camera.pixY);
    toupcam.flags = Toupcam_query_Model(toupcam.hcam)->flag;
    DBG("flags: 0x%llx", toupcam.flags);
    DBG("Allocate data (%d bytes: 2*%d*%d)", 2 * camera.array.w * camera.array.h, camera.array.w, camera.array.h);
    toupcam.data = calloc(camera.array.w * camera.array.h, 2);
#define OPT(opt, val, comment)  do{DBG(comment); if(Toupcam_put_Option(toupcam.hcam, opt, val) < 0){ DBG("Can't put this option"); }}while(0)
    // 12 frames/sec
    OPT(TOUPCAM_OPTION_TRIGGER, 1, "Software/simulated trigger mode");
    OPT(TOUPCAM_OPTION_RAW, 1, "Put to RAW mode");
#undef OPT
    toupcam.state = IM_SLEEP;
    toupcam.imseqno = toupcam.lastcapno = 0;
    uint8_t bp;
    if(!camera.getbitpix(&bp)) toupcam.bytepix = 2;
    else toupcam.bytepix = (bp+7)/8;
    if(!camera.getbin(NULL, NULL)) toupcam.curbin = 1;
    pthread_mutex_init(&toupcam.mutex, NULL);
    return TRUE;
}

/**
 * @brief campoll - camera.pollcapture - polling of capture process status
 * @param st (o) - status of capture process
 * @param remain (o) - time remain (s)
 * @return FALSE if error (exp aborted), TRUE while no errors
 */
static int campoll(cc_capture_status *st, float *remain){
    TCHECK();
    cc_capture_status curst;
    double tremain = 0.f;
    int ret = FALSE;
    switch(toupcam.state){
        case IM_SLEEP:
            curst = CAPTURE_NO;
            break;
        case IM_ERROR:
            curst = CAPTURE_ABORTED;
            break;
        case IM_READY:
            curst = CAPTURE_READY;
            ret = TRUE;
            break;
        default: // IM_PROCESS
            curst = CAPTURE_PROCESS;
            //DBG("exptime: %g, d-s: %g", exptime, sl_dtime() - starttime);
            tremain = exptime - (sl_dtime() - starttime);
            if(tremain < -2.0) curst = CAPTURE_ABORTED;
            else{
                if(tremain < 0.) tremain = 0.;
                ret = TRUE;
            }
    }
    //DBG("curst: %d, tremain: %g", curst, tremain);
    if(st) *st = curst;
    if(remain) *remain = (float)tremain;
    return ret;
}

// callback of image ready event
static void EventCallback(unsigned nEvent, void _U_ *pCallbackCtx){
    DBG("CALLBACK with evt %d", nEvent);
    if(!toupcam.hcam || !toupcam.data){ DBG("NO data!"); return; }
    if(nEvent != TOUPCAM_EVENT_IMAGE){ DBG("Not image event"); return; }
    ToupcamFrameInfoV4 info = {0};
    //DBG("LOCK");
    pthread_mutex_lock(&toupcam.mutex);
    if(Toupcam_PullImageV4(toupcam.hcam, toupcam.data, 0, 0, 0, &info) < 0){
        DBG("Error pulling image");
        toupcam.state = IM_ERROR;
    }else{
        ++toupcam.imseqno;
        DBG("Image %lu (%dx%d) ready!", toupcam.imseqno, info.v3.width, info.v3.height);
        toupcam.state = IM_READY;
        toupcam.imsz = info.v3.height * info.v3.width * toupcam.bytepix;
        // geometry.h/w WITHOUT binning!
        camera.geometry.h = info.v3.height * toupcam.curbin;
        camera.geometry.w = info.v3.width * toupcam.curbin;
        if(toupcam.imseqno - toupcam.lastcapno > 5){
            WARNX("5 uncaptured images -> cancel");
            camera.cancel();
            toupcam.imseqno = toupcam.lastcapno;
        }
    }
    pthread_mutex_unlock(&toupcam.mutex);
    //DBG("UNLOCK");
}

/**
 * @brief startexp - camera.startexposition - start exp if can
 * @return FALSE if failed
 */
static int startexp(){
    TCHECK();
    if(toupcam.state == IM_SLEEP){
        if(Toupcam_StartPullModeWithCallback(toupcam.hcam, EventCallback, NULL) < 0){
            WARNX("Can't run PullMode with Callback!");
            return FALSE;
        }
    }
    // Ask to trigger for several images (maximal speed available)
    int e = Toupcam_Trigger(toupcam.hcam, 100);
    if(e < 0){
        DBG("Can't ask for images stream: %s; try 1", errcode(e));
        e = Toupcam_Trigger(toupcam.hcam, 1);
        if(e < 0){
            WARNX("Can't ask for next image: %s", errcode(e));
            return FALSE;
        }
    }
    toupcam.state = IM_STARTED;
    starttime = sl_dtime();
    return TRUE;
}

/**
 * @brief camcapt - camera.capture - capture an image, struct `ima` should be prepared before
 * @param ima (o) - captured image
 * @return FALSE if failed or bad `ima`
 */
static int camcapt(cc_IMG *ima){
    TCHECK();
    if(!ima || !ima->data || !toupcam.data) return FALSE;
    //DBG("LOCK");
    pthread_mutex_lock(&toupcam.mutex);
    size_t fullsz = ima->w * ima->h * toupcam.bytepix;
    if(toupcam.imsz != fullsz){
        if(toupcam.imsz < fullsz) fullsz = toupcam.imsz;
        int realw = camera.geometry.w / toupcam.curbin;
        if(ima->w != realw) ima->w = realw;
        ima->h = fullsz / realw / toupcam.bytepix;
        WARNX("Asked image size (%zd) not equal real (%zd); set w=%d, h=%d!",
              ima->w * ima->h * toupcam.bytepix, toupcam.imsz, ima->w, ima->h);
    }
    memcpy(ima->data, toupcam.data, fullsz);
    ima->bitpix = toupcam.bytepix * 8;
    toupcam.lastcapno = toupcam.imseqno;
    pthread_mutex_unlock(&toupcam.mutex);
    //DBG("UNLOCK");
    return TRUE;
}

/**
 * @brief camsetbit - camera.setbitdepth
 * @param b - bit depth, 1 - high (16 bit), 0 - low (8 or other bit)
 * @return FALSE if failed
 */
static int camsetbit(int b){
    TCHECK();
    DBG("set bitdepth %d", b);
    if(Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_BITDEPTH, b) < 0) return FALSE;
    DBG("OK");
    int opt = (b) ? TOUPCAM_PIXELFORMAT_RAW16 : TOUPCAM_PIXELFORMAT_RAW8;
    DBG("set pixel format %d", opt);
    if(Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_PIXEL_FORMAT, opt) < 0) return FALSE;
    DBG("OK");
    toupcam.bytepix = (b) ? 2 : 1;
    return TRUE;
}

/**
 * @brief camgetbp - camera.getbitpix - get bit depth in bits per pixel (8, 12, 16 etc)
 * @param bp (o) - bits per pixel
 * @return FALSE if failed
 */
static int camgetbp(uint8_t *bp){
    TCHECK();
    int b;
    DBG("Get bitdepth");
    if(Toupcam_get_Option(toupcam.hcam, TOUPCAM_OPTION_BITDEPTH, &b) < 0) return FALSE;
    DBG("bitdepth=%d", b);
    if(bp){
        if(b == 0) *bp = 8;
        else{
            DBG("Get pixformat");
            if(Toupcam_get_Option(toupcam.hcam, TOUPCAM_OPTION_PIXEL_FORMAT, &b) < 0) return FALSE;
            DBG("pixformat=%d", b);
            *bp = 8 + b*2;
        }
    }
    return TRUE;
}

/**
 * @brief camsetbrig - camera.setbrightness - set `brightness`
 * @param b - `brightness` value
 * @return FALSE if failed or no such property
 */
static int camsetbrig(float b){
    TCHECK();
    if(b < -255.f || b > 255.f){
        WARNX("Available brightness: -255..255");
        return FALSE;
    }
    int br = (int) b;
    DBG("Try to set brightness to %d", br);
    int e = Toupcam_put_Brightness(toupcam.hcam, br);
    if(e < 0){
        WARNX("Can't set brightness: %s", errcode(e));
        return FALSE;
    }
    DBG("OK");
    return TRUE;
}

/**
 * @brief camgetbrig - camera.getbrightness - get `brightness` value
 * @param b (o) - brightness
 * @return FALSE if failed or no such property
 */
static int camgetbrig(float *b){
    TCHECK();
    int br;
    DBG("get brightness");
    if(Toupcam_get_Brightness(toupcam.hcam, &br) < 0) return FALSE;
    DBG("brightness=%d", br);
    if(b) *b = (float) br;
    return TRUE;
}

/**
 * @brief camsetexp - camera.setexp - set exposition time (s)
 * @param t - time (s)
 * @return FALSE if failed
 */
static int camsetexp(float t){
    TCHECK();
    if(t < FLT_EPSILON) return FALSE;
    unsigned int microseconds = (unsigned)(t * 1e6f);
    DBG("Set exptime to %dus", microseconds);
    int e = Toupcam_put_ExpoTime(toupcam.hcam, microseconds);
    if(e < 0){
        WARNX("Can't set exp: %s", errcode(e));
        return FALSE;
    }
    DBG("OK");
    exptime = (double) t;
    return TRUE;
}

/**
 * @brief camsetgain - camera.setgain - set gain
 * @param g - gain
 * @return FALSE if gain is wrong or no such property
 */
static int camsetgain(float g){
    TCHECK();
    unsigned short G = (unsigned short)(100.f * g);
    if(Toupcam_put_ExpoAGain(toupcam.hcam, G) < 0){
        unsigned short gmin, gmax, gdef;
        if(Toupcam_get_ExpoAGainRange(toupcam.hcam, &gmin, &gmax, &gdef) >= 0)
            WARNX("Gain out of range; min: %g, max: %g, default: %g",
                  (float)gmin/100.f, (float)gmax/100.f, (float)gdef/100.f);
        return FALSE;
    }
    DBG("GAIN is %d", G);
    return TRUE;
}

/**
 * @brief camgetgain - camera.getgain - getter
 * @param g (o) - gain
 * @return FALSE if have no such property
 */
static int camgetgain(float *g){
    TCHECK();
    unsigned short G;
    if(Toupcam_get_ExpoAGain(toupcam.hcam, &G) < 0) return FALSE;
    if(g) *g = (float)G / 100.f;
    return TRUE;
}

/**
 * @brief camsett - camera.setT - set cold side temperature
 * @param t - temperature (degC)
 * @return FALSE if failed
 */
static int camsett(float t){
    TCHECK();
    if(!(toupcam.flags & TOUPCAM_FLAG_TEC)) return FALSE; // cannot set temperature
    if(toupcam.flags & TOUPCAM_FLAG_TEC_ONOFF){
        int onoff = (t < 20.f) ? 1 : 0;
        if(Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_TEC, onoff) < 0) return FALSE;
        if(!onoff) return TRUE; // just turn off TEC if user wants >= 20degC
    }
    short T = (short) t * 10.f;
    DBG("Try to set T=%g", t);
    if(Toupcam_put_Temperature(toupcam.hcam, T) < 0) return FALSE;
    DBG("OK");
    return TRUE;
}

/**
 * @brief camgett - camera.getTcold - get cold side T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int camgettc(float *t){
    TCHECK();
    if(!(toupcam.flags & TOUPCAM_FLAG_GETTEMPERATURE)) return FALSE; // cannot get T
    short T;
    DBG("Try to get T");
    int g = Toupcam_get_Temperature(toupcam.hcam, &T);
    if(g < 0){
        WARNX("Can't get T: %s", errcode(g));
        return FALSE;
    }
    DBG("got %u", T);
    if(t) *t = ((float)T) / 10.f;
    return TRUE;
}

/**
 * @brief camgett - camera.getThot - get hot side T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int camgetth(float _U_ *t){
    //TCHECK();
    return FALSE;
}

/**
 * @brief camgett - camera.getTbody - get body T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int gettb(float _U_ *t){
    //TCHECK();
    return FALSE;
}

/**
 * @brief camsetbin - camera.setbin - binning setter
 * @param h, v - binning values (horiz/vert)
 * @return FALSE if failed or can't change binning
 */
static int camsetbin(int h, int v){
    TCHECK();
    if(h != v) return FALSE;
    DBG("Try to set binning %d/%d", h,v);
    if(Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_BINNING, h) < 0) return FALSE;
    if(Toupcam_get_Option(toupcam.hcam, TOUPCAM_OPTION_BINNING, &toupcam.curbin) < 0){
        DBG("CANT get binning");
        return FALSE;
    }
    if(h != toupcam.curbin){
        DBG("Wrong binning: %d instead of %d!", toupcam.curbin, h);
        return FALSE;
    }
    DBG("OK");
    return TRUE;
}

/**
 * @brief camgetbin - camera.getbin - binning getter
 * @param binh (o), binv (o) - current binning
 * @return FALSE if can't change binning
 */
static int camgetbin(int *binh, int *binv){
    TCHECK();
    DBG("Get binning");
    if(Toupcam_get_Option(toupcam.hcam, TOUPCAM_OPTION_BINNING, &toupcam.curbin) < 0) return FALSE;
    DBG("Got: %d", toupcam.curbin);
    if(binh) *binh = toupcam.curbin;
    if(binv) *binv = toupcam.curbin;
    return TRUE;
}

/**
 * @brief camshutter - camera.shuttercmd - work with shutter
 * @param s - new command for shutter (open/close etc)
 * @return FALSE if failed or can't
 */
static int camshutter(cc_shutter_op s){
    TCHECK();
    if(!(toupcam.flags & TOUPCAM_FLAG_GLOBALSHUTTER)) return FALSE;
    if(s != SHUTTER_OPEN && s != SHUTTER_CLOSE) return FALSE;
    int val = (s == SHUTTER_OPEN) ? 0 : 1;
    int e = Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_MECHANICALSHUTTER, val);
    if(e < 0){
        WARNX("Can't run shutter command: %s", errcode(e));
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief camsetgeom - camera.setgeometry - set geometry in UNBINNED coordinates
 * @param f (i) - new geometry
 * @return FALSE if can't change ROI or wrong geometry
 */
static int camsetgeom(cc_frameformat *f){
    TCHECK();
    if(!f) return FALSE;
   // WARNX("Your ROI is: off - %dx%d, frame - %dx%d", f->xoff, f->yoff, f->w, f->h);
    if(Toupcam_put_Roi(toupcam.hcam, (unsigned) f->xoff, (unsigned) f->yoff, (unsigned) f->w, (unsigned) f->h) < 0) return FALSE;
    camera.geometry = *f;
    return TRUE;
}

/**
 * @brief camgetnam - camera.getModelName - get model name
 * @param n (io) - prepared string for name
 * @param l - full length of n in bytes
 * @return FALSE if can't
 */
static int camgetnam(char *n, int l){
    TCHECK();
    if(!toupcam.dev || !n || l < 1) return FALSE;
    DBG("name: %s, strncpy to %d buf", toupcam.dev->displayname, l);
    strncpy(n, toupcam.dev->displayname, l);
    return TRUE;
}

/**
 * @brief camgmg - camera.getmaxgain - get max available gain
 * @param mg (o) - max gain
 * @return FALSE if can't change gain
 */
static int camgmg(float *mg){
    TCHECK();
    unsigned short gmin, gmax, gdef;
    if(Toupcam_get_ExpoAGainRange(toupcam.hcam, &gmin, &gmax, &gdef) < 0) return FALSE;
    if(mg) *mg = (float)gmax / 100.f;
    return TRUE;
}

/**
 * @brief camggl - camera.getgeomlimits - get limits of ROI changing
 * @param max (o) - max ROI
 * @param step (o) - step for ROI change
 * @return
 */
static int camggl(cc_frameformat *max, cc_frameformat _U_ *step){
    TCHECK();
    if(max) *max = camera.array;
    if(step) *step = (cc_frameformat){.w = 1, .h = 1, .xoff = 1, .yoff = 1};
    return TRUE;
}

/**
 * @brief camfan - camera.setfanspeed - set fan speed
 * @param spd - new speed
 * @return FALSE if can't
 */
static int camfan(cc_fan_speed spd){
    TCHECK();
    if(!(toupcam.flags & TOUPCAM_FLAG_FAN)) return FALSE; // don't have a fan
    DBG("Set fan to %d", spd);
    if(Toupcam_put_Option(toupcam.hcam, TOUPCAM_OPTION_FAN, (int)spd) < 0){
        DBG("Can't put this option");
        return FALSE;
    }
    return TRUE;
}

static cc_hresult setopt(const char *str, cc_charbuff *ans){
    if(!str || !toupcam.hcam) return CC_RESULT_FAIL;
    char key[256], *kptr = key;
    snprintf(key, 255, "%s", str);
    char *val = cc_get_keyval(&kptr);
    if(!kptr || !val || strcmp(kptr, "opt")) return CC_RESULT_BADKEY;
    snprintf(key, 255, "%s", val); // now this is our opt[=val]
    kptr = key; val = cc_get_keyval(&kptr);
    int result = -1, par;
    int o;
    if(!sl_str2i(&o, kptr)){
        cc_charbufput(ans, "Wrong integer: ", 15);
        cc_charbufaddline(ans, kptr);
    }
    DBG("optD: %u", o);
    if(val){ // setter
        par = atoi(val);
        result = Toupcam_put_Option(toupcam.hcam, (unsigned) o, par);
    }else{ // getter
        result = Toupcam_get_Option(toupcam.hcam, (unsigned) o, &par);
        if(result >= 0){
            snprintf(key, 255, "Option %d have value %d", o, par);
            cc_charbufaddline(ans, key);
        }
    }
    if(result < 0 ) return CC_RESULT_FAIL;
    return CC_RESULT_OK;
}

static cc_hresult getpf(const char *str, cc_charbuff *ans){
    if(!str || !toupcam.hcam) return CC_RESULT_FAIL;
    int N;
    if(Toupcam_get_PixelFormatSupport(toupcam.hcam, -1, &N) < 0) return CC_RESULT_FAIL;
    cc_charbufaddline(ans, "Supported formats:");
    for(int f = 0; f < N; ++f){
        int pf;
        if(Toupcam_get_PixelFormatSupport(toupcam.hcam, f, &pf) < 0) continue;
        cc_charbufaddline(ans, Toupcam_get_PixelFormatName(pf));
    }
    return CC_RESULT_SILENCE;
}

// cmd, help, checker, pointer, min, max, type
static cc_parhandler_t handlers[] = {
    {"opt", "set/get given option, like opt=0x08=1 (TOUPCAM_OPTION_TEC ON) or opt=0x08 (check)", setopt, NULL, NULL, NULL, 0},
    {"pixfmt", "get list of supported pixel formats", getpf, NULL, NULL, NULL, 0},
    CC_PARHANDLER_END
};

/**
 * @brief plugincmd - custom camera plugin command (get string as input, send string as output or NULL if failed)
 * @param str
 * @param buf
 * @return
 */
static cc_hresult plugincmd(const char _U_ *str, cc_charbuff _U_ *buf){
    return cc_plugin_customcmd(str, handlers, buf);
}


#if 0
// stub for void nonexistant functions
static void vstub(){
    FNAME();
    return;
}
#endif
// stub for nonexistant properties
static int stubi(int _U_ *N){
    return FALSE;
}
// stub for nonexistant integer setters
static int istub(int _U_ N){
    return FALSE;
}

/*
 * Global objects: camera, focuser and wheel
 */
__attribute__ ((visibility("default"))) cc_Camera camera = {
    .check = initcam,
    .Ndevices = 0,
    .close = camclose,
    .pollcapture = campoll,
    .capture = camcapt,
    .cancel = camcancel,
    .startexposition = startexp,
    .plugincmd = plugincmd,
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
    .setframetype = istub,
    .setbitdepth = camsetbit,
    .setfastspeed = istub,
    .setgeometry = camsetgeom,
    .setfanspeed = camfan,
    // getters:
    .getbitpix = camgetbp,
    .getbrightness = camgetbrig,
    .getModelName = camgetnam,
    .getgain = camgetgain,
    .getmaxgain = camgmg,
    .getgeomlimits = camggl,
    .getTcold = camgettc,
    .getThot = camgetth,
    .getTbody = gettb,
    .getbin = camgetbin,
    .getio = stubi,
    // these parameters could be filled after initialization
    .pixX = 10.,
    .pixY = 10.,
    .field = (cc_frameformat){.h = 1024, .w = 1024, .xoff = 10, .yoff = 10},
    .array = (cc_frameformat){.h = 1050, .w = 1050, .xoff = 0, .yoff = 0},
    .geometry = {0},
};

