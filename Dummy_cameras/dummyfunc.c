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

#include "basestructs.h"

extern Camera camera;
extern Focuser focuser;
extern Wheel wheel;

static const int filtermax = 5;
static const float focmaxpos = 10.;
static int curhbin = 1, curvbin = 1;
static int filterpos = 0;
static float focuserpos = 1., brightness = 1., gain = 0.;
static float camtemp = -30.;
static capture_status capstat = CAPTURE_NO;

static int campoll(capture_status *st, float *remain){
    if(capstat == CAPTURE_NO){
        if(st) *st = capstat = CAPTURE_PROCESS;
        if(remain) *remain = 1e-6;
    }else{
        capstat = CAPTURE_NO;
        if(st) *st = CAPTURE_READY;
        if(remain) *remain = 0.;
    }
    return TRUE;
}

static int camcapt(IMG *ima){
    static int n = 0;
    if(!ima || !ima->data) return FALSE;
    uint16_t *d = ima->data;
    for(int y = 0; y < ima->h; ++y)
        for(int x = 0; x < ima->w; ++x){ // sinusoide 100x200
            //*d++ = (uint16_t)(((n+x)%100)/99.*65535.);
            *d++ = (uint16_t)((1 + sin((n+x) * M_PI/50)*sin((n+y) * M_PI/100))*32767.);
        }
    ++n;
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

static int camsetexp(_U_ float t){
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
    curhbin = h; curvbin = v;
    return TRUE;
}

static int camshutter(_U_ shutter_op s){
    return TRUE;
}

static int camsetgeom(frameformat *f){
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

static int camggl(frameformat *max, frameformat *step){
    if(max) *max = camera.array;
    if(step) *step = (frameformat){1,1,1,1};
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

static int camfan(_U_ fan_speed spd){return TRUE;}

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
__attribute__ ((visibility("default"))) Camera camera = {
    .check = stub,
    .Ndevices = 1,
    .close = vstub,
    .pollcapture = campoll,
    .capture = camcapt,
    .cancel = camcancel,
    .startexposition = stub,
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
    .setbitdepth = istub,
    .setfastspeed = istub,
    .setgeometry = camsetgeom,
    .setfanspeed = camfan,
    // getters:
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
    .field = (frameformat){.h = 1024, .w = 1024, .xoff = 10, .yoff = 10},
    .array = (frameformat){.h = 1050, .w = 1050, .xoff = 0, .yoff = 0},
    .geometry = {0},
};

__attribute__ ((visibility("default"))) Focuser focuser = {
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

__attribute__ ((visibility("default"))) Wheel wheel = {
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
