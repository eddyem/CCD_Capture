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
#define SENDMSG(...) do{snprintf(sendbuf, BUFSIZ-1, __VA_ARGS__); verbose(2, "%s", sendbuf); sendstrmessage(sock, sendbuf); getans(sock);}while(0)

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

static char *getans(int sock){
    static char buf[BUFSIZ];
    double t0 = dtime();
    char *ans = NULL;
    while(dtime() - t0 < ANSWER_TIMEOUT){
        if(1 != canberead(sock)) continue;
        int n = read(sock, buf, BUFSIZ-1);
        if(n == 0){
            WARNX("Server disconnected");
            signals(1);
        }
        ans = buf;
        buf[n] = 0;
        DBG("Got from server: %s", buf);
        verbose(1, "%s", buf);
        if(buf[n-1] == '\n'){
            buf[n-1] = 0;
            break;
        }
    }
    if(0 == strcmp(hresult2str(RESULT_BUSY), buf)){
        ERRX("Server busy");
    }
    return ans;
}

/**
 * @brief processData - process here some actions and make messages for server
 */
static void process_data(int sock){
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
    // CCD/CMOS
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
        SENDMSG(CMD_FILENAME "=%s", makeabspath(GP->outfile, FALSE));
        if(GP->rewrite) SENDMSG(CMD_REWRITE "=1");
        else SENDMSG(CMD_REWRITE "=0");
    }
    if(GP->outfileprefix) SENDMSG(CMD_FILENAMEPREFIX "=%s", makeabspath(GP->outfileprefix, FALSE));
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
    // common information
    SENDMSG(CMD_INFO);
}

void client(int sock){
    process_data(sock);
    double t0 = dtime(), tw = t0;
    int Nremain = 0, nframe = 1;
    // if client gives filename/prefix or Nframes, make exposition
    if(GP->outfile || GP->outfileprefix || GP->nframes > 0){
        Nremain = GP->nframes - 1;
        if(Nremain < 1) Nremain = 0;
        else GP->waitexpend = TRUE; // N>1 - wait for exp ends
        SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE);
    }
    double timeout = GP->waitexpend ? CLIENT_TIMEOUT : 0.1;
    verbose(1, "Exposing frame 1");
    if(GP->waitexpend) verbose(2, "Wait for exposition end");
    while(dtime() - t0 < timeout){
        if(GP->waitexpend && dtime() - tw > WAIT_TIMEOUT){
            SENDMSG(CMD_TREMAIN); // get remained time
            tw = dtime();
            sprintf(sendbuf, "%s", CMD_EXPSTATE);
            sendstrmessage(sock, sendbuf);
        }
        char *ans = getans(sock);
        if(ans){
            t0 = dtime();
            char *val = get_keyval(ans);
            if(val && 0 == strcmp(ans, CMD_EXPSTATE)){
                int state = atoi(val);
                if(state == CAMERA_ERROR){
                    WARNX(_("Can't make exposition"));
                    return;
                }
                if(state != CAMERA_CAPTURE){
                    verbose(2, "Frame ready!");
                    SENDMSG(CMD_LASTFNAME);
                    if(Nremain){
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
                        verbose(1, "Exposing frame %d", ++nframe);
                        --Nremain;
                        SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE);
                    }else{
                        GP->waitexpend = 0;
                        timeout = 0.2; // wait for last file name
                    }
                }
            }
        }
    }
    if(GP->waitexpend) WARNX(_("Server timeout"));
    DBG("Timeout");
}
