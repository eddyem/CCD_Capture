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

#include <stdatomic.h>
#include <netdb.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <usefull_macros.h>

#include "ccdfunc.h"
#include "cmdlnopts.h"
#include "server.h"
#include "socket.h"

static atomic_int camdevno = 0, wheeldevno = 0, focdevno = 0; // current devices numbers
static _Atomic camera_state camstate = CAMERA_IDLE;
#define FLAG_STARTCAPTURE       (1<<0)
#define FLAG_CANCEL             (1<<1)
static atomic_int camflags = 0, camfanspd = 0, confio = 0, nflushes;
static char *outfile = NULL;
static pthread_mutex_t locmutex = PTHREAD_MUTEX_INITIALIZER; // mutex for wheel/camera/focuser functions
static frameformat frmformatmax = {0}, curformat = {0}; // maximal format
static void *camdev = NULL, *focdev = NULL, *wheeldev = NULL;

static float focmaxpos = 0., focminpos = 0.; // focuser extremal positions
static int wmaxpos = 0.; // wheel max pos
static float tremain = 0.; // time when capture done

typedef struct{
    const char *key;
    const char *help;
}strpair;

// cat | awk '{print "{ " $3 ", \"\" }," }' | sort
strpair allcommands[] = {
    { "brightness", "camera brightness" },
    { "camdevno", "camera device number" },
    { "camlist", "list all connected cameras" },
    { "ccdfanspeed", "fan speed of camera" },
    { "confio", "camera IO configuration" },
    { "expstate", "get exposition state" },
    { "exptime", "exposition time" },
    { "filename", "save file with this name, like file.fits" },
    { "filenameprefix", "prefix of files, like ex (will be saved as exXXXX.fits)" },
    { "focdevno", "focuser device number" },
    { "foclist", "list all connected focusers" },
    { "focpos", "focuser position" },
    { "format", "camera frame format (X0,Y0,X1,Y1)" },
    { "gain", "camera gain" },
    { "hbin", "horizontal binning" },
    { "help", "show this help" },
    { "info", "connected devices state" },
    { "io", "get/set camera IO" },
    { "maxformat", "camera maximal available format" },
    { "nflushes", "camera number of preflushes" },
    { "rewrite", "rewrite file (if give `filename`, not `filenameprefix`" },
    { "shutter", "camera shutter's operations" },
    { "tcold", "camera chip temperature" },
    { "tremain", "time (in seconds) of exposition remained" },
    { "vbin", "vertical binning" },
    { "wdevno", "wheel device number" },
    { "wlist", "list all connected wheels" },
    { "wpos", "wheel position" },
    {NULL, NULL},
};

static IMG ima = {0};
static void fixima(){
    FNAME();
    int raw_width = curformat.w / GP->hbin,  raw_height = curformat.h / GP->vbin;
    if(ima.data && raw_width == ima.w && raw_height == ima.h) return; // all OK
    FREE(ima.data);
    DBG("curformat: %dx%d", curformat.w, curformat.h);
    ima.h = raw_height;
    ima.w = raw_width;
    ima.data = MALLOC(uint16_t, raw_width * raw_height);
    DBG("new image: %dx%d", raw_width, raw_height);
}

// functions for processCAM finite state machine
static inline void cameraidlestate(){ // idle - wait for capture commands
    if(camflags & FLAG_STARTCAPTURE){ // start capturing
        DBG("Start exposition");
        camflags &= ~(FLAG_STARTCAPTURE | FLAG_CANCEL);
        camstate = CAMERA_CAPTURE;
        camera->cancel();
        fixima();
        if(!camera->startexposition()){
            LOGERR("Can't start exposition");
            WARNX(_("Can't start exposition"));
            camstate = CAMERA_ERROR;
            return;
        }
    }
}
static inline void cameracapturestate(){ // capturing - wait for exposition ends
    if(camflags & FLAG_CANCEL){ // cancel all expositions
        DBG("Cancel exposition");
        camflags &= ~(FLAG_STARTCAPTURE | FLAG_CANCEL);
        camera->cancel();
        camstate = CAMERA_IDLE;
        return;
    }
    capture_status cs;
    if(camera->pollcapture(&cs, &tremain)){
        if(cs != CAPTURE_PROCESS){
            DBG("Capture ready");
            tremain = 0.;
            // now save frame
            if(!ima.data) LOGERR("Can't save image: not initialized");
            else{
                if(!camera->capture(&ima)) LOGERR("Can't capture image");
                else{
                    calculate_stat(&ima);
                    if(saveFITS(&ima)){
                        camstate = CAMERA_FRAMERDY;
                        return;
                    }
                }
            }
            camstate = CAMERA_ERROR;
        }
    }
}

// base camera thread
static void* processCAM(_U_ void *d){
    if(!camera) ERRX(_("No camera device"));
    double logt = 0;
    while(1){
        // log
        if(dtime() - logt > TLOG_PAUSE){
            logt = dtime();
            float t;
            if(camera->getTcold(&t)){
                LOGMSG("CCDTEMP=%f", t);
            }
            if(camera->getThot(&t)){
               LOGMSG("HOTTEMP=%f", t);
            }
            if(camera->getTbody(&t)){
               LOGMSG("BODYTEMP=%f", t);
            }
        }
        camera_state curstate = camstate;
        switch(curstate){
            case CAMERA_IDLE:
                cameraidlestate();
            break;
            case CAMERA_CAPTURE:
                cameracapturestate();
            break;
            case CAMERA_FRAMERDY:
// do nothing: when `server` got this state it sends "expstate=2" to all clients and changes state to IDLE
            break;
            case CAMERA_ERROR:
// do nothing: when `server` got this state it sends "expstate=3" to all clients and changes state to IDLE
            break;
        }
    }
    return NULL;
}

// functions running @ each devno change
static int camdevini(int n){
    if(!camera) return FALSE;
    pthread_mutex_lock(&locmutex);
    if(!camera->setDevNo(n)){
        LOGERR("Can't set active camera number");
        pthread_mutex_unlock(&locmutex);
        return FALSE;
    }
    camdevno = n;
    LOGMSG("Set camera device number to %d", camdevno);
    frameformat step;
    camera->getgeomlimits(&frmformatmax, &step);
    curformat = frmformatmax;
    if(GP->hbin < 1) GP->hbin = 1;
    if(GP->vbin < 1) GP->vbin = 1;
    fixima();
    if(!camera->setbin(GP->hbin, GP->vbin)) WARNX(_("Can't set binning %dx%d"), GP->hbin, GP->vbin);
    if(!camera->setgeometry(&curformat)) WARNX(_("Can't set given geometry"));
    pthread_mutex_unlock(&locmutex);
    return TRUE;
}
static int focdevini(int n){
    if(!focuser) return FALSE;
    pthread_mutex_lock(&locmutex);
    if(!focuser->setDevNo(n)){
        LOGERR("Can't set active focuser number");
        pthread_mutex_unlock(&locmutex);
        return FALSE;
    }
    focdevno = n;
    LOGMSG("Set focuser device number to %d", focdevno);
    focuser->getMaxPos(&focmaxpos);
    focuser->getMinPos(&focminpos);
    pthread_mutex_unlock(&locmutex);
    return TRUE;
}
static int wheeldevini(int n){
    if(!wheel) return FALSE;
    pthread_mutex_unlock(&locmutex);
    if(!wheel->setDevNo(n)){
        LOGERR("Can't set active wheel number");
        pthread_mutex_unlock(&locmutex);
        return FALSE;
    }
    wheeldevno = n;
    LOGMSG("Set wheel device number to %d", wheeldevno);
    wheel->getMaxPos(&wmaxpos);
    pthread_mutex_unlock(&locmutex);
    return TRUE;
}

/*******************************************************************************
 *************************** CCD/CMOS handlers *********************************
 ******************************************************************************/
static hresult camlisthandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[BUFSIZ], modname[256];
    pthread_mutex_lock(&locmutex);
    for(int i = 0; i < camera->Ndevices; ++i){
        if(!camera->setDevNo(i)) continue;
        camera->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_CAMLIST "='%s'", modname);
        sendstrmessage(fd, buf);
    }
    if(camdevno > -1) camera->setDevNo(camdevno);
    pthread_mutex_unlock(&locmutex);
    return RESULT_SILENCE;
}
static hresult camsetNhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > camera->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!camdevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_CAMDEVNO "=%d", camdevno);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
// exposition time setter/getter
static hresult exphandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        double v = atof(val);
        if(v < DBL_EPSILON) return RESULT_BADVAL;
        if(camstate != CAMERA_CAPTURE){
            pthread_mutex_lock(&locmutex);
            if(camera->setexp(v)){
                GP->exptime = v;
            }else LOGWARN("Can't set exptime to %g", v);
            pthread_mutex_unlock(&locmutex);
        }else return RESULT_BUSY;
    }
    snprintf(buf, 63, CMD_EXPOSITION "=%g", GP->exptime);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
// filename setter/getter
static hresult namehandler(int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+1];
    if(val){
        pthread_mutex_lock(&locmutex);
        char *path = makeabspath(val);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            pthread_mutex_unlock(&locmutex);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfile = outfile;
        GP->outfileprefix = NULL;
        pthread_mutex_unlock(&locmutex);
    }
    if(!GP->outfile) return RESULT_FAIL;
    snprintf(buf, PATH_MAX, CMD_FILENAME "=%s", GP->outfile);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
// filename prefix
static hresult nameprefixhandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+1];
    if(val){
        pthread_mutex_lock(&locmutex);
        char *path = makeabspath(val);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            pthread_mutex_unlock(&locmutex);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfileprefix = outfile;
        GP->outfile = NULL;
        pthread_mutex_unlock(&locmutex);
    }
    if(!GP->outfileprefix) return RESULT_FAIL;
    snprintf(buf, PATH_MAX, CMD_FILENAMEPREFIX "=%s", GP->outfileprefix);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
// rewrite
static hresult rewritefilehandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n < 0 || n > 1) return RESULT_BADVAL;
        pthread_mutex_lock(&locmutex);
        GP->rewrite = n;
        pthread_mutex_unlock(&locmutex);
    }
    snprintf(buf, 63, CMD_REWRITE "=%d", GP->rewrite);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult binhandler(_U_ int fd, const char *key, const char *val){
    char buf[64];
    if(val){
        int b = atoi(val);
        if(b < 1) return RESULT_BADVAL;
        if(0 == strcmp(key, CMD_HBIN)) GP->hbin = b;
        else GP->vbin = b;
        pthread_mutex_lock(&locmutex);
        if(!camera->setbin(GP->hbin, GP->vbin)){
            pthread_mutex_unlock(&locmutex);
            return RESULT_BADVAL;
        }
        fixima();
        pthread_mutex_unlock(&locmutex);
    }
    pthread_mutex_lock(&locmutex);
    int r = camera->getbin(&GP->hbin, &GP->vbin);
    pthread_mutex_unlock(&locmutex);
    if(r){
        if(0 == strcmp(key, CMD_HBIN)) snprintf(buf, 63, "%s=%d", key, GP->hbin);
        else snprintf(buf, 63, "%s=%d", key, GP->vbin);
        sendstrmessage(fd, buf);
        return RESULT_SILENCE;
    }
    return RESULT_FAIL;
}
static hresult temphandler(_U_ int fd, _U_ const char *key, const char *val){
    float f;
    char buf[64];
    int r;
    if(val){
        f = atof(val);
        pthread_mutex_lock(&locmutex);
        r = camera->setT((float)f);
        pthread_mutex_unlock(&locmutex);
        if(!r){
            LOGWARN("Can't set camera T to %.1f", f);
            return RESULT_FAIL;
        }
        LOGMSG("Set camera T to %.1f", f);
    }
    pthread_mutex_lock(&locmutex);
    r = camera->getTcold(&f);
    pthread_mutex_unlock(&locmutex);
    if(r){
        snprintf(buf, 63, CMD_CAMTEMPER "=%.1f", f);
        sendstrmessage(fd, buf);
        pthread_mutex_lock(&locmutex);
        r = camera->getTbody(&f);
        pthread_mutex_unlock(&locmutex);
        if(r){
            snprintf(buf, 63, "tbody=%.1f", f);
            sendstrmessage(fd, buf);
        }
        pthread_mutex_lock(&locmutex);
        r = camera->getThot(&f);
        pthread_mutex_unlock(&locmutex);
        if(r){
            snprintf(buf, 63, "thot=%.1f", f);
            sendstrmessage(fd, buf);
        }
        return RESULT_SILENCE;
    }else return RESULT_FAIL;
}
static hresult camfanhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int spd = atoi(val);
        if(spd < 0) return RESULT_BADVAL;
        if(spd > FAN_HIGH) spd = FAN_HIGH;
        pthread_mutex_lock(&locmutex);
        int r = camera->setfanspeed((fan_speed)spd);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
        camfanspd = spd;
    }
    snprintf(buf, 63, CMD_CAMFANSPD "=%d", camfanspd);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
const char *shutterstr[] = {"open", "close", "expose @high", "expose @low"};
static hresult shutterhandler(_U_ int fd, _U_ const char *key, const char *val){
    if(val){
        int x = atoi(val);
        if(x < 0 || x >= SHUTTER_AMOUNT) return RESULT_BADVAL;
        pthread_mutex_lock(&locmutex);
        int r = camera->shuttercmd((shutter_op)x);
        pthread_mutex_unlock(&locmutex);
        if(r){
           LOGMSG("Shutter command '%s'", shutterstr[x]);
        }else{
            LOGWARN("Can't run shutter command '%s'", shutterstr[x]);
            return RESULT_FAIL;
        }
    }
    return RESULT_OK;
}
static hresult confiohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int io = atoi(val);
        pthread_mutex_lock(&locmutex);
        int r = camera->confio(io);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
        confio = io;
    }
    snprintf(buf, 63, CMD_CONFIO "=%d", confio);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult iohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    int io;
    if(val){
        io = atoi(val);
        pthread_mutex_lock(&locmutex);
        int r = camera->setio(io);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
    }
    pthread_mutex_lock(&locmutex);
    int r = camera->getio(&io);
    pthread_mutex_unlock(&locmutex);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_IO "=%d", io);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult gainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float f;
    if(val){
        f = atof(val);
        pthread_mutex_lock(&locmutex);
        int r = camera->setgain(f);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
    }
    pthread_mutex_lock(&locmutex);
    int r = camera->getgain(&f);
    pthread_mutex_unlock(&locmutex);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_GAIN "=%.1f", f);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult brightnesshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float b;
    if(val){
        b = atof(val);
        pthread_mutex_lock(&locmutex);
        int r = camera->setbrightness(b);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
    }
    pthread_mutex_lock(&locmutex);
    int r = camera->getbrightness(&b);
    pthread_mutex_unlock(&locmutex);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_BRIGHTNESS "=%.1f", b);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
// set format: `format=X0,X1,Y0,Y1`
// get geomlimits: `maxformat=X0,X1,Y0,Y1`
static hresult formathandler(int fd, const char *key, const char *val){
    char buf[64];
    frameformat fmt;
    if(val){
        if(strcmp(key, CMD_FRAMEFORMAT)) return RESULT_BADKEY; // can't set maxformat
        if(4 != sscanf(val, "%d,%d,%d,%d", &fmt.xoff, &fmt.yoff, &fmt.w, &fmt.h)) return RESULT_BADVAL;
        fmt.w -= fmt.xoff; fmt.h -= fmt.yoff;
        pthread_mutex_lock(&locmutex);
        int r = camera->setgeometry(&fmt);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
        curformat = fmt;
        fixima();
    }
    if(0 == strcmp(key, CMD_FRAMEMAX)) snprintf(buf, 63, CMD_FRAMEMAX "=%d,%d,%d,%d",
        frmformatmax.xoff, frmformatmax.yoff, frmformatmax.xoff+frmformatmax.w, frmformatmax.yoff+frmformatmax.h);
    else snprintf(buf, 63, CMD_FRAMEFORMAT "=%d,%d,%d,%d",
        camera->geometry.xoff, camera->geometry.yoff, camera->geometry.xoff+camera->geometry.w, camera->geometry.yoff+camera->geometry.h);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult nflusheshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n < 1) return RESULT_BADVAL;
        pthread_mutex_lock(&locmutex);
        if(!camera->setnflushes(n)){
            pthread_mutex_unlock(&locmutex);
            return RESULT_FAIL;
        }
        nflushes = n;
        pthread_mutex_unlock(&locmutex);
    }
    snprintf(buf, 63, CMD_NFLUSHES "=%d", nflushes);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult expstatehandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n == CAMERA_IDLE){ // cancel expositions
            camflags |= FLAG_CANCEL;
            return RESULT_OK;
        }
        else if(n == CAMERA_CAPTURE){ // start exposition
            camflags |= FLAG_STARTCAPTURE;
            return RESULT_OK;
        }
        else return RESULT_BADVAL;
    }
    snprintf(buf, 63, CMD_EXPSTATE "=%d", camstate);
    sendstrmessage(fd, buf);
    snprintf(buf, 63, "camflags=%d", camflags);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult tremainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    snprintf(buf, 63, CMD_TREMAIN "=%g", tremain);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
/*
static hresult handler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    return RESULT_SILENCE;
}
*/
/*******************************************************************************
 ***************************** Wheel handlers **********************************
 ******************************************************************************/
static hresult wlisthandler(int fd, _U_ const char *key, _U_ const char *val){
    if(wheel->Ndevices < 1) return RESULT_FAIL;
    pthread_mutex_lock(&locmutex);
    for(int i = 0; i < wheel->Ndevices; ++i){
        if(!wheel->setDevNo(i)) continue;
        char modname[256], buf[BUFSIZ];
        wheel->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_WLIST "='%s'", modname);
        sendstrmessage(fd, buf);
    }
    if(wheeldevno > -1) wheel->setDevNo(wheeldevno);
    pthread_mutex_unlock(&locmutex);
    return RESULT_SILENCE;
}
static hresult wsetNhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > wheel->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!wheeldevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_WDEVNO "=%d", wheeldevno);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}

static hresult wgotohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    int pos;
    if(val){
        pos = atoi(val);
        pthread_mutex_lock(&locmutex);
        int r = wheel->setPos(pos);
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_BADVAL;
    }
    pthread_mutex_lock(&locmutex);
    int r = wheel->getPos(&pos);
    pthread_mutex_unlock(&locmutex);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_WPOS "=%d", pos);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}

/*******************************************************************************
 **************************** Focuser handlers *********************************
 ******************************************************************************/

static hresult foclisthandler(int fd, _U_ const char *key, _U_ const char *val){
    if(focuser->Ndevices < 1) return RESULT_FAIL;
    pthread_mutex_lock(&locmutex);
    for(int i = 0; i < focuser->Ndevices; ++i){
        char modname[256], buf[BUFSIZ];
        if(!focuser->setDevNo(i)) continue;
        focuser->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_FOCLIST "='%s'", modname);
        sendstrmessage(fd, buf);
    }
    if(focdevno > -1) focuser->setDevNo(focdevno);
    pthread_mutex_unlock(&locmutex);
    return RESULT_SILENCE;
}
static hresult fsetNhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > focuser->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!focdevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_FDEVNO "=%d", focdevno);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}
static hresult fgotohandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    float f;
    int r;
    if(val){
        f = atof(val);
        if(f < focminpos || f > focmaxpos) return RESULT_BADVAL;
        pthread_mutex_lock(&locmutex);
        if(f - focminpos < __FLT_EPSILON__){
            r = focuser->home(1);
        }else{
            r = focuser->setAbsPos(1, f);
        }
        pthread_mutex_unlock(&locmutex);
        if(!r) return RESULT_FAIL;
    }
    pthread_mutex_lock(&locmutex);
    r = focuser->getPos(&f);
    pthread_mutex_unlock(&locmutex);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_FGOTO "=%g", f);
    sendstrmessage(fd, buf);
    return RESULT_SILENCE;
}

/*******************************************************************************
 **************************** Common handlers **********************************
 ******************************************************************************/

// information about everything
static hresult infohandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[BUFSIZ], buf1[256];
    float f;
    int i;
    if(camera){
        if(camera->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CMD_CAMLIST "='%s'", buf1);
            sendstrmessage(fd, buf);
        }
        namehandler(fd, CMD_FILENAME, NULL);
        binhandler(fd, CMD_HBIN, NULL);
        binhandler(fd, CMD_VBIN, NULL);
        temphandler(fd, CMD_CAMTEMPER, NULL);
        exphandler(fd, CMD_EXPOSITION, NULL);
    }
    if(wheel){
        if(wheel->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CMD_WLIST "='%s'", buf1);
            sendstrmessage(fd, buf);
        }
        if(wheel->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "wtemp=%.1f", f);
            sendstrmessage(fd, buf);
        }
        if(wheel->getPos(&i)){
            snprintf(buf, BUFSIZ-1, CMD_WPOS "=%d", i);
            sendstrmessage(fd, buf);
        }
        snprintf(buf, BUFSIZ-1, "wmaxpos=%d", wmaxpos);
        sendstrmessage(fd, buf);
    }
    if(focuser){
        if(focuser->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CMD_FOCLIST "='%s'", buf1);
            sendstrmessage(fd, buf);
        }
        if(focuser->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "foctemp=%.1f", f);
            sendstrmessage(fd, buf);
        }
        snprintf(buf, BUFSIZ-1, "focminpos=%g", focminpos);
        sendstrmessage(fd, buf);
        snprintf(buf, BUFSIZ-1, "focmaxpos=%g", focmaxpos);
        sendstrmessage(fd, buf);
        if(focuser->getPos(&f)){
            snprintf(buf, BUFSIZ-1, CMD_FGOTO "=%g", f);
            sendstrmessage(fd, buf);
        }
    }
    return RESULT_SILENCE;
}
// show help
static hresult helphandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[256];
    strpair *ptr = allcommands;
    while(ptr->key){
        snprintf(buf, 255, "%s - %s", ptr->key, ptr->help);
        sendstrmessage(fd, buf);
        ++ptr;
    }
    return RESULT_SILENCE;
}

// for setters: do nothing when camera not in idle state
static int CAMbusy(){
    if(camera && camstate != CAMERA_IDLE) return TRUE;
    return TRUE;
}

static int chktrue(_U_ char *val){
    return TRUE;
}
static int chkcam(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(camera) return TRUE;
    return FALSE;
}
static int chkwheel(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(wheel) return TRUE;
    return FALSE;
}
static int chkfoc(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(focuser) return TRUE;
    return FALSE;
}
static handleritem items[] = {
    {chktrue, infohandler, CMD_INFO},
    {chktrue, helphandler, CMD_HELP},
    {chkcam, camlisthandler, CMD_CAMLIST},
    {chkcam, camsetNhandler, CMD_CAMDEVNO},
    {chkcam, camfanhandler, CMD_CAMFANSPD},
    {chkcam, exphandler, CMD_EXPOSITION},
    {chkcam, namehandler, CMD_FILENAME},
    {chkcam, binhandler, CMD_HBIN},
    {chkcam, binhandler, CMD_VBIN},
    {chkcam, temphandler, CMD_CAMTEMPER},
    {chkcam, shutterhandler, CMD_SHUTTER},
    {chkcam, confiohandler, CMD_CONFIO},
    {chkcam, iohandler, CMD_IO},
    {chkcam, gainhandler, CMD_GAIN},
    {chkcam, brightnesshandler, CMD_BRIGHTNESS},
    {chkcam, formathandler, CMD_FRAMEFORMAT},
    {chkcam, formathandler, CMD_FRAMEMAX},
    {chkcam, nflusheshandler, CMD_NFLUSHES},
    {chkcam, expstatehandler, CMD_EXPSTATE},
    {chkcam, nameprefixhandler, CMD_FILENAMEPREFIX},
    {chkcam, rewritefilehandler, CMD_REWRITE},
    {chktrue, tremainhandler, CMD_TREMAIN},
    {chkfoc, foclisthandler, CMD_FOCLIST},
    {chkfoc, fsetNhandler, CMD_FDEVNO},
    {chkfoc, fgotohandler, CMD_FGOTO},
    {chkwheel, wlisthandler, CMD_WLIST},
    {chkwheel, wsetNhandler, CMD_WDEVNO},
    {chkwheel, wgotohandler, CMD_WPOS},
    {NULL, NULL, NULL},
};

#define CLBUFSZ     BUFSIZ

void server(int sock){
    // init everything
    startCCD(&camdev);
    camdevini(0);
    startFocuser(&focdev);
    focdevini(0);
    startWheel(&wheeldev);
    wheeldevini(0);
    if(listen(sock, MAXCLIENTS) == -1){
        WARN("listen");
        LOGWARN("listen");
        return;
    }
    // start camera thread
    pthread_t camthread;
    if(camera){
        if(pthread_create(&camthread, NULL, processCAM, NULL)) ERR("pthread_create()");
    }
    int nfd = 1; // only one socket @start
    struct pollfd poll_set[MAXCLIENTS+1];
    char buffers[MAXCLIENTS][CLBUFSZ]; // buffers for data reading
    bzero(poll_set, sizeof(poll_set));
    // ZERO - listening server socket
    poll_set[0].fd = sock;
    poll_set[0].events = POLLIN;
    while(1){
        poll(poll_set, nfd, 1); // max timeout - 1ms
        if(poll_set[0].revents & POLLIN){ // check main for accept()
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);
            int client = accept(sock, (struct sockaddr*)&addr, &len);
            DBG("New connection");
            LOGMSG("SERVER got connection, fd=%d", client);
            if(nfd == MAXCLIENTS + 1){
                LOGWARN("Max amount of connections, disconnect fd=%d", client);
                WARNX("Limit of connections reached");
                close(client);
            }else{
                memset(&poll_set[nfd], 0, sizeof(struct pollfd));
                poll_set[nfd].fd = client;
                poll_set[nfd].events = POLLIN;
                ++nfd;
            }
        }
        // process some data & send messages to ALL
        if(camstate == CAMERA_FRAMERDY || camstate == CAMERA_ERROR){
            char buff[32];
            snprintf(buff, 31, CMD_EXPSTATE "=%d", camstate);
            DBG("Send %s to %d clients", buff, nfd - 1);
            for(int i = 1; i < nfd; ++i)
                sendstrmessage(poll_set[i].fd, buff);
            camstate = CAMERA_IDLE;
        }
        // scan connections
        for(int fdidx = 1; fdidx < nfd; ++fdidx){
            if((poll_set[fdidx].revents & POLLIN) == 0) continue;
            int fd = poll_set[fdidx].fd;
            if(!processData(fd, items, buffers[fdidx-1], CLBUFSZ)){ // socket closed
                DBG("Client fd=%d disconnected", fd);
                LOGMSG("SERVER client fd=%d disconnected", fd);
                buffers[fdidx-1][0] = 0; // clear rest of data in buffer
                close(fd);
                // move last FD to current position
                poll_set[fdidx] = poll_set[nfd - 1];
                --nfd;
            }
        }
    }
    focclose(focdev);
    closewheel(wheeldev);
    closecam(camdev);
}

char *makeabspath(const char *path){
    static char buf[PATH_MAX+1];
    if(!path) return NULL;
    char *ret = NULL;
    int unl = 0;
    FILE *f = fopen(path, "r");
    if(!f){
        f = fopen(path, "a");
        if(!f){
            WARN("Can't create %s", path);
            return NULL;
        }
        unl = 1;
    }
    if(!realpath(path, buf)){
        WARN("realpath()");
        return NULL;
    }else ret = buf;
    fclose(f);
    if(unl) unlink(path);
    return ret;
}
