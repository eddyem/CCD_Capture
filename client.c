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
    }
    return ans;
}

static char *makeabspath(const char *path){
    static char buf[PATH_MAX];
    if(!path) return NULL;
    char *ret = NULL;
    int unl = 0;
    FILE *f = fopen(path, "r");
    if(!f){
        f = fopen(path, "a");
        if(!f){
            ERR("Can't create %s", path);
            return NULL;
        }
        unl = 1;
    }
    if(!realpath(path, buf)){
        ERR("realpath()");
    }else ret = buf;
    fclose(f);
    if(unl) unlink(path);
    return ret;
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
    if(GP->rewrite) SENDMSG(CMD_REWRITE "=1");
    else SENDMSG(CMD_REWRITE "=0");
    if(GP->outfile) SENDMSG(CMD_FILENAME "=%s", makeabspath(GP->outfile));
    if(GP->outfileprefix) SENDMSG(CMD_FILENAMEPREFIX "=%s", makeabspath(GP->outfileprefix));
    // if client gives filename and exptime, make exposition
    if(GP->exptime > -DBL_EPSILON){
        SENDMSG(CMD_EXPOSITION "=%g", GP->exptime);
        if(GP->outfile || GP->outfileprefix) SENDMSG(CMD_EXPSTATE "=%d", CAMERA_CAPTURE);
    }
    // common information
    SENDMSG(CMD_INFO);
}

void client(int sock){
    process_data(sock);
    if(!GP->waitexpend) return;
    double t0 = dtime(), tw = t0;
    while(dtime() - t0 < CLIENT_TIMEOUT){
        if(GP->waitexpend && dtime() - tw > WAIT_TIMEOUT){
            SENDMSG(CMD_TREMAIN); // get remained time
            tw = dtime();
            sprintf(sendbuf, "%s", CMD_EXPSTATE);
            verbose(2, "%s", sendbuf);
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
                    return;
                }
            }
        }
    }
    WARNX(_("Server timeout"));
    DBG("Timeout");
}
