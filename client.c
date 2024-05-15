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

// client-side functions
#include <stdatomic.h>
#include <math.h>  // isnan
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <usefull_macros.h>

#include "ccdfunc.h" // framerate
#include "client.h"
#include "cmdlnopts.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif
#include "server.h" // for common commands names
#include "socket.h"

extern double answer_timeout;

static char sendbuf[BUFSIZ];
// send message and wait any answer
#define SENDMSG(...) do{DBG("SENDMSG"); snprintf(sendbuf, BUFSIZ-1, __VA_ARGS__); verbose(2, "\t> %s", sendbuf); cc_sendstrmessage(sock, sendbuf); while(getans(sock, NULL));} while(0)
// send message and wait answer starting with 'cmd'
#define SENDMSGW(cmd, ...) do{DBG("SENDMSGW"); snprintf(sendbuf, BUFSIZ-1, cmd __VA_ARGS__); verbose(2, "\t> %s", sendbuf); cc_sendstrmessage(sock, sendbuf);}while(!getans(sock, cmd))
// send command and wait for answer on it
#define SENDCMDW(cmd) do{DBG("SENDCMDW"); strncpy(sendbuf, cmd, BUFSIZ-1); verbose(2, "\t> %s", sendbuf); cc_sendstrmessage(sock, sendbuf);}while(!getans(sock, cmd))
static volatile atomic_int expstate = CAMERA_CAPTURE;
static int xm0,ym0,xm1,ym1; // max format
static int xc0,yc0,xc1,yc1; // current format

#ifdef IMAGEVIEW
static volatile atomic_int grabno = 0, oldgrabno = 0;
// IPC key for shared memory
static cc_IMG ima = {0}, *shmima = NULL; // ima - local storage, shmima - shm (if available)
static size_t imbufsz = 0; // image buffer for allocated `ima`
static uint8_t *imbuf = NULL;
#endif

// read message from queue or file descriptor
static char *readmsg(int fd){
    static cc_strbuff *buf = NULL;
    if(!buf) buf = cc_strbufnew(BUFSIZ, 256);
    int test(){
        size_t got = cc_getline(buf);
        if(got > 255){
            DBG("Client fd=%d gave buffer overflow", fd);
            LOGMSG("SERVER client fd=%d buffer overflow", fd);
        }else if(got){
            return TRUE;
        }
        return FALSE;
    }
    if(test()) return buf->string;
    if(1 == cc_canberead(fd)){
        if(cc_read2buf(fd, buf)){
            if(test()) return buf->string;
        }else ERRX("Server disconnected");
    }
    return NULL;
}

// parser of CCD server messages; return TRUE to exit from polling cycle of `getans` (if receive 'FAIL', 'OK' or 'BUSY')
static int parseans(char *ans){
    if(!ans) return FALSE;
    //TIMESTAMP("parseans() begin");
    //DBG("Parsing of '%s'", ans);
    if(0 == strcmp(cc_hresult2str(RESULT_BUSY), ans)){
        WARNX("Server busy");
        return FALSE;
    }
    if(0 == strcmp(cc_hresult2str(RESULT_FAIL), ans)) return TRUE;
    if(0 == strcmp(cc_hresult2str(RESULT_OK), ans)) return TRUE;
    char *val = cc_get_keyval(&ans); // now `ans` is a key and `val` its value
    if(0 == strcmp(CC_CMD_EXPSTATE, ans)){
        expstate = atoi(val);
        DBG("Exposition state: %d", expstate);
        return TRUE;
    }else if(0 == strcmp(CC_CMD_FRAMEMAX, ans)){
        sscanf(val, "%d,%d,%d,%d", &xm0, &ym0, &xm1, &ym1);
        DBG("Got maxformat: %d,%d,%d,%d", xm0, ym0, xm1, ym1);
        return TRUE;
    }else if(0 == strcmp(CC_CMD_FRAMEFORMAT, ans)){
        sscanf(val, "%d,%d,%d,%d", &xc0, &yc0, &xc1, &yc1);
        DBG("Got current format: %d,%d,%d,%d", xc0, yc0, xc1, yc1);
        return TRUE;
    }else if(0 == strcmp(CC_CMD_INFTY, ans)) return TRUE;
    //TIMESTAMP("parseans() end");
    return FALSE;
}

// read until timeout all messages from server; return FALSE if there was no messages from server
// if msg != NULL - wait for it in answer
static int getans(int sock, const char *msg){
    double t0 = dtime();
    char *ans = NULL;
    while(dtime() - t0 < answer_timeout){
        char *s = readmsg(sock);
        if(!s) continue;
        t0 = dtime();
        ans = s;
        TIMESTAMP("Got from server: %s", ans);
        verbose(1, "\t%s", ans);
DBG("1 msg-> %s, ans -> %s", msg, ans);
        if(parseans(ans)){
            DBG("2 msg-> %s, ans -> %s", msg, ans);
            if(msg && strncmp(ans, msg, strlen(msg))) continue;
            DBG("BREAK");
            break;
        }

    }
    return ((ans) ? TRUE : FALSE);
}

/**
 * @brief processData - process here some actions and make messages for server
 */
static void send_headers(int sock){
    if(GP->plugincmd){
        char **p = GP->plugincmd;
        green("Send custom plugin commands\n");
        while(p && *p){
            printf("\t%s\n", *p);
            SENDMSGW(CC_CMD_PLUGINCMD, "=%s", *p);
            ++p;
        }
        while(getans(sock, NULL));
    }
    if(GP->exptime > -DBL_EPSILON) SENDMSGW(CC_CMD_EXPOSITION, "=%g", GP->exptime);
    DBG("infty=%d", GP->infty);
    if(GP->infty > -1) SENDMSGW(CC_CMD_INFTY, "=%d", GP->infty);
    // common information
    SENDMSG(CC_CMD_INFO);
    // focuser
    if(GP->listdevices) SENDMSG(CC_CMD_FOCLIST);
    if(GP->focdevno > -1) SENDMSG(CC_CMD_FDEVNO "=%d", GP->focdevno);
    if(!isnan(GP->gotopos)){
        SENDMSGW(CC_CMD_FGOTO, "=%g", GP->gotopos);
    }
    // wheel
    if(GP->listdevices) SENDCMDW(CC_CMD_WLIST);
    if(GP->whldevno > -1) SENDMSGW(CC_CMD_WDEVNO, "=%d", GP->whldevno);
    if(GP->setwheel > -1) SENDMSGW(CC_CMD_WPOS, "=%d", GP->setwheel);
    DBG("nxt");
    // CCD/CMOS
    if(GP->X0 > INT_MIN || GP->Y0 > INT_MIN || GP->X1 > INT_MIN || GP->Y1 > INT_MIN){ // set format
        SENDCMDW(CC_CMD_FRAMEMAX);
        SENDCMDW(CC_CMD_FRAMEFORMAT);
        // default values
        if(GP->X0 == INT_MIN) GP->X0 = xc0;
        if(GP->X1 == INT_MIN) GP->X1 = xc1;
        if(GP->Y0 == INT_MIN) GP->Y0 = yc0;
        if(GP->Y1 == INT_MIN) GP->Y1 = yc1;
        // limiting values
        if(GP->X0 < 0) GP->X0 = xm0;
        else if(GP->X0 > xm1-1) GP->X0 = xm1-1;
        if(GP->Y0 < 0) GP->Y0 = ym0;
        else if(GP->Y0 > ym1-1) GP->Y0 = ym1-1;
        if(GP->X1 < GP->X0+1 || GP->X1 > xm1) GP->X1 = xm1;
        if(GP->Y1 < GP->Y0+1 || GP->Y1 > ym1) GP->Y1 = ym1;
        DBG("set format: (%d,%d)x(%d,%d)", GP->X0,GP->X1,GP->Y0,GP->Y1);
        SENDMSGW(CC_CMD_FRAMEFORMAT, "=%d,%d,%d,%d", GP->X0, GP->Y0, GP->X1, GP->Y1);
    }
    if(GP->cancelexpose) SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_IDLE);
    if(GP->listdevices) SENDCMDW(CC_CMD_CAMLIST);
    if(GP->camdevno > -1) SENDMSGW(CC_CMD_CAMDEVNO, "=%d", GP->camdevno);
    if(GP->hbin) SENDMSGW(CC_CMD_HBIN, "=%d", GP->hbin);
    if(GP->vbin) SENDMSGW(CC_CMD_VBIN, "=%d", GP->vbin);
    if(!isnan(GP->temperature)) SENDMSGW(CC_CMD_CAMTEMPER, "=%g", GP->temperature);
    if(GP->shtr_cmd > -1) SENDMSGW(CC_CMD_SHUTTER, "=%d", GP->shtr_cmd);
    if(GP->confio > -1) SENDMSGW(CC_CMD_CONFIO, "=%d", GP->confio);
    if(GP->setio > -1) SENDMSGW(CC_CMD_IO, "=%d", GP->setio);\
    if(!isnan(GP->gain)) SENDMSGW(CC_CMD_GAIN, "=%g", GP->gain);
    if(!isnan(GP->brightness)) SENDMSGW(CC_CMD_BRIGHTNESS, "=%g", GP->brightness);
    if(GP->nflushes > 0) SENDMSGW(CC_CMD_NFLUSHES, "=%d", GP->nflushes);
    if(GP->exptime > -DBL_EPSILON){ // exposition and reading control: only if start of exposition
        if(GP->_8bit) SENDMSGW(CC_CMD_8BIT, "=1");
        else SENDMSGW(CC_CMD_8BIT, "=0");
        if(GP->fast) SENDMSGW(CC_CMD_FASTSPD, "=1");
        else SENDMSGW(CC_CMD_FASTSPD, "=0");
        if(GP->dark) SENDMSGW(CC_CMD_DARK, "=1");
        else SENDMSGW(CC_CMD_DARK, "=0");
    }
    if(GP->outfile){
        if(!*GP->outfile) SENDMSGW(CC_CMD_FILENAME, "=");
        else SENDMSGW(CC_CMD_FILENAME, "=%s", makeabspath(GP->outfile, FALSE));
        if(GP->rewrite) SENDMSGW(CC_CMD_REWRITE, "=1");
        else SENDMSGW(CC_CMD_REWRITE, "=0");
    }
    if(GP->outfileprefix){
        if(!*GP->outfileprefix) SENDMSGW(CC_CMD_FILENAMEPREFIX, "=");
        else SENDMSGW(CC_CMD_FILENAMEPREFIX, "=%s", makeabspath(GP->outfileprefix, FALSE));
    }
    // FITS header keywords:
#define CHKHDR(x, cmd)   do{if(x) SENDMSG(cmd "=%s", x);}while(0)
    CHKHDR(GP->author, CC_CMD_AUTHOR);
    CHKHDR(GP->instrument, CC_CMD_INSTRUMENT);
    CHKHDR(GP->observers, CC_CMD_OBSERVER);
    CHKHDR(GP->objname, CC_CMD_OBJECT);
    CHKHDR(GP->prog_id, CC_CMD_PROGRAM);
    CHKHDR(GP->objtype, CC_CMD_OBJTYPE);
#undef CHKHDR
    if(GP->addhdr){
        char buf[1024], *ptr = buf, **sptr = GP->addhdr;
        *buf = 0;
        int L = 1024;
        while(*sptr){
            if(!**sptr){
                ++sptr; continue;
            }
            int N = snprintf(ptr, L-1, "%s,", *sptr++);
            L -= N; ptr += N;
        }
        SENDMSGW(CC_CMD_HEADERFILES, "=%s", buf);
    }
}

void client(int sock){
    if(GP->restart){
        SENDCMDW(CC_CMD_RESTART);
        return;
    }
    send_headers(sock);
    double t0 = dtime(), tw = t0;
    int Nremain = 0, nframe = 1;
    // if client gives filename/prefix or Nframes, make exposition
    if((GP->outfile && *GP->outfile) || (GP->outfileprefix && *GP->outfileprefix) || GP->nframes > 0){
        Nremain = GP->nframes - 1;
        if(Nremain < 1) Nremain = 0;
        else GP->waitexpend = TRUE; // N>1 - wait for exp ends
        SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE);
    }else{
        int cntr = 0;
        while(dtime() - t0 < CC_WAIT_TIMEOUT && cntr < 3)
            if(!getans(sock, NULL)) ++cntr;
        DBG("RETURN: no more data");
        return;
    }
    double timeout = GP->waitexpend ? CC_CLIENT_TIMEOUT : CC_WAIT_TIMEOUT;
    verbose(1, "Exposing frame 1...");
    if(GP->waitexpend){
        expstate = CAMERA_CAPTURE; // could be changed earlier
        verbose(2, "Wait for exposition end");
    }
    while(dtime() - t0 < timeout){
        if(GP->waitexpend && dtime() - tw > CC_WAIT_TIMEOUT){
            SENDCMDW(CC_CMD_TREMAIN); // get remained time
            tw = dtime();
            sprintf(sendbuf, "%s", CC_CMD_EXPSTATE);
            cc_sendstrmessage(sock, sendbuf);
        }
        if(getans(sock, NULL)){ // got next portion of data
            DBG("server message");
            t0 = dtime();
            if(expstate == CAMERA_ERROR){
                WARNX(_("Can't make exposition"));
                continue;
            }
            //if(expstate != CAMERA_CAPTURE){
            if(expstate == CAMERA_FRAMERDY){
                verbose(2, "Frame ready!");
                expstate = CAMERA_IDLE;
                if(Nremain){
                    verbose(1, "\n");
                    if(GP->pause_len > 0){
                        double delta, time1 = dtime() + GP->pause_len;
                        while(1){
                            SENDCMDW(CC_CMD_CAMTEMPER);
                            if((delta = time1 - dtime()) < __FLT_EPSILON__) break;
                            if(delta > 1.) verbose(1, _("%d seconds till pause ends\n"), (int)delta);
                            if(delta > 6.) sleep(5);
                            else if(delta > 1.) sleep((int)delta);
                            else usleep((int)(delta*1e6 + 1));
                        }
                    }
                    verbose(1, "Exposing frame %d...", ++nframe);
                    --Nremain;
                    SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE);
                }else{
                    GP->waitexpend = 0;
                    timeout = answer_timeout; // wait for last file name
                }
            }
        }
    }
    if(GP->waitexpend) WARNX(_("Server timeout"));
    DBG("Timeout");
}

#ifdef IMAGEVIEW
static int controlfd = -1; // control socket FD
void init_grab_sock(int sock){
    if(sock < 0) ERRX("Can't run without command socket");
    controlfd = sock;
    send_headers(sock);
    if(!GP->forceimsock && !shmima){ // init shm buffer if user don't ask to work through image socket
        shmima = cc_getshm(GP->shmkey, 0); // try to init client shm
    }
}

/**
 * @brief readNbytes - read `N` bytes from descriptor `fd` into buffer *buf
 * @return false if failed
 */
static int readNbytes(int fd, size_t N, uint8_t *buf){
    size_t got = 0, need = N;
    double t0 = dtime();
    while(dtime() - t0 < CC_CLIENT_TIMEOUT /*&& cc_canberead(fd)*/ && need){
        ssize_t rd = read(fd, buf + got, need);
        if(rd <= 0){
            WARNX("Server disconnected");
            signals(1);
        }
        got += rd; need -= rd;
    }
    if(need) return FALSE; // didn't got whole packet
    return TRUE;
}

/**
 * @brief getimage - read image from shared memory or socket
 */
static void getimage(){
    FNAME();
    int imsock = -1;
    static double oldtimestamp = -1.;
    TIMESTAMP("Get image sizes");
    /*SENDCMDW(CC_CMD_IMWIDTH);
    SENDCMDW(CC_CMD_IMHEIGHT);*/
    if(shmima){ // read image from shared memory
        memcpy(&ima, shmima, sizeof(cc_IMG));
    }else{ // get image by socket
        imsock = cc_open_socket(FALSE, GP->imageport, TRUE);
        if(imsock < 0) ERRX("getimage(): can't open image transport socket");
        // get image size
        if(!readNbytes(imsock, sizeof(cc_IMG), (uint8_t*)&ima)){
            WARNX("Can't read image header");
            goto eofg;
        }
    }
    if(ima.bytelen < 1){
        WARNX("Wrong image size");
        goto eofg;
    }
    DBG("bytelen=%zd, w=%d, h=%d; bitpix=%d", ima.bytelen, ima.w, ima.h, ima.bitpix);
    // realloc memory if needed
    if(imbufsz < ima.bytelen){
        size_t newsz = 1024 * (1 + ima.bytelen / 1024);
        DBG("Reallocate memory from %zd to %zd", imbufsz, newsz);
        imbufsz = newsz;
        imbuf = realloc(imbuf, imbufsz);
    }
    ima.data = imbuf; // renew this value each time after getting `ima` from server
    TIMESTAMP("Start of data read");
    if(shmima){
        uint8_t *datastart = (uint8_t*)shmima + sizeof(cc_IMG);
        memcpy(imbuf, datastart, ima.bytelen);
        TIMESTAMP("Got by shared memory");
    }else{
        if(!readNbytes(imsock, ima.bytelen, imbuf)){
            WARNX("Can't read image data");
            goto eofg;
        }
        TIMESTAMP("Got by socket");
    }
    DBG("timestamps old-new=%g; imno: %zd", oldtimestamp-ima.timestamp, ima.imnumber);
    if(ima.timestamp != oldtimestamp){ // test if image is really new
        oldtimestamp = ima.timestamp;
        grabno = ima.imnumber;
        TIMESTAMP("Got image #%zd", ima.imnumber);
    }else WARNX("Still got old image");
eofg:
    if(!shmima) close(imsock);
}

static void *grabnext(void _U_ *arg){ // daemon grabbing images through the net
    FNAME();
    if(controlfd < 0) return NULL;
    int sock = controlfd;
    while(1){
        if(!getWin()) exit(1);
        TIMESTAMP("End of cycle, start new");
        expstate = CAMERA_CAPTURE;
        TIMEINIT();
        SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE); // start capture
        double timeout = GP->exptime + CC_CLIENT_TIMEOUT, t0 = dtime();
        useconds_t sleept = 500000; // 0.5s
        if(GP->exptime < 0.5){
            sleept = (useconds_t)(GP->exptime * 500000.);
            if(sleept < 1000) sleept = 1000;
        }
//        double exptime = GP->exptime;
        while(dtime() - t0 < timeout){
            TIMESTAMP("Wait for exposition ends (%u us)", sleept);
            usleep(sleept);
            TIMESTAMP("check answer");
            getans(sock, NULL);
            //TIMESTAMP("EXPSTATE ===> %d", expstate);
            if(expstate != CAMERA_CAPTURE) break;
            if(dtime() - t0 < GP->exptime + 0.5) sleept = 1000;
        }
        if(dtime() - t0 >= timeout || expstate != CAMERA_FRAMERDY){
            WARNX("Image wasn't received");
            continue;
        }
        TIMESTAMP("Frame ready");
        getimage();
    }
    return NULL;
}

static void *waitimage(void _U_ *arg){ // passive waiting for next image
    FNAME();
    if(controlfd < 0) return NULL;
    int sock = controlfd;
    while(1){
        if(!getWin()) exit(1);
        getans(sock, NULL);
        if(expstate != CAMERA_FRAMERDY){
            usleep(1000);
            continue;
        }
        TIMESTAMP("End of cycle, start new");
        TIMEINIT();
        getimage();
        expstate = CAMERA_IDLE;
    }
    return NULL;
}

// try to capture images through socket
int sockcaptured(cc_IMG **imgptr){
    //TIMESTAMP("sockcaptured() start");
    if(!imgptr) return FALSE;
    static pthread_t grabthread = 0;
    if(controlfd < 0) return FALSE;
    if(imgptr == (void*)-1){ // kill `grabnext`
        DBG("Wait for grabbing thread");
        if(grabthread){
            pthread_cancel(grabthread);
            pthread_join(grabthread, NULL);
            grabthread = 0;
        }
        DBG("OK");
        return FALSE;
    }
    if(!grabthread || pthread_kill(grabthread, 0)){ // start new grab
        if(GP->viewer){
            TIMEINIT();
            DBG("\n\n\nStart new waiting");
            if(pthread_create(&grabthread, NULL, &waitimage, NULL)){
                WARN("Can't create waiting thread");
                grabthread = 0;
           }
        }else{
            TIMEINIT();
            DBG("\n\n\nStart new grab");
            if(pthread_create(&grabthread, NULL, &grabnext, NULL)){
                WARN("Can't create grabbing thread");
                grabthread = 0;
           }
        }
    }else{ // grab in process
        if(grabno != oldgrabno){ // image is ready
            TIMESTAMP("Image #%d ready", grabno);
            if(*imgptr && (*imgptr != &ima)) free(*imgptr);
            *imgptr = &ima; /*
            ssize_t delta = ima.imnumber - oldgrabno;
            if(delta > 0 && delta != 1) WARNX("sockcaptured(): missed %zd images", delta-1);*/
            oldgrabno = grabno;
            framerate();
            //TIMESTAMP("sockcaptured() end, return TRUE");
            return TRUE;
        }
    }
    //TIMESTAMP("sockcaptured() end, return FALSE");
    return FALSE;
}
// IMAGEVIEW
#endif
