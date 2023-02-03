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

#include <C/FlyCapture2_C.h>
#include <C/FlyCapture2Defs_C.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "basestructs.h"
#include "omp.h"

extern Camera camera;

static fc2Context context;
static fc2PGRGuid guid;
static fc2Error err = FC2_ERROR_OK;

static int isopened = FALSE, is16bit = FALSE;
static char camname[BUFSIZ] = {0};

#ifndef Stringify
#define Stringify(x) #x
#endif
#define FC2FN(fn, ...) do{err = FC2_ERROR_OK; if(FC2_ERROR_OK != (err=fn(context __VA_OPT__(,) __VA_ARGS__))){ \
    WARNX(Stringify(fn) "(): %s", fc2ErrorToDescription(err)); return FALSE;}}while(0)

static void disconnect(){
    FNAME();
    if(!isopened) return;
    fc2DestroyContext(context);
    isopened = FALSE;
}

static int getfloat(fc2PropertyType t, float *f){
    fc2Property prop = {0};
    prop.type = t;
    FC2FN(fc2GetProperty, &prop);
    if(!prop.present){
        DBG("No property %d", t);
        return FALSE;
    }
    DBG("property %d: abs=%f, vala=%u, valb=%u", t,
        prop.absValue, prop.valueA, prop.valueB);
    if(f) *f = prop.absValue;
    return TRUE;
}

/**
 * @brief setfloat - set absolute property value (float)
 * @param t        - type of property
 * @param f        - new value
 * @return 1 if all OK
 */
static int setfloat(fc2PropertyType t, float f){
    fc2Property prop = {0};
    prop.type = t;
    fc2PropertyInfo i = {0};
    i.type = t;
    FC2FN(fc2GetProperty, &prop);
    FC2FN(fc2GetPropertyInfo, &i);
    if(!prop.present || !i.present) return 0;
    if(prop.autoManualMode){
        if(!i.manualSupported){
            WARNX("Can't set auto-only property");
            return FALSE;
        }
        prop.autoManualMode = false;
    }
    if(!prop.absControl){
        if(!i.absValSupported){
            WARNX("Can't set non-absolute property to absolute value");
            return FALSE;
        }
        prop.absControl = true;
    }
    if(!prop.onOff){
        if(!i.onOffSupported){
            WARNX("Can't set property ON");
            return FALSE;
        }
        prop.onOff = true;
    }
    if(prop.onePush && i.onePushSupported) prop.onePush = false;
    prop.valueA = prop.valueB = 0;
    prop.absValue = f;
    FC2FN(fc2SetProperty, &prop);
    // now check
    FC2FN(fc2GetProperty, &prop);
    if(fabsf(prop.absValue - f) > 0.02f){
        WARNX("Can't set property! Got %g instead of %g.", prop.absValue, f);
        return FALSE;
    }
    return TRUE;
}

static int propOnOff(fc2PropertyType t, BOOL onOff){
    fc2Property prop = {0};
    prop.type = t;
    fc2PropertyInfo i = {0};
    i.type = t;
    FC2FN(fc2GetPropertyInfo, &i);
    FC2FN(fc2GetProperty, &prop);
    if(!prop.present || !i.present) return 0;
    if(prop.onOff == onOff) return 0;
    if(!i.onOffSupported){
        WARNX("Property doesn't support state OFF");
        return  0;
    }
    prop.onOff = onOff;
    FC2FN(fc2SetProperty, &prop);
    FC2FN(fc2GetProperty, &prop);
    if(prop.onOff != onOff){
        WARNX("Can't change property OnOff state");
        return 0;
    }
    return 1;
}

static void disableauto(){
    if(!isopened) return;
    propOnOff(FC2_AUTO_EXPOSURE, false);
    propOnOff(FC2_WHITE_BALANCE, false);
    propOnOff(FC2_GAMMA, false);
    propOnOff(FC2_TRIGGER_MODE, false);
    propOnOff(FC2_TRIGGER_DELAY, false);
    propOnOff(FC2_FRAME_RATE, false);
}

static int connect(){
    FNAME();
    unsigned int numDevices;
    disconnect();
    DBG("fc2CreateContext");
    if(FC2_ERROR_OK != (err = fc2CreateContext(&context))){
        WARNX("fc2CreateContext(): %s", fc2ErrorToDescription(err));
        return FALSE;
    }
    DBG("fc2GetNumOfCameras");
    FC2FN(fc2GetNumOfCameras, &numDevices);
    DBG("test");
    if(numDevices == 0){
        WARNX("No cameras detected!");
        fc2DestroyContext(context);
        return FALSE;
    }
    camera.Ndevices = numDevices;
    DBG("Found %d camera[s]", numDevices);
    return TRUE;
}

static int getbin(int *binh, int *binv){
    //unsigned int h, v;
    //FC2FN(fc2GetGigEImageBinningSettings, &h, &v);
    //green("got: %u x %u", h, v);
    if(binh) *binh = 1;
    if(binv) *binv = 1;
    return TRUE;
}

static int getformat(frameformat *fmt){
    if(!fmt) return FALSE;
    unsigned int packsz; float pc;
    fc2Format7ImageSettings f7;
    FC2FN(fc2GetFormat7Configuration, &f7, &packsz, &pc);
    fmt->h = f7.height; fmt->w = f7.width;
    fmt->xoff = f7.offsetX; fmt->yoff = f7.offsetY;
    if(f7.pixelFormat != FC2_PIXEL_FORMAT_MONO16){
        is16bit = FALSE;
        DBG("8 bit");
    }else{
        is16bit = TRUE;
        DBG("16 bit");
    }
    return TRUE;
}

static int getgeom(){
    FNAME();
    if(!isopened) return FALSE;
    fc2Format7Info f = {.mode = FC2_MODE_0};
    BOOL b;
    FC2FN(fc2GetFormat7Info, &f, &b);

    if(!b) return FALSE;
    camera.array.h = f.maxHeight;
    camera.array.w = f.maxWidth;
    camera.array.xoff = camera.array.yoff = 0;
    camera.field = camera.array;
    getformat(&camera.geometry);
    return TRUE;
}

static int geometrylimits(frameformat *max, frameformat *step){
    FNAME();
    if(!isopened || !max || !step) return FALSE;
    fc2Format7Info f = {.mode = FC2_MODE_0};
    BOOL b;
    fc2Format7Info i = {0};
    FC2FN(fc2GetFormat7Info, &i, &b);
    if(!b) return FALSE;
    max->h = f.maxHeight; max->w = f.maxWidth;
    max->xoff = f.maxWidth - f.offsetHStepSize;
    max->yoff = f.maxHeight - f.offsetVStepSize;
    step->w = f.imageHStepSize;
    step->h = f.imageVStepSize;
    step->xoff = f.offsetHStepSize;
    step->yoff = f.offsetVStepSize;
    return TRUE;
}

static int setdevno(int N){
    if(N > camera.Ndevices - 1) return FALSE;
    FC2FN(fc2GetCameraFromIndex, 0, &guid);
    FC2FN(fc2Connect, &guid);
    isopened = TRUE;
    disableauto();
    fc2CameraInfo caminfo;
    FC2FN(fc2GetCameraInfo, &caminfo);
    if(err == FC2_ERROR_OK){
        strncpy(camname, caminfo.modelName, BUFSIZ-1);
        DBG("Using camera %s\n", camname);
    }else strcpy(camname, "Unknown camera");

    if(!getbin(NULL, NULL)) WARNX("Can't get current binning");
    if(!getgeom()) WARNX("Can't get current frame format");
    return TRUE;
}

// stub function: the capture process is blocking
static int pollcapt(capture_status *st, float *remain){
    if(st) *st = CAPTURE_READY;
    if(remain) *remain = 0.f;
    return TRUE;
}

static int GrabImage(fc2Image *convertedImage){
    if(!convertedImage) return FALSE;
    int ret = FALSE;
    fc2Image rawImage;
    // start capture
    FC2FN(fc2StartCapture);
    err = fc2CreateImage(&rawImage);
    if(err != FC2_ERROR_OK){
        WARNX("Error in fc2CreateImage: %s", fc2ErrorToDescription(err));
        fc2StopCapture(context);
        return FALSE;
    }
    // Retrieve the image
    err = fc2RetrieveBuffer(context, &rawImage);
    if(err != FC2_ERROR_OK){
        WARNX("Error in fc2RetrieveBuffer: %s", fc2ErrorToDescription(err));
        goto rtn;
    }
    // Convert image to gray (we need to convert RAW into same bitpix
    fc2PixelFormat fmt = (is16bit) ? FC2_PIXEL_FORMAT_MONO16 : FC2_PIXEL_FORMAT_MONO8;
    err = fc2ConvertImageTo(fmt, &rawImage, convertedImage);
    if(err != FC2_ERROR_OK){
        WARNX("Error in fc2ConvertImageTo: %s", fc2ErrorToDescription(err));
        goto rtn;
    }
    ret = TRUE;
    DBG("raw: ds=%u, rds=%u, str=%u; conv: ds=%u, rds=%u, str=%u", rawImage.dataSize, rawImage.receivedDataSize, rawImage.stride,
        convertedImage->dataSize, convertedImage->receivedDataSize, convertedImage->stride);
rtn:
    fc2StopCapture(context);
    fc2DestroyImage(&rawImage);
    return ret;
}

static int capture(IMG *ima){
    FNAME();
    if(!ima || !ima->data || !isopened) return FALSE;
    static int toohot = FALSE;
    float f;
    if(getfloat(FC2_TEMPERATURE, &f)){
        DBG("Temperature: %.1f", f);
        if(f > 80.){
            WARNX("Device is too hot");
            toohot = TRUE;
        }else if(toohot && f < 75.){
            DBG("Device temperature is normal");
            toohot = FALSE;
        }
    }

    fc2Image convertedImage;
    err = fc2CreateImage(&convertedImage);
    if(err != FC2_ERROR_OK){
        WARNX("capture_grasshopper(): can't create image, %s", fc2ErrorToDescription(err));
        return FALSE;
    }
    if(!GrabImage(&convertedImage)){
        WARNX("Can't grab image");
        fc2DestroyImage(&convertedImage);
        return FALSE;
    }
    int width = convertedImage.cols, height = convertedImage.rows, stride = convertedImage.stride;
    DBG("w=%d, h=%d, s=%d", width, height, stride);
    if(is16bit){
        int w2 = width<<1;
        OMP_FOR()
        for(int y = 0; y < height; ++y){
            uint16_t *Out = &ima->data[y*width];
            const uint8_t *In = &convertedImage.pData[y*stride];
            memcpy(Out, In, w2);
        }
    }else{
        OMP_FOR()
        for(int y = 0; y < height; ++y){
            uint16_t *Out = &ima->data[y*width];
            const uint8_t *In = &convertedImage.pData[y*stride];
            for(int x = 0; x < width; ++x){
                *Out++ = *In++;
            }
        }
    }
    ima->bitpix = is16bit ? 16 : 8;
    fc2DestroyImage(&convertedImage);
    return TRUE;
}

static int setbrightness(float b){
    return setfloat(FC2_BRIGHTNESS, b);
}

static int setexp(float e){
    FNAME();
    if(!isopened) return FALSE;
    e *= 1e3f;
    if(!setfloat(FC2_SHUTTER, e)){
       WARNX("Can't set expose time %g", e);
       return FALSE;
    }
    return TRUE;
}

static int setgain(float e){
    FNAME();
    if(!isopened) return FALSE;
    if(!setfloat(FC2_GAIN, e)){
       WARNX("Can't set gain %g", e);
       return FALSE;
    }
    DBG("GAIN -> %f", e);
    return TRUE;
}

static int changeformat(frameformat *fmt){
    FNAME();
    if(!isopened) return FALSE;
    DBG("set geom %dx%d (off: %dx%d)", fmt->w, fmt->h, fmt->xoff, fmt->yoff);
    BOOL b;
    fc2Format7ImageSettings f7;
    f7.mode = FC2_MODE_0;
    f7.offsetX = fmt->xoff;
    f7.offsetY = fmt->yoff;
    f7.width = fmt->w;
    f7.height = fmt->h;
    DBG("offx=%d, offy=%d, w=%d, h=%d ", f7.offsetX, f7.offsetY, f7.width, f7.height);
    f7.pixelFormat = (is16bit) ? FC2_PIXEL_FORMAT_MONO16 : FC2_PIXEL_FORMAT_MONO8;
    fc2Format7PacketInfo f7p;
    FC2FN(fc2ValidateFormat7Settings, &f7, &b, &f7p);
    if(!b) return FALSE; // invalid
    FC2FN(fc2SetFormat7Configuration, &f7, f7p.recommendedBytesPerPacket);
    getformat(&camera.geometry);
    return TRUE;
}

static int setbitdepth(int i){
    frameformat fmt;
    getformat(&fmt);
    int o16bit = is16bit;
    if(i == 0) is16bit = FALSE; // 8 bit
    else is16bit = TRUE;
    if(!changeformat(&fmt)){
        is16bit = o16bit;
        return FALSE;
    }
    return TRUE;
}

static int getgain(float *g){
    return getfloat(FC2_GAIN, g);
}

static int gainmax(float *g){
    if(g) *g = 32.f;
    return TRUE;
}

static int modelname(char *buf, int bufsz){
    strncpy(buf, camname, bufsz);
    return TRUE;
}

// can't do binning
static int setbin(int binh, int binv){
    //FC2FN(fc2SetGigEImageBinningSettings, binh, binv);
    //getbin(&binh, &binv);
    if(binh != 1 || binv != 1)  return FALSE;
    return TRUE;
}

static int gett(float *t){
    return getfloat(FC2_TEMPERATURE, t);
}

static int setfanspd(_U_ fan_speed s){
    return FALSE;
}
static int shutter(_U_ shutter_op cmd){
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
Camera camera = {
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
