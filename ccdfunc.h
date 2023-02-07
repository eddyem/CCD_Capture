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

#include "basestructs.h"

extern Camera *camera;
extern Focuser *focuser;
extern Wheel *wheel;

void calculate_stat(IMG *image);
int saveFITS(IMG *img, char **outp); // for imageview module
void focusers();
void wheels();
int prepare_ccds();
void ccds();
void cancel();

int startCCD(void **dlh);
int startWheel(void **dlh);
int startFocuser(void **dlh);
void focclose();
void closewheel();
void closecam();
int ccdcaptured(IMG **img);

#endif // CCDFUNC_H__
