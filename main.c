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

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "cmdlnopts.h"
#include "ccdfunc.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif

void signals(int signo){
    cancel();
    exit(signo);
}

extern const char *__progname;

int main(int argc, char **argv){
    initial_setup();
    parse_args(argc, argv);
    signal(SIGINT, signals);
    signal(SIGQUIT, signals);
    signal(SIGABRT, signals);
    signal(SIGTERM, signals);
    focusers();
    wheels();
    ccds();
    return 0;
}

