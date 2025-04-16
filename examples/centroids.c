/*
 * This file is part of the CCD_Capture project.
 * Copyright 2024 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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
 * A bit more usefull than ccd_client: get images and calculate their gravity center
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <improclib.h>
#include <usefull_macros.h>

#include "socket.h"

#define STRBUFSZ    (256)

typedef struct{
    int isun;           // command socket is UNIX socket instead of INET
    int shmkey;         // shared memory (with image data) key
    int infty;          // run in infinity loop (if not - run by requests)
    int nframes;        // amount of frames to take
    int help;           // show this help
    double exptime;     // time of exposition in seconds
    double background;  // fixed bg
    char *sockname;     // UNIX socket name of command socket or port of local socket
    char *outfile;      // output file name
} glob_pars;


static glob_pars G = {
    .shmkey = 7777777,
    .nframes = 10,
    .background = -1.,
    .exptime = -1.
};

/*
 * Define command line options by filling structure:
 *  name    has_arg flag    val     type        argptr          help
*/
sl_option_t cmdlnopts[] = {
    {"background",NEED_ARG, NULL,   'b',    arg_double, APTR(&G.background),"fixed background level"},
    {"sock",    NEED_ARG,   NULL,   's',    arg_string, APTR(&G.sockname),  "command socket name or port"},
    {"isun",    NO_ARGS,    NULL,   'U',    arg_int,    APTR(&G.isun),      "use UNIX socket"},
    {"shmkey",  NEED_ARG,   NULL,   'k',    arg_int,    APTR(&G.shmkey),    "shared memory (with image data) key (default: 7777777)"},
    {"infty",   NO_ARGS,    NULL,   'i',    arg_int,    APTR(&G.infty),     "run in infinity capturing loop (else - request each frame)"},
    {"nframes", NEED_ARG,   NULL,   'n',    arg_int,    APTR(&G.nframes),   "make series of N frames (default: 10)"},
    {"exptime", NEED_ARG,   NULL,   'x',    arg_double, APTR(&G.exptime),   "set exposure time to given value (seconds!)"},
    {"help",    NO_ARGS,    NULL,   'h',    arg_int,    APTR(&G.help),      "show this help"},
    {"output",  NEED_ARG,   NULL,   'o',    arg_string, APTR(&G.outfile),   "output file with T/x/y/w"},
};

static cc_IMG *shimg = NULL, img = {0};
static FILE *out = NULL;
//static double *sq = NULL;
//static int sqsz = 0;

static void calcimg(){
    int H = img.h, W = img.w, npix = 0;
    /*int m = (H>W) ? H : W;
    if(m > sqsz){
        sq = realloc(sq, sizeof(double)*m);
        for(int i = sqsz; i < m; ++i) sq[i] = (double)i*i;
        sqsz = m;
    }*/
    double Xs = 0., X2s = 0., Ys = 0., Y2s = 0., Is = 0;
    double t0 = sl_dtime();
    uint8_t *d = (uint8_t*)img.data;
    double Timestamp = img.timestamp;
    static double bg = -1.;
    if(bg < 0.){
        if(G.background >= 0.) bg = G.background;
        else{
            il_Image *ii = il_u82Image(d, W, H);
            if(ii){
                il_Image_background(ii, &bg);
                il_Image_free(&ii);
            }else bg = 5.;
        }
    }
    printf("bg=%g\n", bg);
/* SYNC  lasts too long!
#pragma omp parallel
{
    double Xsp = 0., X2sp = 0., Ysp = 0., Y2sp = 0., Isp = 0;
    #pragma omp for nowait
    for(int x = 0; x < W; ++x){
        for(int y = 0; y < H; ++y){
            double val = *d++ - bg;
            if(val < DBL_EPSILON) continue;
            Xsp += val * x; Ysp += val * y; X2sp += val * x * x; Y2sp += val * y * y;
            Isp += val;
        }
    }
    #pragma omp critical
    {
        Xs += Xsp; Ys += Ysp; X2s += X2sp; Y2s += Y2sp; Is += Isp;
    }
}*/
    for(int y = 0; y < H; ++y){
        for(int x = 0; x < W; ++x){
            // *d = (*d > bg) ? *d - bg : 0;
            double val = *d++ - bg;
            if(val < DBL_EPSILON) continue;
            //Xs += val * x; Ys += val * y; X2s += val * sq[x]; Y2s += val * sq[y]; - no time advantage
            Xs += val * x; Ys += val * y; X2s += val * x * x; Y2s += val * y * y;
            Is += val;
            ++npix;
        }
    }
    /*char buf[256];
    snprintf(buf, 255, "im%06zd.png", img.imnumber);
    il_write_png(buf, W, H, 1, img.data);*/
    printf("Xs=%g, X2s=%g, Ys=%g, Y2s=%g, Is=%g\n", Xs, X2s, Ys, Y2s, Is);
    double xc = Xs/Is, yc = Ys/Is, sX = sqrt(X2s/Is-xc*xc), sY = sqrt(Y2s/Is-yc*yc);
    green("Xc = %.2f, Yc=%.2f, Xcs=%.2f, Ycs=%.2f, I=%.1f, T=%gms; npix=%d\n", xc, yc, sX, sY, Is, (sl_dtime() - t0)*1e3, npix);
    if(out) fprintf(out, "%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", Timestamp, xc, yc, Is, sX, sY);
}

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
    calcimg();
    return TRUE;
}

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
    if(G.outfile){
        out = fopen(G.outfile, "w");
        if(!out) ERR("Can't open %s for writing");
        fprintf(out, "# Time\t\tXc\tYc\tI\tsX\tsY\t\n");
    }
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
    if(CC_RESULT_OK != cc_setint(sock, cbuf, CC_CMD_8BIT, 1)){
        ERRX("Can't set 8 bit mode");
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
    }else G.exptime = xt;
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
