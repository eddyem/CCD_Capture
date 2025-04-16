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
    .nframes = 2,
    .exptime = -1.
};

/*
 * Define command line options by filling structure:
 *  name    has_arg flag    val     type        argptr          help
*/
sl_option_t cmdlnopts[] = {
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
    double ts = sl_dtime();
    if(ts - shimg->timestamp > G.exptime + 1.) return FALSE; // too old image
    imnumber = shimg->imnumber;
    void *optr = img.data;
    memcpy(&img, shimg, sizeof(img));
    img.data = realloc(optr, img.bytelen);
    memcpy(img.data, (uint8_t*)shimg + sizeof(cc_IMG), img.bytelen);
    return TRUE;
}

#define STRBUFSZ    (256)

int main(int argc, char **argv){
    sl_init();
    sl_parseargs(&argc, &argv, cmdlnopts);
    if(G.help) sl_showhelp(-1, cmdlnopts);
    if(argc > 0){
        WARNX("%d unused parameters:", argc);
        for(int i = 0; i < argc; ++i)
            printf("%4d: %s\n", i, argv[i]);
    }
    if(G.nframes < 1) ERRX("nframes should be > 0");
    if(!G.sockname) ERRX("Point socket name or port");
    cc_strbuff *cbuf = cc_strbufnew(BUFSIZ, STRBUFSZ);
    int sock = cc_open_socket(FALSE, G.sockname, !G.isun);
    if(sock < 0) ERR("Can't open socket %s", G.sockname);
    int shmemkey = 0;
    if(CC_RESULT_OK == cc_getint(sock, cbuf, CC_CMD_SHMEMKEY, &shmemkey)){
        green("Got shm key: %d\n", shmemkey);
    }else{
        red("Can't read shmkey, try yours\n");
        shmemkey = G.shmkey;
    }
    if(G.infty){
        if(CC_RESULT_OK == cc_setint(sock, cbuf, CC_CMD_INFTY, 1)) green("ask for INFTY\n");
        else red("Can't ask for INFTY\n");
    }
    float xt = 0.f;
    if(CC_RESULT_OK == cc_getfloat(sock, cbuf, CC_CMD_EXPOSITION, &xt)){
        green("Old exp time: %gs\n", xt);
    }
    fflush(stdout);
    if(G.exptime > 0.){
        if(CC_RESULT_OK == cc_setfloat(sock, cbuf, CC_CMD_EXPOSITION, G.exptime)) green("ask for exptime %gs\n", G.exptime);
        else red("Can't change exptime to %gs\n", G.exptime);
    }
    shimg = cc_getshm(shmemkey, 0);
    if(!shimg) ERRX("Can't get shared memory segment");
    int i = 0;
    time_t oldtime = time(NULL);
    double waittime = ((int)G.exptime) + 5.;
    do{
        if(!G.infty){ // ask new image in non-infty mode
            if(CC_RESULT_OK != cc_setint(sock, cbuf, CC_CMD_EXPSTATE, CAMERA_CAPTURE)){
                WARNX("Can't ask new image\n");
                usleep(1000);
                continue;
            }
            usleep(1000);
        }
        if(cc_refreshbuf(sock, cbuf)){
            while(cc_getline(cbuf)) printf("\t\tServer sent: `%s`\n", cbuf->string);
        }
        time_t now = time(NULL);
        if(now - oldtime > waittime){
            WARNX("No new images for %g seconds", waittime);
            break;
        }
        if(!refresh_img()){
            usleep(1000);
            continue;
        }
        ++i;
        oldtime = now;
        printf("Got image #%zd, size %dx%d, bitpix %d, time %.2f\n", img.imnumber, img.w, img.h, img.bitpix, img.timestamp);
    }while(i < G.nframes);
    if(G.infty){
        if(CC_RESULT_OK != cc_setint(sock, cbuf, CC_CMD_INFTY, 0)) red("Can't clear INFTY\n");
    }
    if(xt > 0.){
        if(CC_RESULT_OK != cc_setfloat(sock, cbuf, CC_CMD_EXPOSITION, xt)) red("Can't return exptime to %gs\n", xt);
    }
    cc_strbufdel(&cbuf);
    close(sock);
    return 0;
}
