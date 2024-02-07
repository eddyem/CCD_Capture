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
#include <improclib.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "ccdcapture.h"
#include "omp.h"

extern cc_Camera camera;
extern cc_Focuser focuser;
extern cc_Wheel wheel;

static const int filtermax = 5;
static const float focmaxpos = 10.;
static int curhbin = 1, curvbin = 1;
static int filterpos = 0;
static float focuserpos = 1., brightness = 1., gain = 0.;
static float camtemp = -30., exptime = 0.;
static cc_capture_status capstat = CAPTURE_NO;
static double texpstart = 0.;
static uint8_t bitpix = 16; // bit depth: 8 or 16

typedef struct{
    int width; int height; // size of field where the 'star' can move
    int x0; int y0; // center of star field in array coordinates
    double fwhm; // stars FWHM, arcsec
    double scale; // CCD scale: arcsec/pix
    double mag; // star magnitude: 0m is 16384 ADUs per second per arcsec^2
} settings_t;

static settings_t settings = {
    .width = 500, .height = 500,
    .x0 = 512, .y0 = 512,
    .fwhm = 1.5, .scale = 0.03, .mag = 10.
};
// min/max for parameters
static const int wmin = 100, hmin = 100;
static const double fwhmmin = 0.1, fwhmmax = 10., scalemin = 0.001, scalemax = 3600., magmin = -30., magmax = 30.;

static int campoll(cc_capture_status *st, float *remain){
    if(capstat != CAPTURE_PROCESS){
        if(st) *st = capstat;
        if(remain) *remain = 0.;
        return TRUE;
    }
    if(dtime() - texpstart > exptime){
        if(st) *st = CAPTURE_READY;
        if(remain) *remain = 0.;
        capstat = CAPTURE_NO;
        return TRUE;
    }
    if(st) *st = capstat;
    if(remain) *remain = exptime + texpstart - dtime();
    return TRUE;
}

static int startexp(){
    if(capstat == CAPTURE_PROCESS) return FALSE;
    capstat = CAPTURE_PROCESS;
    texpstart = dtime();
    return TRUE;
}

static void gen16(cc_IMG *ima){
    static int n = 0;
    int y1 = ima->h * curvbin, x1 = ima->w * curhbin;
    OMP_FOR()
    for(int y = 0; y < y1; y += curvbin){
        uint16_t *d = &((uint16_t*)ima->data)[y*ima->w/curvbin];
        for(int x = 0; x < x1; x += curhbin){ // sinusoide 100x200
            //*d++ = (uint16_t)(((n+x)%100)/99.*65535.);
            *d++ = (uint16_t)((1. + sin((n+x) * (2.*M_PI)/11.)*sin((n+y) * (2.*M_PI)/22.))*32767.);
        }
    }
    ++n;
}
static void gen8(cc_IMG *ima){
    static int n = 0;
    int y1 = ima->h * curvbin, x1 = ima->w * curhbin;
    OMP_FOR()
    for(int y = 0; y < y1; y += curvbin){
        uint8_t *d = &((uint8_t*)ima->data)[y*ima->w/curvbin];
        for(int x = 0; x < x1; x += curhbin){ // sinusoide 100x200
            //*d++ = (uint16_t)(((n+x)%100)/99.*65535.);
            *d++ = (uint8_t)((1. + sin((n+x) * (2.*M_PI)/11.)*sin((n+y) * (2.*M_PI)/22.))*127.);
        }
    }
    ++n;
}

static int camcapt(cc_IMG *ima){
    if(!ima || !ima->data) return FALSE;
#ifdef EBUG
    double t0 = dtime();
#endif
    ima->bitpix = bitpix;
    bzero(ima->data, ima->h*ima->w*cc_getNbytes(ima));
    if(bitpix == 16) gen16(ima);
    else gen8(ima);
    DBG("Time of capture: %g", dtime() - t0);
    return TRUE;
}

static int camsetbit(int b){
    bitpix = (b) ? 16 : 8;
    return TRUE;
}

static int camgetbp(uint8_t *bp){
    if(bp) *bp = bitpix;
    return TRUE;
}

static void camcancel(){
    capstat = CAPTURE_NO;
}

static int setdevno(int n){
    if(n) return FALSE;
    return TRUE;
}

static int camsetbrig(float b){
    brightness = b;
    return TRUE;
}

static int camgetbrig(float *b){
    if(!b) return FALSE;
    *b = brightness;
    return TRUE;
}

static int camsetexp(float t){
    exptime = t;
    return TRUE;
}
static int camsetgain(float g){
    gain = g;
    return TRUE;
}

static int camgetgain(float *g){
    if(g) *g = gain;
    return TRUE;
}

static int camsett(float t){
    camtemp = t;
    return TRUE;
}

static int camgett(float *t){
    if(t) *t = camtemp;
    return TRUE;
}

static int gett(float *t){
    if(t) *t = M_PI;
    return TRUE;
}

static int camsetbin(int h, int v){
    DBG("set bin %dx%d", h, v);
    curhbin = h; curvbin = v;
    return TRUE;
}

static int camshutter(_U_ cc_shutter_op s){
    return TRUE;
}

static int camsetgeom(cc_frameformat *f){
    if(!f) return FALSE;
    camera.geometry = *f;
    return TRUE;
}

static int camgetnam(char *n, int l){
    strncpy(n, "Dummy camera", l);
    return TRUE;
}

static int camgmg(float *mg){
    if(mg) *mg = 10.;
    return TRUE;
}

static int camggl(cc_frameformat *max, cc_frameformat *step){
    if(max) *max = camera.array;
    if(step) *step = (cc_frameformat){1,1,1,1};
    return TRUE;
}

static int camgetbin(int *binh, int *binv){
    if(binh) *binh = curhbin;
    if(binv) *binv = curvbin;
    return TRUE;
}

static int camgetio(int *io){
    if(io) *io = 0xDEADBEEF;
    return TRUE;
}

static int camfan(_U_ cc_fan_speed spd){return TRUE;}

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

static int whlsetpos(int n){
    if(n >= filtermax || n < 0) return FALSE;
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

// cmd, help, handler, ptr, min, max, type
static cc_parhandler_t handlers[] = {
    {"width", "width of star moving field", NULL, (void*)&settings.width, (void*)&wmin, NULL, CC_PAR_INT},
    {"height", "height of star moving field", NULL, (void*)&settings.height, (void*)&hmin, NULL, CC_PAR_INT},
    {"xc", "x center of field in array coordinates", NULL, (void*)&settings.x0, NULL, NULL, CC_PAR_INT},
    {"yc", "y center of field in array coordinates", NULL, (void*)&settings.y0, NULL, NULL, CC_PAR_INT},
    {"fwhm", "star FWHM, arcsec", NULL, (void*)&settings.fwhm, (void*)&fwhmmin, (void*)&fwhmmax, CC_PAR_DOUBLE},
    {"scale", "CCD scale: arcsec/pix", NULL, (void*)&settings.scale, (void*)&scalemin, (void*)&scalemax, CC_PAR_DOUBLE},
    {"mag", "star magnitude: 0m is 16384 ADUs per second per arcsec^2", NULL, (void*)&settings.mag, (void*)&magmin, (void*)&magmax, CC_PAR_DOUBLE},
    //{"", "", NULL, (void*)&, (void*)&, (void*)&settings., CC_PAR_DOUBLE},
    CC_PARHANDLER_END
};

static cc_hresult plugincmd(const char *str, cc_charbuff *buf){
    return cc_plugin_customcmd(str, handlers, buf);
}

static int stub(){
    return TRUE;
}

static void vstub(){
    FNAME();
    return;
}
static int istub(_U_ int N){return TRUE;}

/*
 * Global objects: camera, focuser and wheel
 */
__attribute__ ((visibility("default"))) cc_Camera camera = {
    .check = stub,
    .Ndevices = 1,
    .close = vstub,
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
    .getTcold = camgett,
    .getThot = camgett,
    .getTbody = gett,
    .getbin = camgetbin,
    .getio = camgetio,
    .pixX = 10.,
    .pixY = 10.,
    .field = (cc_frameformat){.h = 1024, .w = 1024, .xoff = 10, .yoff = 10},
    .array = (cc_frameformat){.h = 1050, .w = 1050, .xoff = 0, .yoff = 0},
    .geometry = {0},
};

__attribute__ ((visibility("default"))) cc_Focuser focuser = {
    .check = stub,
    .Ndevices = 1,
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
    .Ndevices = 1,
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
