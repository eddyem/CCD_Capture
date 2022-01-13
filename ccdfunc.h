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

#pragma once
#ifndef CCDFUNC_H__
#define CCDFUNC_H__

#include <stdint.h>

typedef struct{
    uint16_t *data;             // image data
    int w, h;                   // image size
    uint16_t max, min;          // min/max values
    float avr, std;             // statistics
} IMG;

// format of single frame
typedef struct{
    int w; int h;               // width & height
    int xoff; int yoff;         // X and Y offset
} frameformat;

typedef enum{
    SHUTTER_OPEN,       // open shutter now
    SHUTTER_CLOSE,      // close shutter now
    SHUTTER_OPENATLOW,  // ext. expose control @low
    SHUTTER_OPENATHIGH, // -//- @high
    SHUTTER_AMOUNT,     // amount of entries
} shutter_op;

typedef enum{
    CAPTURE_NO,         // no capture initiated
    CAPTURE_PROCESS,    // in progress
    CAPTURE_CANTSTART,  // can't start
    CAPTURE_ABORTED,    // some error - aborted
    CAPTURE_READY,      // ready - user can read image
} capture_status;

typedef enum{
    FAN_OFF,
    FAN_LOW,
    FAN_HIGH,
} fan_speed;

// all setters and getters of Camera, Focuser and Wheel should return TRUE if success or FALSE if failed or unsupported
// camera
typedef struct{
    int (*check)();             // check if the device is available, connect and init
    int Ndevices;               // amount of devices found
    void (*close)();            // disconnect & close device
    int (*pollcapture)(capture_status *st, float *remain);// start or poll capture process, `remain` - time remain (s)
    int (*capture)(IMG *ima);   // capture an image, struct `ima` should be prepared before
    void (*cancel)();           // cancel exposition
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setbrightness)(float b);
    int (*setexp)(float e);
    int (*setgain)(float g);
    int (*setT)(float t);
    int (*setbin)(int binh, int binv); // binning
    int (*setnflushes)(int N);  // flushes amount
    int (*shuttercmd)(shutter_op s); // work with shutter
    int (*confio)(int s);       // configure IO-port
    int (*setio)(int s);        // set IO-port to given state
    int (*setframetype)(int l); // set frametype: 1 - light, 0 - dark
    int (*setbitdepth)(int h);  // set bit depth: 1 - high, 0 - low
    int (*setfastspeed)(int s); // set readout speed: 1 - fast, 0 - low
    // geometry (if TRUE, all args are changed to suitable values)
    int (*setgeometry)(frameformat *fmt); // set geometry in UNBINNED coordinates
    int (*setfanspeed)(fan_speed spd); // set fan speed
    // getters:
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getmaxgain)(float *g);// get max available gain value
    // get limits of geometry: maximal values and steps
    int (*getgeomlimits)(frameformat *max, frameformat *step);
    int (*getTcold)(float *t);  // cold-side T
    int (*getThot)(float *t);   // hot-side T
    int (*getTbody)(float *t);  // body T
    int (*getbin)(int *binh, int *binv);
    int (*getio)(int *s);       // get IO-port state
    float pixX, pixY;           // pixel size in um
    frameformat field;          // max field of view
    frameformat array;          // array format
    frameformat geometry;       // current geometry settings (as in setgeometry)
} Camera;

// focuser
typedef struct{
    int (*check)();             // check if the device is available
    int Ndevices;
    void (*close)();
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setAbsPos)(int async, float n);// set absolute position (in millimeters!!!)
    int (*home)(int async);     // home device
    // getters:
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getTbody)(float *t);  // body T
    int (*getPos)(float *p);    // current position number (starting from zero)
    int (*getMaxPos)(float *p); // max position
    int (*getMinPos)(float *p); // min position
} Focuser;

// wheel
typedef struct{
    int (*check)();             // check if the device is available
    int Ndevices;
    void (*close)();
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setPos)(int n);       // set absolute position (starting from 0)
    // getters:
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getTbody)(float *t);  // body T
    int (*getPos)(int *p);      // current position number (starting from zero)
    int (*getMaxPos)(int *p);   // amount of positions
} Wheel;

void saveFITS(IMG *img, char *filename); // for imageview module
void focusers();
void wheels();
void ccds();
void cancel();

#endif // CCDFUNC_H__
