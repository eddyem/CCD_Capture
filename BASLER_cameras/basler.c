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

#include <pylonc/PylonC.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "ccdcapture.h"
#include "omp.h"
//#include "socket.h" // timestamp

extern cc_Camera camera;

static PYLON_DEVICE_HANDLE hDev;
static int isopened = FALSE, bitdepth = 16;
static char camname[BUFSIZ] = {0};
static size_t payloadsize = 0; // size of imgBuf
static unsigned char *imgBuf = NULL;
static uint32_t expostime = 0; // current exposition time (in microseconds)
static PYLON_DEVICECALLBACK_HANDLE hCb;
static int curhbin = 1, curvbin = 1;

typedef struct{
    int64_t min;
    int64_t max;
    int64_t incr;
    int64_t val;
} int64_values;

typedef struct{
    double min;
    double max;
    double val;
} float_values;

static char* describeError(GENAPIC_RESULT reserr){
    static char buf[1024];
    char* errMsg, *bptr = buf;
    size_t length, l = 1023;
    GenApiGetLastErrorMessage(NULL, &length);
    errMsg = MALLOC(char, length);
    GenApiGetLastErrorMessage(errMsg, &length);
    size_t ll = snprintf(bptr, l, "%s (%d); ", errMsg, (int)reserr);
    if(ll > 0){l -= ll; bptr += ll;}
    FREE(errMsg);
    GenApiGetLastErrorDetail(NULL, &length);
    errMsg = MALLOC(char, length);
    GenApiGetLastErrorDetail(errMsg, &length);
    snprintf(bptr, l, "%s", errMsg);
    FREE(errMsg);
    return buf;
}

#define PYLONFN(fn, ...) do{register GENAPIC_RESULT reserr; if(GENAPI_E_OK != (reserr=fn(__VA_ARGS__))){ \
    WARNX(#fn "(): %s", describeError(reserr)); return FALSE;}}while(0)

static void disconnect(){
    FNAME();
    if(!isopened) return;
    FREE(imgBuf);
    PylonDeviceDeregisterRemovalCallback(hDev, hCb);
    PylonDeviceClose(hDev);
    PylonDestroyDevice(hDev);
    PylonTerminate();
    isopened = FALSE;
}

/**
 * @brief chkNode - get node & check it for read/write
 * @param phNode (io) - pointer to node
 * @param nodeType    - type of node
 * @param wr          - ==TRUE if need to check for writeable
 * @return TRUE if node found & checked
 */
static int chkNode(NODE_HANDLE *phNode, const char *featureName, EGenApiNodeType nodeType, int wr){
    if(!isopened || !phNode || !featureName) return FALSE;
    NODEMAP_HANDLE hNodeMap;
    EGenApiNodeType nt;
    _Bool bv;
    PYLONFN(PylonDeviceGetNodeMap, hDev, &hNodeMap);
    PYLONFN(GenApiNodeMapGetNode, hNodeMap, featureName, phNode);
    if(*phNode == GENAPIC_INVALID_HANDLE) return FALSE;
    PYLONFN(GenApiNodeGetType, *phNode, &nt);
    if(nodeType != nt) return FALSE;
    PYLONFN(GenApiNodeIsReadable, *phNode, &bv);
    if(!bv) return FALSE; // not readable
    if(wr){
        PYLONFN(GenApiNodeIsWritable, *phNode, &bv);
        if(!bv) return FALSE; // not writeable
    }
    return TRUE;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

// getters of different types of data
static int getBoolean(const char *featureName, _Bool *val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, BooleanNode, FALSE)) return FALSE;
    if(!val) return TRUE;
    PYLONFN(GenApiBooleanGetValue, hNode, val);
    //DBG("Get boolean: %s = %s", featureName, val ? "true" : "false");
    return TRUE;
}
static int getInt(char *featureName, int64_values *val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, IntegerNode, FALSE)) return FALSE;
    if(!val) return TRUE;
    PYLONFN(GenApiIntegerGetMin, hNode, &val->min);
    PYLONFN(GenApiIntegerGetMax, hNode, &val->max);
    PYLONFN(GenApiIntegerGetInc, hNode, &val->incr);
    PYLONFN(GenApiIntegerGetValue, hNode, &val->val);
    //DBG("Get integer %s = %ld: min = %ld, max = %ld, incr = %ld", featureName, val->val, val->min, val->max, val->incr);
    return TRUE;
}
static int getFloat(char *featureName, float_values *val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, FloatNode, FALSE)) return FALSE;
    if(!val) return TRUE;
    PYLONFN(GenApiFloatGetMin, hNode, &val->min);
    PYLONFN(GenApiFloatGetMax, hNode, &val->max);
    PYLONFN(GenApiFloatGetValue, hNode, &val->val);
    //DBG("Get float %s = %g: min = %g, max = %g", featureName, val->val, val->min, val->max);
    return TRUE;
}

// setters of different types of data
static int setBoolean(char *featureName, _Bool val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, BooleanNode, TRUE)) return FALSE;
    PYLONFN(GenApiBooleanSetValue, hNode, val);
    return TRUE;
}
static int setInt(char *featureName, int64_t val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, IntegerNode, TRUE)) return FALSE;
    PYLONFN(GenApiIntegerSetValue, hNode, val);
    return TRUE;
}
static int setFloat(char *featureName, float val){
    if(!isopened || !featureName) return FALSE;
    NODE_HANDLE hNode;
    if(!chkNode(&hNode, featureName, FloatNode, TRUE)) return FALSE;
    PYLONFN(GenApiFloatSetValue, hNode, val);
    return TRUE;
}
#pragma GCC diagnostic pop

static void disableauto(){
    if(!isopened) return;
    const char *features[] = {"EnumEntry_TriggerSelector_AcquisitionStart",
                              "EnumEntry_TriggerSelector_FrameBurstStart",
                              "EnumEntry_TriggerSelector_FrameStart"};
    const char *triggers[] = {"AcquisitionStart", "FrameBurstStart", "FrameStart"};
    for(int i = 0; i < 3; ++i){
        if(PylonDeviceFeatureIsAvailable(hDev, features[i])){
            PylonDeviceFeatureFromString(hDev, "TriggerSelector", triggers[i]);
            PylonDeviceFeatureFromString(hDev, "TriggerMode", "Off");
        }
    }
    PylonDeviceFeatureFromString(hDev, "GainAuto", "Off");
    PylonDeviceFeatureFromString(hDev, "ExposureAuto", "Off");
    PylonDeviceFeatureFromString(hDev, "ExposureMode", "Timed");
    PylonDeviceFeatureFromString(hDev, "SequencerMode", "Off");
}

static void GENAPIC_CC removalCallbackFunction(_U_ PYLON_DEVICE_HANDLE hDevice){
    disconnect();
}

static int connect(){
    FNAME();
    size_t numDevices;
    disconnect();
    PylonInitialize();
    PYLONFN(PylonEnumerateDevices, &numDevices);
    if(!numDevices){
        WARNX("No cameras found");
        return FALSE;
    }
    camera.Ndevices = numDevices;
    return TRUE;
}

static int getbin(int *binh, int *binv){
    if(!PylonDeviceFeatureIsAvailable(hDev, "BinningVertical") ||
       !PylonDeviceFeatureIsAvailable(hDev, "BinningHorizontal")) return FALSE;
    int64_values v;
    if(!getInt("BinningVertical", &v)){
        DBG("Can't get Vbin");
        return FALSE;
    }
    curvbin = v.val;
    if(!getInt("BinningHorizontal", &v)){
        DBG("Can't get Hbin");
        return FALSE;
    }
    curhbin = v.val;
    DBG("binning: %d x %d", curhbin, curvbin);
    if(binv) *binv = curvbin;
    if(binh) *binh = curhbin;
    return TRUE;
}

static int getgeom(){
    FNAME();
    if(!isopened) return FALSE;
    int64_values i;
    if(!getInt("Width", &i)) return FALSE;
    camera.field.w = i.val;
    camera.array.w = i.max;
    if(!getInt("Height", &i)) return FALSE;
    camera.field.h = i.val;
    camera.array.h = i.max;
    if(!getInt("OffsetX", &i)) return FALSE;
    camera.field.xoff = i.val;
    camera.array.xoff = i.val;
    camera.array.w -= i.val;
    if(!getInt("OffsetY", &i)) return FALSE;
    camera.field.yoff = i.val;
    camera.array.yoff = i.val;
    camera.array.h -= i.val;
    camera.geometry = camera.field;
    return TRUE;
}

static int geometrylimits(cc_frameformat *max, cc_frameformat *step){
    FNAME();
    if(!isopened || !max || !step) return FALSE;
    int64_values i;
    if(!getInt("Width", &i)) return FALSE;
    max->w = i.max; step->w = i.incr;
    max->xoff = i.max; step->xoff = i.incr;
    if(!getInt("Height", &i)) return FALSE;
    max->h = i.max; step->h = i.incr;
    max->yoff = i.max; step->yoff = i.incr;
    if(!getInt("OffsetX", &i)) return FALSE;
    max->w -= i.max;
    if(!getInt("OffsetY", &i)) return FALSE;
    max->h -= i.max;
    return TRUE;
}

static int setdevno(int N){
    if(N > camera.Ndevices - 1) return FALSE;
    PYLONFN(PylonCreateDeviceByIndex, 0, &hDev);
    isopened = TRUE;
    PYLONFN(PylonDeviceOpen, hDev, PYLONC_ACCESS_MODE_CONTROL | PYLONC_ACCESS_MODE_STREAM | PYLONC_ACCESS_MODE_EXCLUSIVE);
    disableauto();
    PYLONFN(PylonDeviceFeatureFromString, hDev, "CameraOperationMode", "LongExposure");
    PYLONFN(PylonDeviceFeatureFromString, hDev, "UserSetSelector", "HighGain"); // set high gain selector
    PYLONFN(PylonDeviceFeatureFromString, hDev, "AcquisitionMode", "SingleFrame");
    PYLONFN(PylonDeviceExecuteCommandFeature, hDev, "UserSetLoad"); // load high gain mode
    if(PylonDeviceFeatureIsReadable(hDev, "DeviceModelName")){
        size_t l = BUFSIZ-1;
        PYLONFN(PylonDeviceFeatureToString, hDev, "DeviceModelName", camname, &l);
        DBG("Using camera %s\n", camname);
    }else strcpy(camname, "Unknown camera");
    if(!getgeom()) WARNX("Can't get current frame format");

    PylonDeviceRegisterRemovalCallback(hDev, removalCallbackFunction, &hCb);
    PYLON_STREAMGRABBER_HANDLE hGrabber;
    PYLONFN(PylonDeviceGetStreamGrabber, hDev, 0, &hGrabber);
    PYLONFN(PylonStreamGrabberOpen, hGrabber);
    PYLONFN(PylonStreamGrabberGetPayloadSize, hDev, hGrabber, &payloadsize);
    DBG("payload sz %zd", payloadsize);
    PylonStreamGrabberClose(hGrabber);
    FREE(imgBuf);
    imgBuf = MALLOC(unsigned char, payloadsize);
    return TRUE;
}

static int setbitdepth(int depth){
#define MONON 4
    const char *fmts[MONON] = {"Mono16", "Mono14", "Mono12", "Mono10"};
    const int depths[MONON] = {16, 14, 12, 10};
    if(depth == 0){ // 8 bit
        if(!PylonDeviceFeatureIsAvailable( hDev, "EnumEntry_PixelFormat_Mono8" )) return FALSE;
        PYLONFN(PylonDeviceFeatureFromString, hDev, "PixelFormat", "Mono8");
        green("Pixel format: Mono8\n");
        bitdepth = 8;
        DBG("8 bit");
    }else{ // 16 bit
        char buf[128];
        int  i = 0;
        for(; i < MONON; ++i){
            snprintf(buf, 127, "EnumEntry_PixelFormat_%s", fmts[i]);
            if(!PylonDeviceFeatureIsAvailable( hDev, buf)) continue;
            green("Pixel format: %s\n", fmts[i]);
            PYLONFN(PylonDeviceFeatureFromString, hDev, "PixelFormat", fmts[i]);
            bitdepth = depths[i];
            break;
        }
        if(i == MONON) return FALSE;
#undef MONON
        DBG("other bits");
    }
    PYLON_STREAMGRABBER_HANDLE hGrabber;
    PYLONFN(PylonDeviceGetStreamGrabber, hDev, 0, &hGrabber);
    PYLONFN(PylonStreamGrabberOpen, hGrabber);
    PYLONFN(PylonStreamGrabberGetPayloadSize, hDev, hGrabber, &payloadsize);
    DBG("payload sz %zd", payloadsize);
    PylonStreamGrabberClose(hGrabber);
    FREE(imgBuf);
    imgBuf = MALLOC(unsigned char, payloadsize);
    return TRUE;
}

static int getbitdepth(uint8_t *d){
    if(!d) return TRUE;
    *d = bitdepth;
    return TRUE;
}

// stub function: the capture process is blocking
static int pollcapt(cc_capture_status *st, float *remain){
    if(st) *st = CAPTURE_READY;
    if(remain) *remain = 0.f;
    return TRUE;
}

static int capture(cc_IMG *ima){
    FNAME();
    //double __t0 = dtime();
    if(!ima || !ima->data || !imgBuf || !isopened) return FALSE;
    static int toohot = FALSE;
    float_values f;
    //TIMESTAMP("start capt");
    if(!getFloat("DeviceTemperature", &f)) WARNX("Can't get temperature");
    else{
        DBG("Temperature: %.1f", f.val);
        if(f.val > 80.){
            WARNX("Device too hot");
            toohot = TRUE;
        }else if(toohot && f.val < 75.){
            DBG("Device temperature is normal");
            toohot = FALSE;
        }
    }
    PylonGrabResult_t grabResult;
    _Bool bufferReady;
    //TIMESTAMP("grab single frame");
    GENAPIC_RESULT res = PylonDeviceGrabSingleFrame(hDev, 0, imgBuf, payloadsize,
                                                    &grabResult, &bufferReady, 1000 + expostime);
    //TIMESTAMP("end of grabbing");
    if(res != GENAPI_E_OK || !bufferReady){
        WARNX("res != GENAPI_E_OK || !bufferReady");
        return FALSE;
    }
    if(grabResult.Status != Grabbed){
        WARNX("grabResult.Status != Grabbed (%d)", grabResult.Status);
        return FALSE;
    }
    int width = grabResult.SizeX, height = grabResult.SizeY, stride = grabResult.SizeX + grabResult.PaddingX;
    //TIMESTAMP("start converting");
    if(bitdepth > 8){
        int s2 = stride<<1;
        OMP_FOR()
        for(int y = 0; y < height; ++y){
            uint8_t *Out = &((uint8_t*)ima->data)[y*width];
            const uint8_t *In = &imgBuf[y*s2];
            for(int x = 0; x < width; ++x){
                *Out++ = *In; In += 2;
            }
        }
    }else{
        OMP_FOR()
        for(int y = 0; y < height; ++y){
            uint16_t *Out = &((uint16_t*)ima->data)[y*width];
            const uint8_t *In = &imgBuf[y*stride];
            memcpy(Out, In, width);
        }
    }
    //TIMESTAMP("image ready");
    ima->bitpix = bitdepth;
    return TRUE;
}

// Basler have no "brightness" parameter
static int setbrightness(_U_ float b){
    FNAME();
    return FALSE;
}

static int setexp(float e){
    FNAME();
    if(!isopened) return FALSE;
    e *= 1e6f;
    if(!setFloat("ExposureTime", e)){
       WARNX("Can't set expose time %g", e);
       return FALSE;
    }
    float_values f;
    if(!getFloat("ExposureTime", &f)) return FALSE;
    expostime = (uint32_t)(f.val);
    DBG("EXPOSURE time: need %f, real %f", e, f.val);
    return TRUE;
}

static int setgain(float e){
    FNAME();
    if(!isopened) return FALSE;
    if(!setFloat("Gain", e)){
       WARNX("Can't set gain %g", e);
       return FALSE;
    }
    DBG("GAIN -> %f", e);
    return TRUE;
}

static int changeformat(cc_frameformat *fmt){
    FNAME();
    if(!isopened) return FALSE;
    if(!getbin(NULL, NULL)){curhbin = 1; curvbin = 1;}
    fmt->h /= curvbin; fmt->yoff /= curvbin;
    fmt->w /= curhbin; fmt->xoff /= curhbin;
    DBG("set geom %dx%d (off: %dx%d)", fmt->w, fmt->h, fmt->xoff, fmt->yoff);
    setInt("Width", fmt->w);
    setInt("Height", fmt->h);
    setInt("OffsetX", fmt->xoff);
    setInt("OffsetY", fmt->yoff);
    int64_values i;
    if(getInt("Width", &i)) camera.geometry.w = fmt->w = i.val * curhbin;
    if(getInt("Height", &i)) camera.geometry.h = fmt->h = i.val * curvbin;
    if(getInt("OffsetX", &i)) camera.geometry.xoff = fmt->xoff = i.val * curhbin;
    if(getInt("OffsetY", &i)) camera.geometry.yoff = fmt->yoff = i.val * curvbin;
    return TRUE;
}

static int getgain(float *g){
    FNAME();
    float_values v;
    if(!getFloat("Gain", &v)) return FALSE;
    if(g) *g = (float)v.val;
    return TRUE;
}

static int gainmax(float *g){
    FNAME();
    float_values v;
    if(!getFloat("Gain", &v)) return FALSE;
    if(g) *g = (float)v.max;
    return TRUE;
}

static int modelname(char *buf, int bufsz){
    strncpy(buf, camname, bufsz);
    return TRUE;
}

static int setbin(int binh, int binv){
    if(!PylonDeviceFeatureIsAvailable(hDev, "BinningVertical") ||
       !PylonDeviceFeatureIsAvailable(hDev, "BinningHorizontal")){
        DBG("No Vbin / Hbin");
        return FALSE;
    }
    if(setInt("BinningVertical", (int64_t)binv) &&
       setInt("BinningHorizontal", (int64_t)binh)) return TRUE;
    DBG("Can't set v or h");
    return FALSE;
}

static int gett(float *t){
    if(!t) return FALSE;
    float_values fv;
    if(!getFloat("DeviceTemperature", &fv)) return FALSE;
    *t = fv.val;
    return TRUE;
}

static int setfanspd(_U_ cc_fan_speed s){
    return FALSE;
}
static int shutter(_U_ cc_shutter_op cmd){
    return FALSE;
}

static int ffalse(_U_ float f){ return FALSE; }
static int fpfalse(_U_ float *f){ return FALSE; }
static int ifalse(_U_ int i){ return FALSE; }
static int vtrue(){ return TRUE; }
static int ipfalse(_U_ int *i){ return FALSE; }
static void vstub(){ return ;}

/*
 * Global objects: camera, focuser and wheel
 */
cc_Camera camera = {
    .check = connect,
    .close = disconnect,
    .pollcapture = pollcapt,
    .capture = capture,
    .cancel = vstub,
    .startexposition = vtrue,
    // setters:
    .setDevNo = setdevno,
    .setbrightness = setbrightness,
    .setexp = setexp,
    .setgain = setgain,
    .setT = ffalse,
    .setbin = setbin,
    .setnflushes = ifalse,
    .shuttercmd = shutter,
    .confio = ifalse,
    .setio = ifalse,
    .setframetype = ifalse, // set DARK or NORMAL: no shutter -> no darks
    .setbitdepth = setbitdepth,
    .setfastspeed = ifalse,
    .setgeometry = changeformat,
    .setfanspeed = setfanspd,
    // getters:
    .getbrightness = fpfalse,
    .getbitpix = getbitdepth,
    .getModelName = modelname,
    .getgain = getgain,
    .getmaxgain = gainmax,
    .getgeomlimits = geometrylimits,
    .getTcold = fpfalse,
    .getThot = fpfalse,
    .getTbody = gett,
    .getbin = getbin,
    .getio = ipfalse,
};
