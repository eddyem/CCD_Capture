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
#include <omp.h>
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
#include "omp.h"

void signals(int signo){
    WARNX("Get signal %d - exit", signo);
    DBG("Cancel capturing");
    cancel();
#ifdef IMAGEVIEW
    DBG("KILL GL");
    closeGL();
    usleep(100000);
#endif
    exit(signo);
}

int main(int argc, char **argv){
    initial_setup();
/*
    int cpunumber = sysconf(_SC_NPROCESSORS_ONLN);
    if(omp_get_max_threads() != cpunumber)
        omp_set_num_threads(cpunumber);
*/
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

