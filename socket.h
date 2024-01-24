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
#include <pthread.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <usefull_macros.h>

#include "ccdcapture.h"

#ifdef EBUG
extern double __t0;
#define TIMEINIT()  do{__t0 = dtime();}while(0)
#define TIMESTAMP(...) do{DBG(__VA_ARGS__); green("%g\n", dtime()-__t0); fflush(stdout);}while(0)
#else
#define TIMEINIT()
#define TIMESTAMP(...)
#endif

