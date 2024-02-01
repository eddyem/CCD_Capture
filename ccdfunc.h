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
#include "ccdcapture.h"

extern cc_Camera *camera;
extern cc_Focuser *focuser;
extern cc_Wheel *wheel;

void calculate_stat(cc_IMG *image);
cc_charbuff *getFITSheader(cc_IMG *img);
int saveFITS(cc_IMG *img, char **outp); // for imageview module
void focusers();
void wheels();
int prepare_ccds();
void ccds();
void camstop();

cc_Camera *startCCD();
cc_Wheel *startWheel();
cc_Focuser *startFocuser();
void focclose();
void closewheel();
void closecam();
#ifdef IMAGEVIEW
void framerate();
int ccdcaptured(cc_IMG **img);
#endif

int start_socket(int isserver);
