/*
 * This file is part of the CCD_Capture project.
 * Copyright 2023 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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

/*
 * A simple example to take 8-bit images with default size and given exposition time.
 * Works in `infinity` mode or requesting each file after receiving previous.
 */

#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "socket.h"

typedef struct{
    char *sockname;     // UNIX socket name of command socket or port of local socket
    int isun;           // command socket is UNIX socket instead of INET
    int shmkey;         // shared memory (with image data) key
    int infty;          // run in infinity loop (if not - run by requests)
    int nframes;        // amount of frames to take
    int help;           // show this help
    double exptime;     // time of exposition in seconds
} glob_pars;


static glob_pars G = {
    .shmkey = 7777777,
    .nframes = 10
};

/*
 * Define command line options by filling structure:
 *  name    has_arg flag    val     type        argptr          help
*/
myoption cmdlnopts[] = {
    {"sock",    NEED_ARG,   NULL,   's',    arg_string, APTR(&G.sockname),  "command socket name or port"},
    {"isun",    NO_ARGS,    NULL,   'U',    arg_int,    APTR(&G.isun),      "use UNIX socket"},
    {"shmkey",  NEED_ARG,   NULL,   'k',    arg_int,    APTR(&G.shmkey),    "shared memory (with image data) key (default: 7777777)"},
    {"infty",   NO_ARGS,    NULL,   'i',    arg_int,    APTR(&G.infty),     "run in infinity capturing loop (else - request each frame)"},
    {"nframes", NEED_ARG,   NULL,   'n',    arg_int,    APTR(&G.nframes),   "make series of N frames"},
    {"exptime", NEED_ARG,   NULL,   'x',    arg_double, APTR(&G.exptime),   "set exposure time to given value (seconds!)"},
    {"help",    NO_ARGS,    NULL,   'h',    arg_int,    APTR(&G.help),      "show this help"},
};

static cc_IMG *shimg = NULL, img = {0};

static int refresh_img(){
    if(!shimg) return FALSE;
    static size_t imnumber = 0;
    if(shimg->imnumber == imnumber) return FALSE;
    imnumber = shimg->imnumber;
    void *optr = img.data;
    memcpy(&img, shimg, sizeof(img));
    img.data = realloc(optr, img.bytelen);
    memcpy(img.data, (uint8_t*)shimg + sizeof(cc_IMG), img.bytelen);
    return TRUE;
}

int main(int argc, char **argv){
    initial_setup();
    parseargs(&argc, &argv, cmdlnopts);
    if(G.help) showhelp(-1, cmdlnopts);
    if(argc > 0){
        WARNX("%d unused parameters:", argc);
        for(int i = 0; i < argc; ++i)
            printf("%4d: %s\n", i, argv[i]);
    }
    if(!G.sockname) ERRX("Point socket name or port");
    int sock = cc_open_socket(FALSE, G.sockname, !G.isun);
    if(sock < 0) ERR("Can't open socket %s", G.sockname);
    shimg = cc_getshm(G.shmkey, 0);
    if(!shimg) ERRX("Can't get shared memory segment");
    int i = 0;
    time_t oldtime = time(NULL);
    double oldtimestamp = shimg->timestamp;
    do{
        time_t now = time(NULL);
        if(now - oldtime > 5){
            WARNX("No new images for 5 seconds");
            break;
        }
        if(!refresh_img()){
            usleep(1000);
            continue;
        }
        ++i;
        oldtime = now;
        printf("Got image #%zd, size %dx%d, bitpix %d, time %g\n", img.imnumber, img.w, img.h, img.bitpix, img.timestamp-oldtimestamp);
    }while(i < 10);
    close(sock);
    return 0;
}
