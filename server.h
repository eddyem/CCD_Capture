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
#ifndef SERVER_H__
#define SERVER_H__

typedef enum{
    CAMERA_IDLE,        // idle state, client send this to cancel capture
    CAMERA_CAPTURE,     // capturing frame, client send this to start capture
    CAMERA_FRAMERDY,    // frame ready to be saved
    CAMERA_ERROR        // can't do exposition
} camera_state;

// pause (seconds) between temperature logging
#define TLOG_PAUSE  60.

// server-side functions
void server(int fd);

// common information about everything
#define CMD_INFO        "info"

// CCD/CMOS
#define CMD_CAMLIST     "camlist"
#define CMD_CAMDEVNO    "camdevno"
#define CMD_EXPOSITION  "exptime"
#define CMD_FILENAME    "filename"
#define CMD_FILENAMEPREFIX "filenameprefix"
// rewrite=1 will rewrite files, =0 - not (only for `filename`)
#define CMD_REWRITE     "rewrite"
#define CMD_HBIN        "hbin"
#define CMD_VBIN        "vbin"
#define CMD_CAMTEMPER   "tcold"
#define CMD_CAMFANSPD   "ccdfanspeed"
#define CMD_SHUTTER     "shutter"
#define CMD_CONFIO      "confio"
#define CMD_IO          "io"
#define CMD_GAIN        "gain"
#define CMD_BRIGHTNESS  "brightness"
#define CMD_FRAMEFORMAT "format"
#define CMD_FRAMEMAX    "maxformat"
#define CMD_NFLUSHES    "nflushes"
// expstate=CAMERA_CAPTURE will start exposition, CAMERA_IDLE - cancel
#define CMD_EXPSTATE    "expstate"
#define CMD_TREMAIN     "tremain"

// focuser
#define CMD_FOCLIST     "foclist"
#define CMD_FDEVNO      "focdevno"
#define CMD_FGOTO       "focgoto"

// wheel
#define CMD_WLIST       "wlist"
#define CMD_WDEVNO      "wdevno"
#define CMD_WPOS        "wpos"

#endif // SERVER_H__
