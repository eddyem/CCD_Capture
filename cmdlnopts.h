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
#ifndef CMDLNOPTS_H__
#define CMDLNOPTS_H__

#include "ccdfunc.h"
#include <usefull_macros.h>

/*
 * here are some typedef's for global data
 */
typedef struct{
    char *commondev;    // common (camera+focuser+wheel) plugin ("devfli.so", "devzwo.so" etc)
    char *cameradev;    // camera device plugin
    char *focuserdev;   // focuser ...
    char *wheeldev;     // wheel ...
    char *objname;      // object's name
    char *outfile;      // output filename prefix
    char *objtype;      // type of object (dark/obj/bias)
    char *instrument;   // instrument's name
    char *observers;    // observers' names
    char *prog_id;      // programm identificator
    char *author;       // programm author
    int listdevices;    // list connected devices
    int fanspeed;       // fan speed: 0-2
    int noflush;        // turn off bg flushing
    int camdevno;       // camera number (0, 1, 2 etc)
    int focdevno;
    int whldevno;
    int dark;           // dark frame
    int nframes;        // amount of frames to take
    int hbin; int vbin; // binning
    int X0; int Y0;     // top left corner coordinate (-1 - all, including overscan)
    int X1; int Y1;     // bottom right corner coordinate
    int fullframe;      // grab full frame (with overscans)
    int nflushes;       // amount of flushes
    int pause_len;      // pause (in seconds) between expositions
    int shtr_cmd;       // shutter command (flishutter_t)
    int _8bit;          // 8bit mode
    int fast;           // fast (8MHz) readout mode
    int getio;          // get value of ioport
    int setio;          // set value of ioport
    int confio;         // configure ioport
    double exptime;     // time of exposition in seconds
    double temperature; // temperature of CCD
    double gotopos;     // move stepper motor of focuser to absolute position
    double addsteps;    // move stepper motor of focuser to relative position
    int setwheel;       // set wheel position
    int async;          // asynchronous moving
    int verbose;        // each '-V' increases it
    int rewrite;        // rewrite file
    int showimage;      // show image preview
    char **addhdr;      // list of files from which to add header records
} glob_pars;


// default & global parameters
extern glob_pars const Gdefault;
extern glob_pars  *GP;

glob_pars *parse_args(int argc, char **argv);
void verbose(int levl, const char *fmt, ...);
#endif // CMDLNOPTS_H__
