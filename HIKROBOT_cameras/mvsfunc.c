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

#include <MvCameraControl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "basestructs.h"
#include "omp.h"

#ifndef FLT_EPSILON
#define FLT_EPSILON 1.19209290E-07F
#endif

extern Camera camera;

static MV_CC_DEVICE_INFO_LIST stDeviceList;
static void *handle = NULL;
static char camname[BUFSIZ] = {0};
//static long cam_err, tmpl;
static capture_status capStatus = CAPTURE_NO;
static int curhbin = 1, curvbin = 1;
static double starttime = 0.;   // time when exposure started
static float exptime = 0.;      // exposition time (in seconds)
static MV_FRAME_OUT_INFO_EX stImageInfo = {0}; // last image info
static uint8_t *pdata = NULL;
static int pdatasz = 0;
static int lastecode = MV_OK;

static struct{
    float maxgain;
    float mingain;
    float maxbright;
    float minbright;
    float minexp;
    float maxexp;
    int maxbin;
} extrvalues = {0}; // extremal values

static void printErr(){
    const char *errcode = "unknown error";
    switch(lastecode){
        case MV_E_HANDLE:           errcode = "Error or invalid handle ";                                         break;
        case MV_E_SUPPORT:          errcode = "Not supported function ";                                          break;
        case MV_E_BUFOVER:          errcode = "Cache is full ";                                                   break;
        case MV_E_CALLORDER:        errcode = "Function calling order error ";                                    break;
        case MV_E_PARAMETER:        errcode = "Incorrect parameter ";                                             break;
        case MV_E_RESOURCE:         errcode = "Applying resource failed ";                                        break;
        case MV_E_NODATA:           errcode = "No data ";                                                         break;
        case MV_E_PRECONDITION:     errcode = "Precondition error, or running environment changed ";              break;
        case MV_E_VERSION:          errcode = "Version mismatches ";                                              break;
        case MV_E_NOENOUGH_BUF:     errcode = "Insufficient memory ";                                             break;
        case MV_E_ABNORMAL_IMAGE:   errcode = "Abnormal image, maybe incomplete image because of lost packet ";   break;
        case MV_E_UNKNOW:           errcode = "Unknown error ";                                                   break;
        case MV_E_GC_GENERIC:       errcode = "General error ";                                                   break;
        case MV_E_GC_ACCESS:        errcode = "Node accessing condition error ";                                  break;
        case MV_E_ACCESS_DENIED:	errcode = "No permission ";                                                   break;
        case MV_E_BUSY:             errcode = "Device is busy, or network disconnected ";                         break;
        case MV_E_NETER:            errcode = "Network error ";
    }
    WARNX("CMOS error: %s", errcode);
}

#define TRY(fn, ...)    do{lastecode = MV_CC_ ## fn(handle __VA_OPT__(,) __VA_ARGS__); if(lastecode != MV_OK) printErr(); }while(0)
#define ONERR()         if(MV_OK != lastecode)
#define ONOK()          if(MV_OK == lastecode)

static int changeenum(const char *key, uint32_t val){
    if(!handle) return FALSE;
    MVCC_ENUMVALUE e;
    TRY(GetEnumValue, key, &e);
    ONERR(){
        WARNX("Enum '%s' is absent", key);
        return FALSE;
    }
    DBG("Try to change '%s' to %u, cur=%u", key, val, e.nCurValue);
    if(e.nCurValue == val) return TRUE;
    TRY(SetEnumValue, key, val);
    ONERR(){
        WARNX("Cant change %s to %d, supported values are:", key, val);
        for(int i = 0; i < (int)e.nSupportedNum; ++i){
            fprintf(stderr, "%s%u", i ? ", " : "", e.nSupportValue[i]);
        }
        fprintf(stderr, "\n");
        return FALSE;
    }
    TRY(GetEnumValue, key, &e);
    ONERR() return FALSE;
    if(e.nCurValue == val) return TRUE;
    WARNX("New value of '%s' changed to %d, not to %d", key, e.nCurValue, val);
    return FALSE;
}

static int changeint(const char *key, uint32_t val){
    if(!handle) return FALSE;
    MVCC_INTVALUE i;
    TRY(GetIntValue, key, &i);
    ONERR(){
        WARNX("Int '%s' is absent", key);
        return FALSE;
    }
    if(i.nCurValue == val) return TRUE;
    TRY(SetIntValue, key, val);
    ONERR(){
        WARNX("Cant change %s to %u; available range is %u..%u", key, val, i.nMin, i.nMax);
        return FALSE;
    }
    TRY(GetIntValue, key, &i);
    ONERR() return FALSE;
    if(i.nCurValue == val) return TRUE;
    WARNX("New value of '%s' changed to %d, not to %d", key, i.nCurValue, val);
    return FALSE;
}

static int changefloat(const char *key, float val){
    if(!handle) return FALSE;
    MVCC_FLOATVALUE f;
    TRY(GetFloatValue, key, &f);
    ONERR(){
        WARNX("Float '%s' is absent", key);
        return FALSE;
    }
    if(f.fCurValue == val) return TRUE;
    TRY(SetFloatValue, key, val);
    ONERR(){
        WARNX("Cant change %s to %g; available range is %g..%g", key, val, f.fMin, f.fMax);
        return FALSE;
    }
    TRY(GetFloatValue, key, &f);
    ONERR() return FALSE;
    if(fabs(f.fCurValue - val) < FLT_EPSILON) return TRUE;
    WARNX("New value of '%s' changed to %g, not to %g", key, f.fCurValue, val);
    return FALSE;
}

static int cam_setbin(int binh, int binv){
    FNAME();
    if(!handle) return FALSE;
    if(!changeenum("BinningHorizontal", binh)) return FALSE;
    if(!changeenum("BinningVertical", binv)) return FALSE;
    curhbin = binh;
    curvbin = binv;
    return TRUE;
}

static int cam_getbin(int *h, int *v){
    MVCC_ENUMVALUE e;
    TRY(GetEnumValue, "BinningHorizontal", &e);
    ONERR() return FALSE;
    curhbin = e.nCurValue;
    //printf("Hbin supported = %d", e.nSupportedNum);
    //for(int i = 0; i < (int)e.nSupportedNum; ++i) printf("\t%d", e.nSupportValue[i]);
    TRY(GetEnumValue, "BinningVertical", &e);
    ONERR() return FALSE;
    curvbin = e.nCurValue;
    //printf("Vbin supported = %d", e.nSupportedNum);
    //for(int i = 0; i < (int)e.nSupportedNum; ++i) printf("\t%d", e.nSupportValue[i]);
    if(h) *h = curhbin;
    if(v) *v = curvbin;
    return TRUE;
}

static int cam_getgain(float *g){
    if(!handle) return FALSE;
    MVCC_FLOATVALUE gain;
    TRY(GetFloatValue, "Gain", &gain);
    ONERR() return FALSE;
    if(g) *g = gain.fCurValue;
    extrvalues.maxgain = gain.fMax;
    extrvalues.mingain = gain.fMin;
    DBG("Gain: cur=%g, min=%g, max=%g", gain.fCurValue, gain.fMin, gain.fMax);
    return TRUE;
}

static int cam_getmaxgain(float *g){
    if(!handle) return FALSE;
    if(g) *g = extrvalues.maxgain;
    return TRUE;
}

static int cam_setgain(float g){
    if(!handle) return FALSE;
    return changefloat("Gain", g);
}

static int cam_getbright(float *b){
    if(!handle) return FALSE;
    DBG("Get brightness");
    MVCC_INTVALUE bright;
    TRY(GetIntValue, "Brightness", &bright);
    ONERR() return FALSE;
    if(b) *b = bright.nCurValue;
    extrvalues.maxgain = bright.nMax;
    extrvalues.mingain = bright.nMin;
    DBG("Brightness: cur=%d, min=%d, max=%d", bright.nCurValue, bright.nMin, bright.nMax);
    return TRUE;
}

static int cam_setbright(float b){
    if(!handle) return FALSE;
    return changeint("Brightness", (uint32_t)b);
}

static void PrintDeviceInfo(MV_CC_DEVICE_INFO* pstMVDevInfo){
    if(!pstMVDevInfo) return;
    if(pstMVDevInfo->nTLayerType == MV_GIGE_DEVICE){
        int nIp1 = ((pstMVDevInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0xff000000) >> 24);
        int nIp2 = ((pstMVDevInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x00ff0000) >> 16);
        int nIp3 = ((pstMVDevInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x0000ff00) >> 8);
        int nIp4 = (pstMVDevInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0x000000ff);
        printf("Device Model Name: %s\n", pstMVDevInfo->SpecialInfo.stGigEInfo.chModelName);
        strncpy(camname, (char*)pstMVDevInfo->SpecialInfo.stGigEInfo.chModelName, BUFSIZ-1);
        printf("CurrentIp: %d.%d.%d.%d\n" , nIp1, nIp2, nIp3, nIp4);
        printf("UserDefinedName: %s\n\n" , pstMVDevInfo->SpecialInfo.stGigEInfo.chUserDefinedName);
    }else if (pstMVDevInfo->nTLayerType == MV_USB_DEVICE){
        printf("Device Model Name: %s\n", pstMVDevInfo->SpecialInfo.stUsb3VInfo.chModelName);
        printf("UserDefinedName: %s\n\n", pstMVDevInfo->SpecialInfo.stUsb3VInfo.chUserDefinedName);
        strncpy(camname, (char*)pstMVDevInfo->SpecialInfo.stUsb3VInfo.chModelName, BUFSIZ-1);
    }else {
        printf("Not support.\n");
    }
}

static void cam_closecam(){
    DBG("CAMERA CLOSE");
    if(handle){
        MV_CC_StopGrabbing(handle);
        TRY(CloseDevice);
        ONERR() WARNX("Can't close opened camera");
        TRY(DestroyHandle);
        ONERR() WARNX("Can't destroy camera handle");
        handle = NULL;
    }
    FREE(pdata);
    pdatasz = 0;
}

static int cam_findCCD(){
    DBG("Try to find HIKROBOT cameras .. ");
    memset(&stDeviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    if(MV_OK != MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &stDeviceList)){
        WARNX("No HIKROBOT cameras found");
        return FALSE;
    }
    camera.Ndevices = stDeviceList.nDeviceNum;
    if(stDeviceList.nDeviceNum > 0){
        for(uint32_t i = 0; i < stDeviceList.nDeviceNum; ++i){
            DBG("[device %d]:\n", i);
            MV_CC_DEVICE_INFO* pDeviceInfo = stDeviceList.pDeviceInfo[i];
            if(!pDeviceInfo) continue;
            PrintDeviceInfo(pDeviceInfo);
        }
    } else{
        WARNX("No HIKROBOT cameras found");
        return FALSE;
    }
    return TRUE;
}

static int cam_setActiceCam(int n){
    DBG("SET ACTIVE #%d", n);
    if(!camera.Ndevices && !cam_findCCD()) return FALSE;
    if(n >= camera.Ndevices){
        return FALSE;
    }
    cam_closecam();
    lastecode = MV_CC_CreateHandleWithoutLog(&handle, stDeviceList.pDeviceInfo[n]);
    ONERR(){
        WARNX("Can't create camera handle");
        printErr();
        return FALSE;
    }
    TRY(OpenDevice, MV_ACCESS_Exclusive, 0);
    ONERR(){
        WARNX("Can't open camera file");
        return FALSE;
    }
    if(stDeviceList.pDeviceInfo[n]->nTLayerType == MV_GIGE_DEVICE){
        int nPacketSize = MV_CC_GetOptimalPacketSize(handle);
        if(nPacketSize > 0){
            if(!changeint("GevSCPSPacketSize", nPacketSize)){
                WARNX("Can't set optimal packet size");
            }
        } else{
            WARNX("Can't get optimal packet size");
        }
    }
    // set software trigger
/*    MVCC_ENUMVALUE enumval;
    MV_CC_GetEnumValue(handle, "TriggerMode", &enumval);
    DBG("TRmode: %d", enumval.nSupportedNum);
    for(uint16_t i = 0; i < enumval.nSupportedNum; ++i) fprintf(stderr, "\t%d: %u\n", i, enumval.nSupportValue[i]);
    */
    if(!changeenum("TriggerMode", MV_TRIGGER_MODE_OFF)){
        WARNX("Can't turn off triggered mode");
        return FALSE;
    }
    if(!changeenum("AcquisitionMode", MV_ACQ_MODE_SINGLE)){
        WARNX("Can't set acquisition mode to single");
        return FALSE;
    }
    if(!changeenum("ExposureMode", MV_EXPOSURE_MODE_TIMED)){
        WARNX("Can't change exposure mode to timed");
        return FALSE;
    }
    if(!changeenum("ExposureAuto", MV_EXPOSURE_AUTO_MODE_OFF)){
        WARNX("Can't turn off auto exposure mode");
        return FALSE;
    }
    if(!changeenum("GainAuto", 0)){
        WARNX("Can't turn off auto gain");
        return FALSE;
    }
    MVCC_ENUMVALUE EnumValue;
    TRY(GetEnumValue, "PixelFormat", &EnumValue);
    ONOK(){
        DBG("PixelFormat=%x", EnumValue.nCurValue);
#ifdef EBUG
        for(int i = 0; i < (int)EnumValue.nSupportedNum; ++i) fprintf(stderr, "\t\t%x\n", EnumValue.nSupportValue[i]);
#endif
    }
    TRY(GetEnumValue, "PixelSize", &EnumValue);
    ONOK(){
        DBG("PixelSize=%d", EnumValue.nCurValue);
#ifdef EBUG
        for(int i = 0; i < (int)EnumValue.nSupportedNum; ++i) fprintf(stderr, "\t\t%d\n", EnumValue.nSupportValue[i]);
#endif
    }
    cam_getgain(NULL); // get extremal gain values
    cam_getbright(NULL); // get extremal brightness values
    cam_getbin(NULL, NULL); // get current binning
    MVCC_FLOATVALUE FloatValue;
    // get extremal exptime values
    TRY(GetFloatValue, "ExposureTime", &FloatValue);
    ONOK(){
        extrvalues.maxexp = FloatValue.fMax / 1e6;
        extrvalues.minexp = FloatValue.fMin / 1e6;
        exptime = FloatValue.fCurValue / 1e6;
        printf("Min exp: %g s, max exp: %g s\n", extrvalues.minexp, extrvalues.maxexp);
    }
    camera.pixX = camera.pixY = 0.; // unknown
    MVCC_INTVALUE IntValue;
    camera.array.xoff = camera.array.yoff = 0;
    int *values[6] = {&camera.array.w, &camera.array.h, &camera.geometry.w, &camera.geometry.h, &camera.geometry.xoff, &camera.geometry.yoff};
    const char *names[2] = {"WidthMax", "HeightMax"};//, "Width", "Height", "OffsetX", "OffsetY"};
    for(int i = 0; i < 2; ++i){
        TRY(GetIntValue, names[i], &IntValue);
        ONERR(){
            WARNX("Can't get %s", names[i]); return FALSE;
        }
        *values[i] = IntValue.nCurValue;
        DBG("%s = %d", names[i], *values[i]);
    }
    camera.array.h *= curvbin;
    camera.array.w *= curhbin;
    camera.geometry = camera.array;
    camera.field = camera.array;
    pdatasz = camera.array.h * camera.array.w * 2;
    DBG("\t\t2*w*h = %d", pdatasz);
#ifdef EBUG
    MVCC_INTVALUE stParam = {0};
    TRY(GetIntValue, "PayloadSize", &stParam);
    ONOK(){DBG("PAYLOAD: %u", stParam.nCurValue);}
#endif
    pdata = MALLOC(uint8_t, pdatasz); // allocate max available buffer
    return TRUE;
}

static int cam_geomlimits(frameformat *l, frameformat *s){
    if(l) *l = camera.array;
    if(s) *s = (frameformat){.w = 1, .h = 1, .xoff = 1, .yoff = 1};
    return TRUE;
}

static int cam_startexp(){
    if(!handle || !pdata) return FALSE;
    DBG("Start exposition");
    MV_CC_StopGrabbing(handle);
    TRY(StartGrabbing);
    ONERR() return FALSE;
    starttime = dtime();
    capStatus = CAPTURE_PROCESS;
    return TRUE;
}

static int cam_pollcapt(capture_status *st, float *remain){
    if(!handle || !pdata) return FALSE;
    DBG("capStatus = %d", capStatus);
    if(capStatus == CAPTURE_READY){
        DBG("Capture ends");
        goto retn;
    }
    if(capStatus == CAPTURE_NO){ // start capture
        goto retn;
    }
    if(capStatus == CAPTURE_PROCESS){
        DBG("PDATASZ=%d", pdatasz);
        TRY(GetOneFrameTimeout, pdata, pdatasz, &stImageInfo, 50);
        ONOK(){
            DBG("OK, ready");
            if(remain) *remain = 0.f;
            if(st) *st = CAPTURE_READY;
            capStatus = CAPTURE_NO;
            return TRUE;
        }
        DBG("not ready");
        if(remain){
            float diff = exptime - (dtime() - starttime);
            DBG("diff = %g", diff);
            if(diff < -5.0){
                capStatus = CAPTURE_NO;
                if(st) *st = CAPTURE_ABORTED;
                return FALSE;
            }
            if(diff < 0.f) diff = 0.f;
            *remain = diff;
        }
    }else{ // some error
        if(st) *st = CAPTURE_ABORTED;
        capStatus = CAPTURE_NO;
        return FALSE;
    }
retn:
    if(st) *st = capStatus;
    return TRUE;
}

static int cam_capt(IMG *ima){
    if(!handle || !pdata) return FALSE;
    if(!ima || !ima->data) return FALSE;
    ;
    int bytes = ima->h*ima->w * 2, stbytes = stImageInfo.nWidth * stImageInfo.nHeight * 2;
    if(bytes > pdatasz) bytes = pdatasz;
    if(bytes != stbytes) WARNX("Different sizes of image buffer & grabbed image");
    if(stbytes > bytes) bytes = stbytes;
    DBG("Copy %d bytes (stbytes=%d)", bytes, stbytes);
    MVCC_ENUMVALUE EnumValue;
    TRY(GetEnumValue, "PixelSize", &EnumValue);
    DBG("PixelSize = %u", EnumValue.nCurValue);
    ONOK(){
//green("pixsize=%d\n", EnumValue.nCurValue);
        if(EnumValue.nCurValue == 16){
            memcpy(ima->data, pdata, bytes);
            return TRUE;
        }else if(EnumValue.nCurValue != 8){
            WARNX("Unsupported pixel size");
            return FALSE;
        }
    }
    // transform 8bits to 16
    DBG("TRANSFORM 8 bit to 16");
    bytes /= 2;
    uint8_t *ptr = (uint8_t*) pdata;
    OMP_FOR()
    for(int i = 0; i < bytes; ++i){
        ima->data[i] = (uint16_t) *ptr++;
    }
    return TRUE;
}

static int cam_modelname(char *buf, int bufsz){
    strncpy(buf, camname, bufsz);
    return TRUE;
}

static int cam_setgeometry(frameformat *f){
    FNAME();
    if(!f || !handle) return FALSE;
    DBG("getbin");
    if(!cam_getbin(NULL, NULL)) return FALSE;
    DBG("set geom %dx%d (off: %dx%d)", f->w, f->h, f->xoff, f->yoff);
    if(!changeint("Width", f->w * curhbin)) return FALSE;
    if(!changeint("Height", f->h * curvbin)) return FALSE;
    if(!changeint("OffsetX", f->xoff * curhbin)) return FALSE;
    if(!changeint("OffsetY", f->yoff * curvbin)) return FALSE;
    DBG("Success!");
    return TRUE;
}

static int cam_settemp(float t){
    if(!handle) return FALSE;
    if(!changeenum("DeviceTemperatureSelector", 0)) return FALSE;
    if(!changeenum("DeviceTemperature", t)) return FALSE;
    return TRUE;
}

static int cam_gettemp(float *t){
    MVCC_FLOATVALUE fl;
    TRY(GetFloatValue, "DeviceTemperature", &fl);
    ONERR() return FALSE;
    if(t) *t = fl.fCurValue;
    return TRUE;
}

static int cam_gettchip(float *t){
    if(!handle) return FALSE;
    changeenum("DeviceTemperatureSelector", 0); // there's can be camera without this enume
    return cam_gettemp(t);
}

static int cam_gettbody(_U_ float *t){
    if(!handle) return FALSE;
    if(!changeenum("DeviceTemperatureSelector", 1)) return FALSE;
    return cam_gettemp(t);
}

static void cam_cancel(){
    if(!handle) return;
    TRY(StopGrabbing);
}

static int cam_shutter(_U_ shutter_op cmd){
    return FALSE;
}

/*
static int cam_confio(int io){
    if(!handle) return FALSE;
    MVCC_ENUMVALUE e;
    TRY(GetEnumValue, "LineSelector", &e);
    ONERR() return FALSE;
    int bit = 1;
    for(int i = 0; i < (int)e.nSupportedNum; ++i, bit <<= 1){
        green("line %d: %d\n", e.nSupportValue[i]);
        if(io & bit) printf("bit %d\n", i);
    }
    return TRUE;
}
*/

static int cam_setexp(float t){ // t is in seconds!!
    if(!handle) return FALSE;
    if(!changefloat("ExposureTime", t*1e6)) return FALSE;
    exptime = t;
    return TRUE;
}

static int cam_setbitdepth(int i){
    if(!handle) return FALSE;
    int d = i ? PixelType_Gvsp_Mono12  : PixelType_Gvsp_Mono8;
    if(!changeenum("PixelFormat", d)){
        WARNX("pixformat");
        return FALSE;
    }
    d = i ? 16 : 8;
    if(!changeenum("PixelSize", d)){
        WARNX("pixsz");
        return FALSE;
    }
    return TRUE;
}

static int cam_setfanspd(_U_ fan_speed s){
    return FALSE;
}

//static int cam_ffalse(_U_ float f){ return FALSE; }
static int cam_fpfalse(_U_ float *f){ return FALSE; }
static int cam_ifalse(_U_ int i){ return FALSE; }
static int cam_ipfalse(_U_ int *i){ return FALSE; }

/*
 * Global objects: camera, focuser and wheel
 */
Camera camera = {
    .check = cam_findCCD,
    .close = cam_closecam,
    .pollcapture = cam_pollcapt,
    .capture = cam_capt,
    .cancel = cam_cancel,
    .startexposition = cam_startexp,
    // setters:
    .setDevNo = cam_setActiceCam,
    .setbrightness = cam_setbright,
    .setexp = cam_setexp,
    .setgain = cam_setgain,
    .setT = cam_settemp,
    .setbin = cam_setbin,
    .setnflushes = cam_ifalse,
    .shuttercmd = cam_shutter,
    .confio = cam_ifalse,
    .setio = cam_ifalse,
    .setframetype = cam_ifalse, // set DARK or NORMAL: no shutter -> no darks
    .setbitdepth = cam_setbitdepth,
    .setfastspeed = cam_ifalse,
    .setgeometry = cam_setgeometry,
    .setfanspeed = cam_setfanspd,
    // getters:
    .getbrightness = cam_getbright,
    .getModelName = cam_modelname,
    .getgain = cam_getgain,
    .getmaxgain = cam_getmaxgain,
    .getgeomlimits = cam_geomlimits,
    .getTcold = cam_gettchip,
    .getThot = cam_gettbody,
    .getTbody = cam_fpfalse,
    .getbin = cam_getbin,
    .getio = cam_ipfalse,
};
