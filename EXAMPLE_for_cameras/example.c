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

#include <usefull_macros.h>

#include "ccdcapture.h"

extern cc_Camera camera;
extern cc_Focuser focuser;
extern cc_Wheel wheel;

/**
 * @brief campoll - camera.pollcapture - polling of capture process status
 * @param st (o) - status of capture process
 * @param remain (o) - time remain (s)
 * @return FALSE if error (exp aborted), TRUE while no errors
 */
static int campoll(cc_capture_status _U_ *st, float _U_ *remain){
    return FALSE;
}

/**
 * @brief startexp - camera.startexposition - start exp if can
 * @return FALSE if failed
 */
static int startexp(){
    return FALSE;
}

/**
 * @brief camcapt - camera.capture - capture an image, struct `ima` should be prepared before
 * @param ima (o) - captured image
 * @return FALSE if failed or bad `ima`
 */
static int camcapt(cc_IMG _U_ *ima){
    return FALSE;
}

/**
 * @brief camsetbit - camera.setbitdepth
 * @param b - bit depth, 1 - high (16 bit), 0 - low (8 or other bit)
 * @return FALSE if failed
 */
static int camsetbit(int _U_ b){
    return FALSE;
}

/**
 * @brief camgetbp - camera.getbitpix - get bit depth in bits per pixel (8, 12, 16 etc)
 * @param bp (o) - bits per pixel
 * @return FALSE if failed
 */
static int camgetbp(uint8_t _U_ *bp){
    return FALSE;
}

/**
 * @brief camcancel - camera.cancel - cancel exposition
 */
static void camcancel(){
    ;
}

/**
 * @brief setdevno - camera.setDevNo - set active device number
 * @param n - device no
 * @return FALSE if no such device or failed
 */
static int setdevno(int _U_ n){
    return FALSE;
}

/**
 * @brief camsetbrig - camera.setbrightness - set `brightness`
 * @param b - `brightness` value
 * @return FALSE if failed or no such property
 */
static int camsetbrig(float _U_ b){
    return FALSE;
}

/**
 * @brief camgetbrig - camera.getbrightness - get `brightness` value
 * @param b (o) - brightness
 * @return FALSE if failed or no such property
 */
static int camgetbrig(float _U_ *b){
    return FALSE;
}

/**
 * @brief camsetexp - camera.setexp - set exposition time (s)
 * @param t - time (s)
 * @return FALSE if failed
 */
static int camsetexp(float _U_ t){
    return FALSE;
}

/**
 * @brief camsetgain - camera.setgain - set gain
 * @param g - gain
 * @return FALSE if gain is wrong or no such property
 */
static int camsetgain(float _U_ g){
    return FALSE;
}

/**
 * @brief camgetgain - camera.getgain - getter
 * @param g (o) - gain
 * @return FALSE if have no such property
 */
static int camgetgain(float _U_ *g){
    return FALSE;
}

/**
 * @brief camsett - camera.setT - set cold side temperature
 * @param t - temperature (degC)
 * @return FALSE if failed
 */
static int camsett(float _U_ t){
    return FALSE;
}

/**
 * @brief camgett - camera.getTcold - get cold side T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int camgettc(float _U_ *t){
    return FALSE;
}

/**
 * @brief camgett - camera.getThot - get hot side T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int camgetth(float _U_ *t){
    return FALSE;
}

/**
 * @brief camgett - camera.getTbody - get body T
 * @param t - temperature (degC)
 * @return FALSE if failed or no such property
 */
static int gettb(float _U_ *t){
    return FALSE;
}

/**
 * @brief camsetbin - camera.setbin - binning setter
 * @param h, v - binning values (horiz/vert)
 * @return FALSE if failed or can't change binning
 */
static int camsetbin(int _U_ h, int _U_ v){
    return FALSE;
}

/**
 * @brief camshutter - camera.shuttercmd - work with shutter
 * @param s - new command for shutter (open/close etc)
 * @return FALSE if failed or can't
 */
static int camshutter(cc_shutter_op _U_ s){
    return FALSE;
}

/**
 * @brief camsetgeom - camera.setgeometry - set geometry in UNBINNED coordinates
 * @param f (i) - new geometry
 * @return FALSE if can't change ROI or wrong geometry
 */
static int camsetgeom(cc_frameformat _U_ *f){
    return FALSE;
}

/**
 * @brief camgetnam - camera.getModelName - get model name
 * @param n (io) - prepared string for name
 * @param l - full length of n in bytes
 * @return FALSE if can't
 */
static int camgetnam(char _U_ *n, int _U_ l){
    return FALSE;
}

/**
 * @brief camgmg - camera.getmaxgain - get max available gain
 * @param mg (o) - max gain
 * @return FALSE if can't change gain
 */
static int camgmg(float _U_ *mg){
    return FALSE;
}

/**
 * @brief camggl - camera.getgeomlimits - get limits of ROI changing
 * @param max (o) - max ROI
 * @param step (o) - step for ROI change
 * @return
 */
static int camggl(cc_frameformat _U_ *max, cc_frameformat _U_ *step){
    return FALSE;
}

/**
 * @brief camgetbin - camera.getbin - binning getter
 * @param binh (o), binv (o) - current binning
 * @return FALSE if can't change binning
 */
static int camgetbin(int _U_ *binh, int _U_ *binv){
    return FALSE;
}

/**
 * @brief camgetio - camera.getio - get IO status
 * @param io (o) - GPIO status
 * @return FALSE if have no such property
 */
static int camgetio(int _U_ *io){
    return FALSE;
}

/**
 * @brief camfan - camera.setfanspeed - set fan speed
 * @param spd - new speed
 * @return FALSE if can't
 */
static int camfan(cc_fan_speed _U_ spd){
    return FALSE;
}

/**
 * @brief focsetpos - focuser.setAbsPos - set absolute position (in millimeters!!!)
 * @param a - ==1 for async running
 * @param n - new position
 * @return FALSE if failed or bad values
 */
static int focsetpos(int _U_ a, float _U_ n){
    return FALSE;
}

/**
 * @brief fochome - focuser.home - go to homing position
 * @param a - async flag
 * @return FALSE if failed
 */
static int fochome(int _U_ a){
    return FALSE;
}

/**
 * @brief focgetnam - focuser.getModelName - get name
 * @param n (o) - string for name
 * @param l - its length
 * @return FALSE if failed
 */
static int focgetnam(char _U_ *n, int _U_ l){
    return FALSE;
}

/**
 * @brief focpos - focuser.getPos - get current position
 * @param p (o) - position
 * @return  FALSE if failed
 */
static int focpos(float _U_ *p){
    return FALSE;
}

/**
 * @brief focMp - focuser.getMaxPos - max position number
 * @param p (o) - number
 * @return FALSE if failed
 */
static int focMp(float _U_ *p){
    return FALSE;
}

/**
 * @brief focmp - focuser.getMinPos - min position number
 * @param p (o) - position
 * @return FALSE if failed
 */
static int focmp(float _U_ *p){
    return FALSE;
}

/**
 * @brief whlsetpos - wheel.setPos - set position
 * @param n - new position
 * @return FALSE if failed or bad position
 */
static int whlsetpos(int _U_ n){
    return FALSE;
}

/**
 * @brief whlgetpos - wheel.getPos - get current position
 * @param n (o) - position
 * @return FALSE if failed
 */
static int whlgetpos(int _U_ *n){
    return FALSE;
}

/**
 * @brief whlgmp - wheel.getMaxPos - get max position
 * @param n (o) - position
 * @return FALSE if failed
 */
static int whlgmp(int _U_ *n){
    return FALSE;
}

/**
 * @brief whlgetnam - whhel.getModelName - get string with name
 * @param n (o) - string for name
 * @param l - its length
 * @return
 */
static int whlgetnam(char _U_ *n, int _U_ l){
    return FALSE;
}

#if 0
double sinPx, sinPy, sinmin;
// cmd, help, checker, pointer, min, max, type
static cc_parhandler_t handlers[] = {
    {"px", "set/get sin period over X axis (pix)", NULL, (void*)&sinPx, (void*)&sinmin, NULL, CC_PAR_DOUBLE},
    {"py", "set/get sin period over Y axis (pix)", NULL, (void*)&sinPy, (void*)&sinmin, NULL, CC_PAR_DOUBLE},
    CC_PARHANDLER_END
};
#endif

/**
 * @brief plugincmd - custom camera plugin command (get string as input, send string as output or NULL if failed)
 * @param str
 * @param buf
 * @return
 */
static cc_hresult plugincmd(const char _U_ *str, cc_charbuff _U_ *buf){
    return CC_RESULT_FAIL;
    //return cc_plugin_customcmd(str, handlers, buf);
}

// stub for nonexistant properties
static int stub(){
    return FALSE;
}

// stub for void nonexistant functions
static void vstub(){
    FNAME();
    return;
}

// stub for nonexistant integer setters
static int istub(int _U_ N){
    return FALSE;
}

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
    .getTcold = camgettc,
    .getThot = camgetth,
    .getTbody = gettb,
    .getbin = camgetbin,
    .getio = camgetio,
    // these parameters could be filled after initialization
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
    .getTbody = gettb,
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
    .getTbody = gettb,
    .getPos = whlgetpos,
    .getMaxPos = whlgmp,
};
