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
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <usefull_macros.h>

#include "client.h"
#include "cmdlnopts.h"
#include "server.h" // for common commands names
#include "socket.h"

static char sendbuf[BUFSIZ];
#define SENDMSG(...) do{snprintf(sendbuf, BUFSIZ-1, __VA_ARGS__); verbose(2, "\t> %s", sendbuf); sendstrmessage(sock, sendbuf); getans(sock);}while(0)
static volatile atomic_int expstate = CAMERA_CAPTURE;
static int xm0,ym0,xm1,ym1; // max format
static int xc0,yc0,xc1,yc1; // current format

#ifdef IMAGEVIEW
static IMG ima = {0};
static volatile atomic_int grabends = 0;
static int imdatalen = 0, imbufsz = 0;
#endif

/**
 * check data from  fd (polling function for client)
 * @param fd - file descriptor
 * @return 0 in case of timeout, 1 in case of fd have data, -1 if error
 */
static int canberead(int fd){
    fd_set fds;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    do{
        int rc = select(fd+1, &fds, NULL, NULL, &timeout);
        if(rc < 0){
            if(errno != EINTR){
                LOGWARN("select()");
                WARN("select()");
                return -1;
            }
            continue;
        }
        break;
    }while(1);
    if(FD_ISSET(fd, &fds)){
        return 1;
    }
    return 0;
}

static char *readmsg(int fd){
    static char buf[BUFSIZ] = {0}, line[BUFSIZ];
    int curlen = strlen(buf);
    if(curlen == BUFSIZ-1) curlen = 0; // buffer overflow - clear old content
    ssize_t rd = 0;
    if(1 == canberead(fd)){
        rd = read(fd, buf + curlen, BUFSIZ-1 - curlen);
            if(rd <= 0){
                WARNX("Server disconnected");
                signals(1);
            }
    }
    curlen += rd;
    buf[curlen] = 0;
    if(curlen == 0) return NULL;
    //DBG("cur buffer: ----%s----", buf);
    char *nl = strchr(buf, '\n');
    if(!nl) return NULL;
    *nl++ = 0;
    strcpy(line, buf);
    int rest = curlen - (int)(nl-buf);
    if(rest > 0) memmove(buf, nl, rest+1);
    else *buf = 0;
    return line;
}

// parser of CCD server messages; return TRUE to exit from polling cycle of `getans` (if receive 'FAIL', 'OK' or 'BUSY')
static int parseans(char *ans){
    if(!ans) return FALSE;
    //DBG("Parsing of '%s'", ans);
    if(0 == strcmp(hresult2str(RESULT_BUSY), ans)) ERRX("Server busy");
    if(0 == strcmp(hresult2str(RESULT_FAIL), ans)) return TRUE;
    if(0 == strcmp(hresult2str(RESULT_OK), ans)) return TRUE;
    char *val = get_keyval(ans); // now `ans` is a key and `val` its value
    if(0 == strcmp(CMD_EXPSTATE, ans)){
        expstate = atoi(val);
        DBG("Exposition state: %d", expstate);
    }else if(0 == strcmp(CMD_FRAMEMAX, ans)){
        sscanf(val, "%d,%d,%d,%d", &xm0, &ym0, &xm1, &ym1);
        DBG("Got maxformat: %d,%d,%d,%d", xm0, ym0, xm1, ym1);
    }else if(0 == strcmp(CMD_FRAMEFORMAT, ans)){
        sscanf(val, "%d,%d,%d,%d", &xc0, &yc0, &xc1, &yc1);
        DBG("Got current format: %d,%d,%d,%d", xc0, yc0, xc1, yc1);
    }
#ifdef IMAGEVIEW
    else if(0 == strcmp(CMD_IMWIDTH, ans)){
        ima.w = atoi(val);
        DBG("Get width: %d", ima.w);
        imdatalen = ima.w * ima.h * 2;
    }else if(0 == strcmp(CMD_IMHEIGHT, ans)){
        ima.h = atoi(val);
        DBG("Get height: %d", ima.h);
        imdatalen = ima.w * ima.h * 2;
    }
#endif
    return FALSE;
}

// read until timeout all messages from server; return FALSE if there was no messages from server
static int getans(int sock){
    double t0 = dtime();
    char *ans = NULL;
    while(dtime() - t0 < ANSWER_TIMEOUT){
        char *s = readmsg(sock);
        if(!s){ // buffer is empty, return last message or wait for it
            if(ans) return TRUE;
            else continue;
        }
        t0 = dtime();
        ans = s;
        DBG("Got from server: %s", ans);
        verbose(1, "\t%s", ans);
        if(parseans(ans)) break;
    }
    DBG("GETANS: timeout, ans: %s", ans);
    return ((ans) ? TRUE : FALSE);
}

/**
 * @brief processData - process here some actions and make messages for server
 */
static void send_headers(int sock){
    // common information
    SENDMSG(CMD_INFO);
    // focuser
    if(GP->listdevices) SENDMSG(CMD_FOCLIST);
    if(GP->focdevno > -1) SENDMSG(CMD_FDEVNO "=%d", GP->focdevno);
    if(!isnan(GP->gotopos)){
        SENDMSG(CMD_FGOTO "=%g", GP->gotopos);
    }
    // wheel
    if(GP->listdevices) SENDMSG(CMD_WLIST);
    if(GP->whldevno > -1) SENDMSG(CMD_WDEVNO "=%d", GP->whldevno);
    if(GP->setwheel > -1) SENDMSG(CMD_WPOS "=%d", GP->setwheel);
    DBG("nxt");
    // CCD/CMOS
    if(GP->X0 > -1 || GP->Y0 > -1 || GP->X1 > -1 || GP->Y1 > -1){ // set format
        SENDMSG(CMD_FRAMEMAX);
        SENDMSG(CMD_FRAMEFORMAT);
        if(GP->X0 < 0) GP->X0 = xc0; // default values
        else if(GP->X0 > xm1-1) GP->X0 = xm1-1;
        if(GP->Y0 < 0) GP->Y0 = yc0;
        else if(GP->Y0 > ym1-1) GP->Y0 = ym1-1;
        if(GP->X1 < GP->X0+1) GP->X1 = xc1;
        else if(GP->X1 > xm1) GP->X1 = xm1;
        if(GP->Y1 < GP->Y0+1) GP->Y1 = yc1;
        else if(GP->Y1 > ym1) GP->Y1 = ym1;
        DBG("set format: (%d,%d)x(%d,%d)", GP->X0,GP->X1,GP->Y0,GP->Y1);
        SENDMSG(CMD_FRAMEFORMAT "=%d,%d,%d,%d", GP->X0, GP->Y0, GP->X1, GP->Y1);
    }
    if(GP->cancelexpose) SENDMSG(CMD_EXPSTATE "=%d", CAMERA_IDLE);
    if(GP->listdevices) SENDMSG(CMD_CAMLIST);
    if(GP->camdevno > -1) SENDMSG(CMD_CAMDEVNO "=%d", GP->camdevno);
    if(GP->hbin) SENDMSG(CMD_HBIN "=%d", GP->hbin);
    if(GP->vbin) SENDMSG(CMD_VBIN "=%d", GP->vbin);
    if(!isnan(GP->temperature)) SENDMSG(CMD_CAMTEMPER "=%g", GP->temperature);
    if(GP->shtr_cmd > -1) SENDMSG(CMD_SHUTTER "=%d", GP->shtr_cmd);
    if(GP->confio > -1) SENDMSG(CMD_CONFIO "=%d", GP->confio);
    if(GP->setio > -1) SENDMSG(CMD_IO "=%d", GP->setio);\
    if(!isnan(GP->gain)) SENDMSG(CMD_GAIN "=%g", GP->gain);
    if(!isnan(GP->brightness)) SENDMSG(CMD_BRIGHTNESS "=%g", GP->brightness);
    if(GP->nflushes > 0) SENDMSG(CMD_NFLUSHES "=%d", GP->nflushes);
    if(GP->outfile || GP->outfileprefix){ // exposition and reading control: only if start of exposition
        if(GP->_8bit) SENDMSG(CMD_8BIT "=1");
        else SENDMSG(CMD_8BIT "=0");
        if(GP->fast) SENDMSG(CMD_FASTSPD "=1");
        else SENDMSG(CMD_FASTSPD "=0");
        if(GP->dark) SENDMSG(CMD_DARK "=1");
        else SENDMSG(CMD_DARK "=0");
    }
    if(GP->outfile){
        if(!*GP->outfile) SENDMSG(CMD_FILENAME "=");
        else SENDMSG(CMD_FILENAME "=%s", makeabspath(GP->outfile, FALSE));
        if(GP->rewrite) SENDMSG(CMD_REWRITE "=1");
        else SENDMSG(CMD_REWRITE "=0");
    }
    if(GP->outfileprefix){
        if(!*GP->outfileprefix) SENDMSG(CMD_FILENAMEPREFIX "=");
        else SENDMSG(CMD_FILENAMEPREFIX "=%s", makeabspath(GP->outfileprefix, FALSE));
    }
    if(GP->exptime > -DBL_EPSILON) SENDMSG(CMD_EXPOSITION "=%g", GP->exptime);
    // FITS header keywords:
#define CHKHDR(x, cmd)   do{if(x) SENDMSG(cmd "=%s", x);}while(0)
    CHKHDR(GP->author, CMD_AUTHOR);
    CHKHDR(GP->instrument, CMD_INSTRUMENT);
    CHKHDR(GP->observers, CMD_OBSERVER);
    CHKHDR(GP->objname, CMD_OBJECT);
    CHKHDR(GP->prog_id, CMD_PROGRAM);
    CHKHDR(GP->objtype, CMD_OBJTYPE);
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
        SENDMSG(CMD_HEADERFILES "=%s", buf);
    }
}

void client(int sock){
    if(GP->restart){
        SENDMSG(CMD_RESTART);
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
        SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE);
    }else{
        double t0 = dtime();
        while(getans(sock) && dtime() - t0 < WAIT_TIMEOUT);
        DBG("RETURN: no more data");
        return;
    }
    double timeout = GP->waitexpend ? CLIENT_TIMEOUT : WAIT_TIMEOUT;
    verbose(1, "Exposing frame 1...");
    if(GP->waitexpend){
        expstate = CAMERA_CAPTURE; // could be changed earlier
        verbose(2, "Wait for exposition end");
    }
    while(dtime() - t0 < timeout){
        if(GP->waitexpend && dtime() - tw > WAIT_TIMEOUT){
            SENDMSG(CMD_TREMAIN); // get remained time
            tw = dtime();
            sprintf(sendbuf, "%s", CMD_EXPSTATE);
            sendstrmessage(sock, sendbuf);
        }
        if(getans(sock)){ // got next portion of data
            DBG("server message");
            t0 = dtime();
            if(expstate == CAMERA_ERROR){
                WARNX(_("Can't make exposition"));
                return;
            }
            if(expstate != CAMERA_CAPTURE){
                verbose(2, "Frame ready!");
                if(Nremain){
                    verbose(1, "\n");
                    if(GP->pause_len > 0){
                        double delta, time1 = dtime() + GP->pause_len;
                        while(1){
                            SENDMSG(CMD_CAMTEMPER);
                            if((delta = time1 - dtime()) < __FLT_EPSILON__) break;
                            // %d секунд до окончания паузы\n
                            if(delta > 1.) verbose(1, _("%d seconds till pause ends\n"), (int)delta);
                            if(delta > 6.) sleep(5);
                            else if(delta > 1.) sleep((int)delta);
                            else usleep((int)(delta*1e6 + 1));
                        }
                    }
                    verbose(1, "Exposing frame %d...", ++nframe);
                    --Nremain;
                    SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE);
                }else{
                    GP->waitexpend = 0;
                    timeout = WAIT_TIMEOUT; // wait for last file name
                }
            }
        }
    }
    if(GP->waitexpend) WARNX(_("Server timeout"));
    DBG("Timeout");
}

#ifdef IMAGEVIEW
static int grabsockfd = -1;
void init_grab_sock(int sock){
    grabsockfd = sock;
    send_headers(sock);
}

static void getimage(){
    int sock = grabsockfd;
    SENDMSG(CMD_IMWIDTH);
    SENDMSG(CMD_IMHEIGHT);
    while(readmsg(sock)); // clear all incoming data
    sendstrmessage(sock, CMD_GETIMAGE); // ask for image
    if(imbufsz < imdatalen){
        DBG("Reallocate memory from %d to %d", imbufsz, imdatalen);
        ima.data = realloc(ima.data, imdatalen);
        imbufsz = imdatalen;
    }
    double t0 = dtime();
    int got = 0;
    while(dtime() - t0 < CLIENT_TIMEOUT){
        if(!canberead(sock)) continue;
        uint8_t *target = ((uint8_t*)ima.data)+got;
        int rd = read(sock, target, imdatalen - got);
        if(rd <= 0){
            WARNX("Server disconnected");
            signals(1);
        }
        got += rd;
        DBG("Read %d bytes; total read %d from %d; first: %x %x %x %x", rd, got, imdatalen, target[0],
                target[1], target[2], target[3]);
        if(got == imdatalen){
            DBG("Got image");
            grabends = 1;
            break;
        }
    }
    if(dtime() - t0 > CLIENT_TIMEOUT) WARNX("Timeout, image didn't received");
}

static void *grabnext(void _U_ *arg){ // daemon grabbing images through the net
    FNAME();
    if(grabsockfd < 0) return NULL;
    int sock = grabsockfd;
    while(1){
        DBG("WAIT");
        while(grabends); // wait until image processed
        expstate = CAMERA_CAPTURE;
        DBG("CAPT");
        SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE); // start capture
        double timeout = GP->exptime + CLIENT_TIMEOUT, t0 = dtime();
        useconds_t sleept = 500000; // 0.5s
        if(GP->exptime < 0.5){
            sleept = (useconds_t)(GP->exptime * 500000.);
            if(sleept < 1000) sleept = 1000;
        }
        while(dtime() - t0 < timeout){
            DBG("SLEEP!");
            usleep(sleept);
            //SENDMSG(CMD_EXPSTATE);
            getans(sock);
            DBG("EXPSTATE ===> %d", expstate);
            if(expstate != CAMERA_CAPTURE) break;
        }
        if(dtime() - t0 >= timeout || expstate != CAMERA_FRAMERDY){
            WARNX("Image wasn't received");
            continue;
        }
        DBG("Frame ready");
        getimage();
    }
    return NULL;
}

static void *waitimage(void _U_ *arg){ // passive waiting for next image
    FNAME();
    if(grabsockfd < 0) return NULL;
    int sock = grabsockfd;
    while(1){
        while(grabends); // wait until image processed
        getans(sock);
        if(expstate != CAMERA_FRAMERDY){
            usleep(1000);
            continue;
        }
        getimage();
    }
    return NULL;
}

// try to capture images through socket
int sockcaptured(IMG **imgptr){
    if(!imgptr) return FALSE;
    static pthread_t grabthread = 0;
    if(grabsockfd < 0) return FALSE;
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
    if(!grabthread){ // start new grab
        if(GP->viewer){
            DBG("\n\n\nStart new waiting");
            if(pthread_create(&grabthread, NULL, &waitimage, NULL)){
                WARN("Can't create waiting thread");
                grabthread = 0;
           }
        }else{
            DBG("\n\n\nStart new grab");
            if(pthread_create(&grabthread, NULL, &grabnext, NULL)){
                WARN("Can't create grabbing thread");
                grabthread = 0;
           }
        }
    }else{ // grab in process
        if(grabends){ // image is ready
            DBG("Image ready");
            grabthread = 0;
            if(*imgptr && (*imgptr != &ima)) free(*imgptr);
            *imgptr = &ima;
            grabends = 0;
            return TRUE;
        }
    }
    return FALSE;
}
// IMAGEVIEW
#endif
