/*
 * This file is part of the CCD_Capture project.
 * Copyright 2026 Edward V. Emelianov <edward.emelianoff@gmail.com>.
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
 * A 16-bit analog of `centroids`: get images from the server (in 16-bit mode)
 * and calculate their gravity center and second moments.
 *
 * The background is estimated as `median + 1 sigma` over the whole frame
 * (improclib does not yet provide an il_Image wrapper for 16-bit data).
 * The estimate is computed once and cached; you can override it with --background.
 */

#include <float.h>    // DBL_EPSILON
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "socket.h"

#define STRBUFSZ    (256)

typedef struct{
    int shmkey;         // shared memory (with image data) key
    int infty;          // run in infinity loop (if not - run by requests)
    int nframes;        // amount of frames to take
    int help;           // show this help
    double nsigmas;     // calculate backround as `median + nsigmas*RMS`
    double exptime;     // time of exposition in seconds
    double background;  // fixed bg (-1 -> auto: median + 1 sigma)
    char *port;         // net port
    char *sockname;     // UNIX socket name of command socket or port of local socket
    char *outfile;      // output file name
} glob_pars;


static glob_pars G = {
    .shmkey = 7777777,
    .sockname = UNIX_SOCK_NAME,
    .nframes = 10,
    .nsigmas = 1,
    .background = -1.,
    .exptime = -1.
};

/*
 * Define command line options by filling structure:
 *  name    has_arg flag    val     type        argptr          help
*/
sl_option_t cmdlnopts[] = {
    {"background",NEED_ARG, NULL,   'b',    arg_double, APTR(&G.background),"fixed background level (default: median + 1 sigma)"},
    {"sigmas",  NEED_ARG,   NULL,   's',    arg_double, APTR(&G.nsigmas),   "amount of sigmas [-5..15] added to image median for background calculation (default: 1)"},
    {"node",    NEED_ARG,   NULL,   'N',    arg_string, APTR(&G.sockname),  "INET command socket node or UNIX socket name"},
    {"port",    NEED_ARG,   NULL,   'p',    arg_string, APTR(&G.port),      "port of INET command socket"},
    {"shmkey",  NEED_ARG,   NULL,   'k',    arg_int,    APTR(&G.shmkey),    "shared memory (with image data) key (default: 7777777)"},
    {"infty",   NO_ARGS,    NULL,   'i',    arg_int,    APTR(&G.infty),     "run in infinity capturing loop (else - request each frame)"},
    {"nframes", NEED_ARG,   NULL,   'n',    arg_int,    APTR(&G.nframes),   "make series of N frames (default: 10)"},
    {"exptime", NEED_ARG,   NULL,   'x',    arg_double, APTR(&G.exptime),   "set exposure time to given value (seconds!)"},
    {"help",    NO_ARGS,    NULL,   'h',    arg_int,    APTR(&G.help),      "show this help"},
    {"output",  NEED_ARG,   NULL,   'o',    arg_string, APTR(&G.outfile),   "output file with T/x/y/w"},
};

static cc_IMG *shimg = NULL, img = {0};
static FILE *out = NULL;

/**
 * @brief calc_bg - estimate background as "median + 1 sigma" over the whole frame
 * @param data (i) - 16-bit image data
 * @param n        - number of pixels
 * @return background level (median + 1 sigma)
 *
 * Uses a full 65536-bin histogram, so both median and sigma are computed in
 * O(n + 65536). The histogram lives in static storage because it is 512 KB
 * and we do not want it on the stack.
 */
static double calc_bg(const uint16_t *data, size_t n){
    static size_t hist[0x10000];
    memset(hist, 0, sizeof(hist));
    for(size_t i = 0; i < n; ++i) ++hist[data[i]];
    // Find median (upper median, standard convention)
    size_t half = (n + 1) / 2, cum = 0;
    int median = 0;
    for(int i = 0; i < 0x10000; ++i){
        cum += hist[i];
        if(cum >= half){ median = i; break; }
    }
    // Second central moment around median = robust sigma estimate
    double sq = 0.;
    for(int i = 0; i < 0x10000; ++i){
        if(!hist[i]) continue;
        double dv = (double)i - (double)median;
        sq += dv * dv * (double)hist[i];
    }
    double sigma = sqrt(sq / (double)n);
    double bg = (double)median + sigma * G.nsigmas;
    printf("bg: median=%d, sigma=%.2f, bg=%.2f\n", median, sigma, bg);
    return bg;
}

static void calcimg(){
    int H = img.h, W = img.w, npix = 0;
    double Xs = 0., X2s = 0., Ys = 0., Y2s = 0., Is = 0;
    static double tstart = -1.;
    if(tstart < 0.) tstart = img.timestamp;
    uint16_t *d = (uint16_t*)img.data;
    static double bg = -1.;
    double t0 = sl_dtime(); // measure one pass time
    if(G.background > 0.){
        bg = G.background;
    }else{
        bg = calc_bg(d, (size_t)W * H);
    }
    printf("bg=%g\n", bg);
    for(int y = 0; y < H; ++y){
        for(int x = 0; x < W; ++x){
            double val = (double)(*d++) - bg;
            if(val < DBL_EPSILON) continue;
            Xs += val * x; Ys += val * y; X2s += val * x * x; Y2s += val * y * y;
            Is += val;
            ++npix;
        }
    }
    DBG("Xs=%g, X2s=%g, Ys=%g, Y2s=%g, Is=%g\n", Xs, X2s, Ys, Y2s, Is);
    double xc = Xs/Is, yc = Ys/Is, sX = sqrt(X2s/Is-xc*xc), sY = sqrt(Y2s/Is-yc*yc);
    green("Xc = %.2f, Yc=%.2f, Xcs=%.2f, Ycs=%.2f, Isum=%.1f, Tcalcs=%gms; npix=%d\n",
          xc, yc, sX, sY, Is, (sl_dtime() - t0)*1e3, npix);
    if(out) fprintf(out, "%08.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
                    img.timestamp - tstart, xc, yc, Is, sX, sY);
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
    if(img.bitpix < 9){
        WARNX("Server sent 8-bit image, but 16-bit mode was requested; skip frame");
        return FALSE;
    }
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
    if(G.nsigmas < -5.) G.nsigmas = -5.;
    else if(G.nsigmas > 15.) G.nsigmas = 15.;
    cc_strbuff *cbuf = cc_strbufnew(BUFSIZ, STRBUFSZ);
    int sock = -1;
    if(G.port){
        sock = cc_open_socket(FALSE, G.sockname, G.port, TRUE);
    }else{
        sock = cc_open_socket(FALSE, G.sockname, NULL, FALSE);
    }
    if(sock < 0) ERR("Can't open command socket");
    int shmemkey = 0;
    if(CC_RESULT_OK == cc_getint(sock, cbuf, CC_CMD_SHMEMKEY, &shmemkey)){
        green("Got shm key: %d\n", shmemkey);
    }else{
        red("Can't read shmkey, try yours\n");
        shmemkey = G.shmkey;
    }
    // Explicitly request 16-bit mode (lowres=0)
    if(CC_RESULT_OK != cc_setint(sock, cbuf, CC_CMD_8BIT, 0)){
        ERRX("Can't set 16 bit mode");
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
            cc_setint(sock, cbuf, CC_CMD_8BIT, 0);
            usleep(1000);
            continue;
        }
        ++i;
        oldtime = now;
        printf("Got image #%zd, size %dx%d, bitpix %d, time %.2f\n",
               img.imnumber, img.w, img.h, img.bitpix, img.timestamp);
    }while(i < G.nframes);
    if(G.infty){
        if(CC_RESULT_OK != cc_setint(sock, cbuf, CC_CMD_INFTY, 0)) red("Can't clear INFTY\n");
    }
    if(xt > 0.){
        if(CC_RESULT_OK != cc_setfloat(sock, cbuf, CC_CMD_EXPOSITION, xt)) red("Can't return exptime to %gs\n", xt);
    }
    cc_strbufdel(&cbuf);
    if(out) fclose(out);
    close(sock);
    return 0;
}
