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

static int parsestring(int fd, cc_handleritem *handlers, char *str);

static atomic_int camdevno = 0, wheeldevno = 0, focdevno = 0; // current devices numbers
static _Atomic cc_camera_state camstate = CAMERA_IDLE;
#define FLAG_STARTCAPTURE       (1<<0)
#define FLAG_CANCEL             (1<<1)
#define FLAG_RESTARTSERVER      (1<<2)
static atomic_int camflags = 0, camfanspd = 0, confio = 0, nflushes, infty = 0;
static char *outfile = NULL, *lastfile = NULL; // current output file name/prefix; last name of saved file
static cc_frameformat frmformatmax = {0}, curformat = {0}; // maximal format

static float focmaxpos = 0., focminpos = 0.; // focuser extremal positions
static int wmaxpos = 0.; // wheel max pos
static float tremain = 0.; // time when capture done

// IPC key for shared memory
static key_t shmkey = IPC_PRIVATE;

typedef struct{
    const char *key;
    const char *help;
}strpair;

// cat | awk '{print "{ " $3 ", \"\" }," }' | sort
strpair allcommands[] = {
    { CC_CMD_8BIT,         "run in 8 bit mode instead of 16 bit" },
    { CC_CMD_AUTHOR,       "FITS 'AUTHOR' field" },
    { CC_CMD_BRIGHTNESS,   "camera brightness" },
    { CC_CMD_CAMDEVNO,     "camera device number" },
    { CC_CMD_CAMLIST,      "list all connected cameras" },
    { CC_CMD_CAMFANSPD,    "fan speed of camera" },
    { CC_CMD_CONFIO,       "camera IO configuration" },
    { CC_CMD_DARK,         "don't open shutter @ exposure" },
    { CC_CMD_EXPSTATE,     "get exposition state" },
    { CC_CMD_EXPOSITION,   "exposition time" },
    { CC_CMD_FASTSPD,      "fast readout speed" },
    { CC_CMD_FILENAME,     "save file with this name, like file.fits" },
    { CC_CMD_FILENAMEPREFIX,"prefix of files, like ex (will be saved as exXXXX.fits)" },
    { CC_CMD_FDEVNO,       "focuser device number" },
    { CC_CMD_FOCLIST,      "list all connected focusers" },
    { CC_CMD_FGOTO,        "focuser position" },
    { CC_CMD_FRAMEFORMAT,  "camera frame format (X0,Y0,X1,Y1)" },
    { CC_CMD_GAIN,         "camera gain" },
    { CC_CMD_GETHEADERS,   "get last file FITS headers" },
    { CC_CMD_HBIN,         "horizontal binning" },
    { CC_CMD_HEADERFILES,  "add FITS records from these files (comma-separated list)" },
    { CC_CMD_HELP,         "show this help" },
    { CC_CMD_IMHEIGHT,     "last image height" },
    { CC_CMD_IMWIDTH,      "last image width" },
    { CC_CMD_INFO,         "connected devices state" },
    { CC_CMD_INFTY,        "an infinity loop taking images until there's connected clients" },
    { CC_CMD_INSTRUMENT,   "FITS 'INSTRUME' field" },
    { CC_CMD_IO,           "get/set camera IO" },
    { CC_CMD_LASTFNAME,    "path to last saved file"},
    { CC_CMD_FRAMEMAX,     "camera maximal available format" },
    { CC_CMD_NFLUSHES,     "camera number of preflushes" },
    { CC_CMD_OBJECT,       "FITS 'OBJECT' field" },
    { CC_CMD_OBJTYPE,      "FITS 'IMAGETYP' field" },
    { CC_CMD_OBSERVER,     "FITS 'OBSERVER' field" },
    { CC_CMD_PLUGINCMD,    "custom camera plugin command" },
    { CC_CMD_PROGRAM,      "FITS 'PROG-ID' field" },
    { CC_CMD_RESTART,      "restart server" },
    { CC_CMD_REWRITE,      "rewrite file (if give `filename`, not `filenameprefix`)" },
    { CC_CMD_SHMEMKEY,     "get shared memory key" },
    { CC_CMD_SHUTTER,      "camera shutter's operations" },
    { CC_CMD_CAMTEMPER,    "camera chip temperature" },
    { CC_CMD_TREMAIN,      "time (in seconds) of exposition remained" },
    { CC_CMD_VBIN,         "vertical binning" },
    { CC_CMD_WDEVNO,       "wheel device number" },
    { CC_CMD_WLIST,        "list all connected wheels" },
    { CC_CMD_WPOS,         "wheel position" },
    {NULL, NULL},
};

static pthread_mutex_t locmutex = PTHREAD_MUTEX_INITIALIZER; // mutex for wheel/camera/focuser functions

// return TRUE if `locmutex` can be locked
static int lock(){
    if(pthread_mutex_trylock(&locmutex)){
        //DBG("\n\nAlready locked");
        return FALSE;
    }
    return TRUE;
}
static void unlock(){
    if(pthread_mutex_unlock(&locmutex)) ERR("Can't unlock mutex");
}

static cc_IMG *ima = NULL;

static void fixima(){
    FNAME();
    if(!camera) return;
    int raw_width = curformat.w / GP->hbin,  raw_height = curformat.h / GP->vbin;
    // allocate memory for largest possible image
    if(!ima) ima = cc_getshm(GP->shmkey, camera->array.h * camera->array.w * 2);
    if(!ima) ERRX("Can't allocate memory for image");
    shmkey = GP->shmkey;
    //if(raw_width == ima->w && raw_height == ima->h) return; // all OK
    DBG("curformat: %dx%d", curformat.w, curformat.h);
    ima->h = raw_height;
    ima->w = raw_width;
    if(!camera->getbitpix || !camera->getbitpix(&ima->bitpix)) ima->bitpix = 16;
    if(ima->bitpix < 8 || ima->bitpix > 16) ima->bitpix = 16; // use maximum in any strange cases
    ima->bytelen = raw_height * raw_width * cc_getNbytes(ima);
    DBG("new image: %dx%d", raw_width, raw_height);
}

// functions for processCAM finite state machine
static inline void cameraidlestate(){ // idle - wait for capture commands
    if(camflags & FLAG_STARTCAPTURE){ // start capturing
        TIMESTAMP("Start exposition");
        camflags &= ~(FLAG_STARTCAPTURE | FLAG_CANCEL);
        camstate = CAMERA_CAPTURE;
        fixima();
        if(!camera->startexposition){
            LOGERR(_("Camera plugin have no function `start exposition`"));
            WARNX(_("Camera plugin have no function `start exposition`"));
            camstate = CAMERA_ERROR;
            return;
        }
        if(!camera->startexposition()){
            LOGERR(_("Can't start exposition"));
            WARNX(_("Can't start exposition"));
            camstate = CAMERA_ERROR;
            return;
        }
    }
}
static inline void cameracapturestate(){ // capturing - wait for exposition ends
    cc_capture_status cs;
    if(camera->pollcapture && camera->pollcapture(&cs, &tremain)){
        if(cs != CAPTURE_PROCESS){
            TIMESTAMP("Capture ready");
            tremain = 0.;
            // now save frame
            if(!ima->data) LOGERR("Can't save image: not initialized");
            else{
                TIMESTAMP("start capture");
                if(!camera->capture){
                    WARNX(_("Camera plugin have no function `capture`"));
                    LOGERR(_("Camera plugin have no function `capture`"));
                    camstate = CAMERA_ERROR;
                    return;
                }
                if(!camera->capture(ima)){
                    LOGERR("Can't capture image");
                    camstate = CAMERA_ERROR;
                    return;
                }else{
                    ima->gotstat = 0; // fresh image without statistics - recalculate when save
                    ima->timestamp = dtime(); // set timestamp
                    ++ima->imnumber; // increment counter
                    if(saveFITS(ima, &lastfile)){
                        DBG("LAST file name changed");
                    }
                    TIMESTAMP("Image saved");
                }
            }
            camstate = CAMERA_FRAMERDY;
        }
    }
}

// base camera thread
static void* processCAM(_U_ void *d){
    if(!camera){
        LOGERR("No camera device");
        ERRX(_("No camera device"));
    }
    double logt = 0;
    while(1){
        if(camflags & FLAG_RESTARTSERVER){
            LOGERR("User asks to restart");
            signals(1);
        }
        usleep(100);
        if(tremain < 0.5 && tremain > 0.) usleep(tremain*1e6);
        if(lock()){
            // log
            if(dtime() - logt > TLOG_PAUSE){
                logt = dtime();
                float t;
                if(camera->getTcold && camera->getTcold(&t)){
                    LOGMSG("CCDTEMP=%.1f", t);
                }
                if(camera->getThot && camera->getThot(&t)){
                   LOGMSG("HOTTEMP=%.1f", t);
                }
                if(camera->getTbody && camera->getTbody(&t)){
                   LOGMSG("BODYTEMP=%.1f", t);
                }
            }
            if(camflags & FLAG_CANCEL){ // cancel all expositions
                DBG("Cancel exposition");
                LOGMSG("User canceled exposition");
                camflags &= ~(FLAG_STARTCAPTURE | FLAG_CANCEL);
                if(camera->cancel) camera->cancel();
                camstate = CAMERA_IDLE;
                infty = 0; // also cancel infinity loop
                unlock();
                continue;
            }
            cc_camera_state curstate = camstate;
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
            unlock();
        }
    }
    return NULL;
}

// functions running @ each devno change
static int camdevini(int n){
    if(!camera) return FALSE;
    if(camera->setDevNo && !camera->setDevNo(n)){
        LOGERR("Can't set active camera number");
        return FALSE;
    }
    camdevno = n;
    LOGMSG("Set camera device number to %d", camdevno);
    cc_frameformat step;
    if(camera->getgeomlimits) camera->getgeomlimits(&frmformatmax, &step);
    curformat = frmformatmax;
    DBG("\n\nGeometry format max (offx/offy) w/h: (%d/%d) %d/%d", curformat.xoff, curformat.yoff,
        curformat.w, curformat.h);
//    curformat.w -= curformat.xoff;
//    curformat.h -= curformat.yoff;
    curformat.xoff = 0;
    curformat.yoff = 0;
    if(GP->hbin < 1) GP->hbin = 1;
    if(GP->vbin < 1) GP->vbin = 1;
    fixima();
    if(!camera->setbin || !camera->setbin(GP->hbin, GP->vbin)) WARNX(_("Can't set binning %dx%d"), GP->hbin, GP->vbin);
    if(!camera->setgeometry || !camera->setgeometry(&curformat)) WARNX(_("Can't set given geometry"));
    return TRUE;
}
static int focdevini(int n){
    if(!focuser) return FALSE;
    if(!focuser->setDevNo(n)){
        LOGERR("Can't set active focuser number");
        return FALSE;
    }
    focdevno = n;
    LOGMSG("Set focuser device number to %d", focdevno);
    focuser->getMaxPos(&focmaxpos);
    focuser->getMinPos(&focminpos);
    return TRUE;
}
static int wheeldevini(int n){
    if(!wheel) return FALSE;
    if(!wheel->setDevNo(n)){
        LOGERR("Can't set active wheel number");
        return FALSE;
    }
    wheeldevno = n;
    LOGMSG("Set wheel device number to %d", wheeldevno);
    wheel->getMaxPos(&wmaxpos);
    return TRUE;
}

/*******************************************************************************
 *************************** Service handlers **********************************
 ******************************************************************************/
static cc_hresult restarthandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    camflags |= FLAG_RESTARTSERVER;
    return RESULT_OK;
}

/*******************************************************************************
 *************************** CCD/CMOS handlers *********************************
 ******************************************************************************/
// image size
static cc_hresult imsizehandler(int fd, const char *key, _U_ const char *val){
    char buf[64];
    // send image width/height in pixels
    if(0 == strcmp(key, CC_CMD_IMHEIGHT)) snprintf(buf, 63, CC_CMD_IMHEIGHT "=%d", ima->h);
    else snprintf(buf, 63, CC_CMD_IMWIDTH "=%d", ima->w);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult camlisthandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[BUFSIZ], modname[256];
    if(!camera->getModelName) return RESULT_FAIL;
    for(int i = 0; i < camera->Ndevices; ++i){
        if(camera->setDevNo && !camera->setDevNo(i)) continue;
            camera->getModelName(modname, 255);
            snprintf(buf, BUFSIZ-1, CC_CMD_CAMLIST "='%s'", modname);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(camdevno > -1 && camera->setDevNo) camera->setDevNo(camdevno);
    return RESULT_SILENCE;
}
static cc_hresult camsetNhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > camera->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!camdevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CC_CMD_CAMDEVNO "=%d", camdevno);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// exposition time setter/getter
static cc_hresult exphandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        DBG("setexp to %s", val);
        double v = atof(val);
        if(v < DBL_EPSILON) return RESULT_BADVAL;
        if(!camera->setexp) return RESULT_FAIL;
        if(camera->setexp(v)){
            GP->exptime = v;
        }else LOGWARN("Can't set exptime to %g", v);
    }
    DBG("expt: %g", GP->exptime);
    snprintf(buf, 63, CC_CMD_EXPOSITION "=%g", GP->exptime);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// show last filename of saved FITS
static cc_hresult lastfnamehandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[PATH_MAX+32];
    if(lastfile && *lastfile) snprintf(buf, PATH_MAX+31, CC_CMD_LASTFNAME "=%s", lastfile);
    else snprintf(buf, PATH_MAX+31, CC_CMD_LASTFNAME "=");
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// filename setter/getter
static cc_hresult namehandler(int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+32];
    DBG("filename=%s", val);
    if(val && *val){
        DBG("Make abs path");
        char *path = makeabspath(val, FALSE);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfile = outfile;
        GP->outfileprefix = NULL;
    }else{ // clear names
        DBG("Clear names");
        GP->outfileprefix = NULL;
        GP->outfile = NULL;
        return RESULT_OK;
    }
    if(!GP->outfile) return RESULT_FAIL;
    snprintf(buf, PATH_MAX+31, CC_CMD_FILENAME "=%s", GP->outfile);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// filename prefix
static cc_hresult nameprefixhandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+32];
    DBG("filename prefix=%s", val);
    if(val){
        char *path = makeabspath(val, FALSE);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfileprefix = outfile;
        GP->outfile = NULL;
    }else{ // clear names
        GP->outfileprefix = NULL;
        GP->outfile = NULL;
        return RESULT_OK;
    }
    if(!GP->outfileprefix) return RESULT_FAIL;
    snprintf(buf, PATH_MAX+31, CC_CMD_FILENAMEPREFIX "=%s", GP->outfileprefix);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// rewrite
static cc_hresult rewritefilehandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n < 0 || n > 1) return RESULT_BADVAL;
        GP->rewrite = n;
    }
    snprintf(buf, 63, CC_CMD_REWRITE "=%d", GP->rewrite);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult binhandler(_U_ int fd, const char *key, const char *val){
    char buf[64];
    if(val){
        int b = atoi(val);
        if(b < 1) return RESULT_BADVAL;
        if(0 == strcmp(key, CC_CMD_HBIN)) GP->hbin = b;
        else GP->vbin = b;
        if(!camera->setbin) return RESULT_FAIL;
        if(!camera->setbin(GP->hbin, GP->vbin)){
            return RESULT_BADVAL;
        }
    }
    if(!camera->getbin) return RESULT_SILENCE;
    int r = camera->getbin(&GP->hbin, &GP->vbin);
    if(r){
        if(0 == strcmp(key, CC_CMD_HBIN)) snprintf(buf, 63, "%s=%d", key, GP->hbin);
        else snprintf(buf, 63, "%s=%d", key, GP->vbin);
        if(val) fixima();
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        return RESULT_SILENCE;
    }
    return RESULT_FAIL;
}
static cc_hresult temphandler(int fd, _U_ const char *key, const char *val){
    float f;
    char buf[64];
    int r;
    if(!camera->setT) return RESULT_FAIL;
    if(val){
        f = atof(val);
        r = camera->setT((float)f);
        if(!r){
            LOGWARN("Can't set camera T to %.1f", f);
            return RESULT_FAIL;
        }
        LOGMSG("Set camera T to %.1f", f);
    }
    if(!camera->getTcold) return RESULT_SILENCE;
    r = camera->getTcold(&f);
    if(r){
        snprintf(buf, 63, CC_CMD_CAMTEMPER "=%.1f", f);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        if(camera->getTbody){
            r = camera->getTbody(&f);
            if(r){
                snprintf(buf, 63, "tbody=%.1f", f);
                if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
            }
        }
        if(camera->getThot){
            r = camera->getThot(&f);
            if(r){
                snprintf(buf, 63, "thot=%.1f", f);
                if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
            }
        }
        return RESULT_SILENCE;
    }else return RESULT_FAIL;
}
static cc_hresult camfanhandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(!camera->setfanspeed) return RESULT_FAIL;
    if(val){
        int spd = atoi(val);
        if(spd < 0) return RESULT_BADVAL;
        if(spd > FAN_HIGH) spd = FAN_HIGH;
        int r = camera->setfanspeed((cc_fan_speed)spd);
        if(!r) return RESULT_FAIL;
        camfanspd = spd;
    }
    snprintf(buf, 63, CC_CMD_CAMFANSPD "=%d", camfanspd);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
const char *shutterstr[] = {"open", "close", "expose @high", "expose @low"};
static cc_hresult shutterhandler(_U_ int fd, _U_ const char *key, const char *val){
    if(!camera->shuttercmd) return RESULT_FAIL;
    if(val){
        int x = atoi(val);
        if(x < 0 || x >= SHUTTER_AMOUNT) return RESULT_BADVAL;
        int r = camera->shuttercmd((cc_shutter_op)x);
        if(r){
           LOGMSG("Shutter command '%s'", shutterstr[x]);
        }else{
            LOGWARN("Can't run shutter command '%s'", shutterstr[x]);
            return RESULT_FAIL;
        }
    }
    return RESULT_OK;
}
static cc_hresult confiohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(!camera->confio) return RESULT_FAIL;
    if(val){
        int io = atoi(val);
        int r = camera->confio(io);
        if(!r) return RESULT_FAIL;
        confio = io;
    }
    snprintf(buf, 63, CC_CMD_CONFIO "=%d", confio);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult iohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    int io;
    if(!camera->setio) return RESULT_FAIL;
    if(val){
        io = atoi(val);
        int r = camera->setio(io);
        if(!r) return RESULT_FAIL;
    }
    int r = camera->getio(&io);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_IO "=%d", io);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult gainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float f;
    if(!camera->setgain) return RESULT_FAIL;
    if(val){
        f = atof(val);
        int r = camera->setgain(f);
        if(!r) return RESULT_FAIL;
    }
    if(!camera->getgain) return RESULT_SILENCE;
    int r = camera->getgain(&f);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_GAIN "=%.1f", f);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult brightnesshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float b;
    if(!camera->setbrightness) return RESULT_FAIL;
    if(val){
        b = atof(val);
        int r = camera->setbrightness(b);
        if(!r) return RESULT_FAIL;
    }
    if(!camera->getbrightness) return RESULT_SILENCE;
    int r = camera->getbrightness(&b);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_BRIGHTNESS "=%.1f", b);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// set format: `format=X0,X1,Y0,Y1`
// get geomlimits: `maxformat=X0,X1,Y0,Y1`
static cc_hresult formathandler(int fd, const char *key, const char *val){
    char buf[64];
    cc_frameformat fmt;
    DBG("key=%s, val=%s", key, val);
    if(val){
        if(!camera->setgeometry) return RESULT_FAIL;
        if(0 == strcmp(key, CC_CMD_FRAMEMAX)){
            DBG("CANT SET MAXFORMAT");
            return RESULT_BADKEY; // can't set maxformat
        }
        if(4 != sscanf(val, "%d,%d,%d,%d", &fmt.xoff, &fmt.yoff, &fmt.w, &fmt.h)){
            DBG("Wrong format %s", val);
            return RESULT_BADVAL;
        }
        fmt.w -= fmt.xoff; fmt.h -= fmt.yoff;
        int r = camera->setgeometry(&fmt);
        if(!r) return RESULT_FAIL;
        curformat = fmt;
        DBG("curformat: w=%d, h=%d", curformat.w, curformat.h);
        fixima();
    }
    if(0 == strcmp(key, CC_CMD_FRAMEMAX)) snprintf(buf, 63, CC_CMD_FRAMEMAX "=%d,%d,%d,%d",
        frmformatmax.xoff, frmformatmax.yoff, frmformatmax.xoff+frmformatmax.w, frmformatmax.yoff+frmformatmax.h);
    else snprintf(buf, 63, CC_CMD_FRAMEFORMAT "=%d,%d,%d,%d",
        camera->geometry.xoff, camera->geometry.yoff, camera->geometry.xoff+camera->geometry.w, camera->geometry.yoff+camera->geometry.h);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult nflusheshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(!camera->setnflushes) return RESULT_FAIL;
    if(val){
        int n = atoi(val);
        if(n < 1) return RESULT_BADVAL;
        if(!camera->setnflushes(n)){
            return RESULT_FAIL;
        }
        nflushes = n;
    }
    snprintf(buf, 63, CC_CMD_NFLUSHES "=%d", nflushes);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult expstatehandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n == CAMERA_IDLE){ // cancel expositions
            camflags |= FLAG_CANCEL;
            return RESULT_OK;
        }
        else if(n == CAMERA_CAPTURE){ // start exposition
            TIMESTAMP("Get FLAG_STARTCAPTURE");
            TIMEINIT();
            camflags |= FLAG_STARTCAPTURE;
            return RESULT_OK;
        }
        else return RESULT_BADVAL;
    }
    snprintf(buf, 63, CC_CMD_EXPSTATE "=%d", camstate);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    snprintf(buf, 63, "camflags=%d", camflags);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult tremainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    snprintf(buf, 63, CC_CMD_TREMAIN "=%g", tremain);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult _8bithandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(!camera->setbitdepth) return RESULT_FAIL;
    if(val){
        int s = atoi(val);
        if(s != 0 && s != 1) return RESULT_BADVAL;
        if(!camera->setbitdepth(!s)) return RESULT_FAIL;
        fixima();
        GP->_8bit = s;
    }
    snprintf(buf, 63, CC_CMD_8BIT "=%d", GP->_8bit);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult fastspdhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(!camera->setfastspeed) return RESULT_FAIL;
    if(val){
        int b = atoi(val);
        if(b != 0 && b != 1) return RESULT_BADVAL;
        GP->fast = b;
        if(!camera->setfastspeed(b)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CC_CMD_FASTSPD "=%d", GP->fast);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult darkhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(!camera->setframetype) return RESULT_FAIL;
    if(val){
        int d = atoi(val);
        if(d != 0 && d != 1) return RESULT_BADVAL;
        GP->dark = d;
        d = !d;
        if(!camera->setframetype(d)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CC_CMD_DARK "=%d", GP->dark);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult FITSparhandler(int fd, const char *key, const char *val){
    char buf[256], **fitskey = NULL;
    if(0 == strcmp(key, CC_CMD_AUTHOR)){
        fitskey = &GP->author;
    }else if(0 == strcmp(key, CC_CMD_INSTRUMENT)){
        fitskey = &GP->instrument;
    }else if(0 == strcmp(key, CC_CMD_OBSERVER)){
        fitskey = &GP->observers;
    }else if(0 == strcmp(key, CC_CMD_OBJECT)){
        fitskey = &GP->objname;
    }else if(0 == strcmp(key, CC_CMD_PROGRAM)){
        fitskey = &GP->prog_id;
    }else if(0 == strcmp(key, CC_CMD_OBJTYPE)){
        fitskey = &GP->objtype;
    }else return RESULT_BADKEY;
    if(val){
        FREE(*fitskey);
        *fitskey = strdup(val);
    }
    snprintf(buf, 255, "%s=%s", key, *fitskey);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult FITSheaderhandler(int fd, _U_ const char *key, const char *val){
    char buf[BUFSIZ], **sptr;
    static char *curhdr = NULL;
    static int firstrun = 1;
    if(val){
        int sz = 10, amount = 0;
        // first we should check `val`
        char b2[BUFSIZ], *bptr = buf;
        snprintf(b2, BUFSIZ-1, "%s", val);
        char **list = MALLOC(char*, sz), **lptr = list;
        int L = BUFSIZ;
        for(char *s = b2; ; s = NULL){
            char *tok = strtok(s, ",;");
            if(!tok){
                *lptr = NULL;
                break;
            }
            // check path
            char *newpath = makeabspath(tok, TRUE);
            DBG("next token: %s, path: %s", tok, newpath);
            if(!newpath){ // error! Free list and return err
                DBG("No such file");
                sptr = list;
                while(*sptr){
                    FREE(*sptr++);
                }
                FREE(list);
                return RESULT_BADVAL;
            }
            *lptr++ = strdup(newpath);
            if(++amount == sz){
                DBG("Realloc");
                sz += 10;
                list = realloc(list, sz*sizeof(char*));
                bzero(&list[sz-10], 10*sizeof(char*));
            }
            int N = snprintf(bptr, L-1, "%s,", newpath);
            bptr += N; L -= N;
        }
        // free old list and change its value
        if(GP->addhdr){
            DBG("Free old list");
            sptr = GP->addhdr;
            while(*sptr){
                DBG("Free %s", *sptr);
                free(*(sptr++));
            }
        }
        GP->addhdr = list;
        FREE(curhdr);
        if(*val && *val != ',') curhdr = strdup(buf); // command with empty arg will clear curhdr
        DBG("curhdr now: %s", curhdr);
    }
    if(!curhdr && firstrun){
        firstrun = 0;
        if(GP->addhdr && *GP->addhdr){
            char *ptr = buf;
            int L = BUFSIZ;
            sptr = GP->addhdr;
            while(*sptr){
                DBG("Add to curhdr: %s", *sptr);
                int N = snprintf(ptr, L-1, "%s,", *sptr++);
                L -= N; ptr += N;
            }
            curhdr = strdup(buf);
        }
    }
    snprintf(buf, BUFSIZ-1, CC_CMD_HEADERFILES "=%s", curhdr);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
/*
static cc_hresult handler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    return RESULT_SILENCE;
}
*/
/*******************************************************************************
 ***************************** cc_Wheel handlers **********************************
 ******************************************************************************/
static cc_hresult wlisthandler(int fd, _U_ const char *key, _U_ const char *val){
    if(wheel->Ndevices < 1) return RESULT_FAIL;
    for(int i = 0; i < wheel->Ndevices; ++i){
        if(!wheel->setDevNo(i)) continue;
        char modname[256], buf[BUFSIZ];
        wheel->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CC_CMD_WLIST "='%s'", modname);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(wheeldevno > -1) wheel->setDevNo(wheeldevno);
    return RESULT_SILENCE;
}
static cc_hresult wsetNhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > wheel->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!wheeldevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CC_CMD_WDEVNO "=%d", wheeldevno);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

static cc_hresult wgotohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    int pos;
    if(val){
        pos = atoi(val);
        DBG("USER wants to %d", pos);
        int r = wheel->setPos(pos);
        DBG("wheel->setPos(%d)", pos);
        if(!r) return RESULT_BADVAL;
    }
    int r = wheel->getPos(&pos);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_WPOS "=%d", pos);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

/*******************************************************************************
 **************************** Focuser handlers *********************************
 ******************************************************************************/

static cc_hresult foclisthandler(int fd, _U_ const char *key, _U_ const char *val){
    if(focuser->Ndevices < 1) return RESULT_FAIL;
    for(int i = 0; i < focuser->Ndevices; ++i){
        char modname[256], buf[BUFSIZ];
        if(!focuser->setDevNo(i)) continue;
        focuser->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CC_CMD_FOCLIST "='%s'", modname);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(focdevno > -1) focuser->setDevNo(focdevno);
    return RESULT_SILENCE;
}
static cc_hresult fsetNhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int num = atoi(val);
        if(num > focuser->Ndevices - 1 || num < 0){
            return RESULT_BADVAL;
        }
        if(!focdevini(num)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CC_CMD_FDEVNO "=%d", focdevno);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static cc_hresult fgotohandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    float f;
    int r;
    if(val){
        f = atof(val);
        if(f < focminpos || f > focmaxpos) return RESULT_BADVAL;
        if(f - focminpos < __FLT_EPSILON__){
            r = focuser->home(1);
        }else{
            r = focuser->setAbsPos(1, f);
        }
        if(!r) return RESULT_FAIL;
    }
    r = focuser->getPos(&f);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_FGOTO "=%g", f);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

/*******************************************************************************
 **************************** Common handlers **********************************
 ******************************************************************************/

// information about everything
static cc_hresult infohandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[BUFSIZ], buf1[256];
    float f;
    int i;
    if(camera){
        if(camera->getModelName && camera->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CC_CMD_CAMLIST "='%s'", buf1);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
#define RUN(f, arg) do{if(RESULT_DISCONNECTED == f(fd, arg, NULL)) return RESULT_DISCONNECTED;}while(0)
        RUN(namehandler, CC_CMD_FILENAME);
        RUN(binhandler, CC_CMD_HBIN);
        RUN(binhandler, CC_CMD_VBIN);
        RUN(temphandler, CC_CMD_CAMTEMPER);
        RUN(exphandler, CC_CMD_EXPOSITION);
        RUN(lastfnamehandler, CC_CMD_LASTFNAME);
        RUN(expstatehandler, CC_CMD_EXPSTATE);
#undef RUN
    }
    if(wheel){
        DBG("chk wheel");
        if(wheel->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CC_CMD_WLIST "='%s'", buf1);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(wheel->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "wtemp=%.1f", f);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(wheel->getPos(&i)){
            snprintf(buf, BUFSIZ-1, CC_CMD_WPOS "=%d", i);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        snprintf(buf, BUFSIZ-1, "wmaxpos=%d", wmaxpos);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(focuser){
        DBG("Chk focuser");
        if(focuser->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CC_CMD_FOCLIST "='%s'", buf1);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(focuser->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "foctemp=%.1f", f);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        snprintf(buf, BUFSIZ-1, "focminpos=%g", focminpos);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        snprintf(buf, BUFSIZ-1, "focmaxpos=%g", focmaxpos);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        if(focuser->getPos(&f)){
            snprintf(buf, BUFSIZ-1, CC_CMD_FGOTO "=%g", f);
            if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
    }
    DBG("EOF");
    return RESULT_SILENCE;
}
// show help
static cc_hresult helphandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[256];
    strpair *ptr = allcommands;
    while(ptr->key){
        snprintf(buf, 255, "%s - %s", ptr->key, ptr->help);
        if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        ++ptr;
    }
    return RESULT_SILENCE;
}

// shared memory key
static cc_hresult shmemkeyhandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(shmkey == IPC_PRIVATE) return RESULT_FAIL;
    snprintf(buf, 63, CC_CMD_SHMEMKEY "=%d", shmkey);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

// infinity loop
static cc_hresult inftyhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int i = atoi(val);
        infty = (i) ? 1 : 0;
    }
    snprintf(buf, 63, CC_CMD_INFTY "=%d", infty);
    if(!cc_sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

// custom camera plugin command
static cc_hresult pluginhandler(int fd, _U_ const char *key, const char *val){
    if(!camera->plugincmd) return RESULT_BADKEY;
    static cc_charbuff *ans = NULL;
    if(!ans) ans = cc_charbufnew();
    cc_buff_lock(ans);
    cc_charbufclr(ans);
    cc_hresult r = camera->plugincmd(val, ans);
    cc_buff_unlock(ans);
    if(ans->buflen && !cc_sendstrmessage(fd, ans->buf)) r = RESULT_DISCONNECTED;
    return r;
}

// get headers
static cc_hresult gethdrshandler(int fd, _U_ const char *key, _U_ const char *val){
    cc_charbuff *b = getFITSheader(ima);
    if(!b) return RESULT_FAIL;
    if(!cc_sendstrmessage(fd, b->buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

// for setters: do nothing when camera not in idle state
static int CAMbusy(){
    if(camera && camstate != CAMERA_IDLE){
        DBG("Camera busy");
        return TRUE;
    }
    return FALSE;
}
// check funtions
static cc_hresult chktrue(_U_ char *val){ // dummy check for `infohandler` (need to lock mutex anymore)
    return RESULT_OK;
}
static cc_hresult chkcam(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(camera) return RESULT_OK;
    return RESULT_FAIL;
}
static cc_hresult chkcc(_U_ char *val){ // just check that camera connected
    if(camera) return RESULT_OK;
    return RESULT_FAIL;
}
static cc_hresult chkwhl(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(wheel) return RESULT_OK;
    return RESULT_FAIL;
}
static cc_hresult chkfoc(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(focuser) return RESULT_OK;
    return RESULT_FAIL;
}
static cc_handleritem items[] = {
    {chktrue,infohandler, CC_CMD_INFO},
    {NULL,   helphandler, CC_CMD_HELP},
    {NULL,   restarthandler, CC_CMD_RESTART},
    {chkcc,  camlisthandler, CC_CMD_CAMLIST},
    {chkcc,  camsetNhandler, CC_CMD_CAMDEVNO},
    {chkcc,  camfanhandler, CC_CMD_CAMFANSPD},
    {chkcc,  exphandler, CC_CMD_EXPOSITION},
    {chkcc,  namehandler, CC_CMD_FILENAME},
    {chkcc,  binhandler, CC_CMD_HBIN},
    {chkcc,  binhandler, CC_CMD_VBIN},
    {chkcc,  temphandler, CC_CMD_CAMTEMPER},
    {chkcam, shutterhandler, CC_CMD_SHUTTER},
    {chkcc,  confiohandler, CC_CMD_CONFIO},
    {chkcc,  iohandler, CC_CMD_IO},
    {chkcc,  gainhandler, CC_CMD_GAIN},
    {chkcc,  brightnesshandler, CC_CMD_BRIGHTNESS},
    {chkcc,  formathandler, CC_CMD_FRAMEFORMAT},
    {chkcc,  formathandler, CC_CMD_FRAMEMAX},
    {chkcc,  nflusheshandler, CC_CMD_NFLUSHES},
    {NULL,   expstatehandler, CC_CMD_EXPSTATE},
    {chktrue,shmemkeyhandler, CC_CMD_SHMEMKEY},
    {chktrue,imsizehandler, CC_CMD_IMWIDTH},
    {chktrue,imsizehandler, CC_CMD_IMHEIGHT},
    {chkcc,  nameprefixhandler, CC_CMD_FILENAMEPREFIX},
    {chkcc,  rewritefilehandler, CC_CMD_REWRITE},
    {chkcc,  _8bithandler, CC_CMD_8BIT},
    {chkcc,  fastspdhandler, CC_CMD_FASTSPD},
    {chkcc,  darkhandler, CC_CMD_DARK},
    {chkcc,  inftyhandler, CC_CMD_INFTY},
    {chkcc,  pluginhandler, CC_CMD_PLUGINCMD},
    {NULL,   tremainhandler, CC_CMD_TREMAIN},
    {chkcc,  gethdrshandler, CC_CMD_GETHEADERS},
    {NULL,   FITSparhandler, CC_CMD_AUTHOR},
    {NULL,   FITSparhandler, CC_CMD_INSTRUMENT},
    {NULL,   FITSparhandler, CC_CMD_OBSERVER},
    {NULL,   FITSparhandler, CC_CMD_OBJECT},
    {NULL,   FITSparhandler, CC_CMD_PROGRAM},
    {NULL,   FITSparhandler, CC_CMD_OBJTYPE},
    {NULL,   FITSheaderhandler, CC_CMD_HEADERFILES},
    {NULL,   lastfnamehandler, CC_CMD_LASTFNAME},
    {chkfoc, foclisthandler, CC_CMD_FOCLIST},
    {chkfoc, fsetNhandler, CC_CMD_FDEVNO},
    {chkfoc, fgotohandler, CC_CMD_FGOTO},
    {chkwhl, wlisthandler, CC_CMD_WLIST},
    {chkwhl, wsetNhandler, CC_CMD_WDEVNO},
    {chkwhl, wgotohandler, CC_CMD_WPOS},
    {NULL, NULL, NULL},
};

#define CLBUFSZ     BUFSIZ
#define STRBUFSZ    (255)

// send image as raw data
static void sendimage(int client){
    if(ima->h < 1 || ima->w < 1) return;
    cc_senddata(client, ima, sizeof(cc_IMG));
    cc_senddata(client, ima->data, ima->bytelen);
}

void server(int sock, int imsock){
    DBG("sockfd=%d, imsockfd=%d", sock, imsock);
    if(sock < 0) ERRX("server(): need at least command socket fd");
    if(imsock < 0) WARNX("Server run without image transport socket");
    else if(listen(imsock, CC_MAXCLIENTS) == -1){
        WARN("listen()");
        LOGWARN("listen()");
        return;
    }
    if(listen(sock, CC_MAXCLIENTS) == -1){
        WARN("listen()");
        LOGWARN("listen()");
        return;
    }
    // init everything
    int ctr = 3;
    if(startFocuser()) --ctr;
    focdevini(0);
    if(startWheel()) --ctr;
    wheeldevini(0);
    if(startCCD()) --ctr;
    camdevini(0);
    if(ctr == 3) ERRX("No devices found");
    // start camera thread
    pthread_t camthread;
    if(camera){
        if(pthread_create(&camthread, NULL, processCAM, NULL)) ERR("pthread_create()");
    }
    int nfd = 2; // only two listening sockets @start: command and image
    struct pollfd poll_set[CC_MAXCLIENTS+2];
    cc_strbuff *buffers[CC_MAXCLIENTS];
    for(int i = 0; i < CC_MAXCLIENTS; ++i){
        buffers[i] = cc_strbufnew(CLBUFSZ, STRBUFSZ);
    }
    bzero(poll_set, sizeof(poll_set));
    // ZERO - listening server socket
    poll_set[0].fd = sock;
    poll_set[0].events = POLLIN;
    poll_set[1].fd = imsock;
    poll_set[1].events = POLLIN;
    while(1){
        poll(poll_set, nfd, 1); // max timeout - 1ms
        //if(imsock > -1 && cc_canberead(imsock) > 0){
        if(imsock > -1 && (poll_set[1].revents & POLLIN)){
            //uint8_t buf[32];
            //int l = read(imsock, buf, 32);
            DBG("Somebody wants an image");
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);
            int client = accept(imsock, (struct sockaddr*)&addr, &len);
            DBG("client=%d", client);
            if(client > -1){
                DBG("client fd: %d", client);
                sendimage(client);
                close(client);
                DBG("%d closed", client);
            }else{WARN("accept()"); DBG("disconnected");}
            TIMESTAMP("Image sent");
        }
        if(poll_set[0].revents & POLLIN){ // check main for accept()
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);
            int client = accept(sock, (struct sockaddr*)&addr, &len);
            if(client > -1){
                DBG("New connection");
                LOGMSG("SERVER got connection, fd=%d", client);
                if(nfd == CC_MAXCLIENTS + 1){
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
        }
        // process some data & send messages to ALL
        if(camstate == CAMERA_FRAMERDY || camstate == CAMERA_ERROR){
            DBG("new image: timestamp=%.1f, num=%zd", ima->timestamp, ima->imnumber);
            char buff[PATH_MAX+32];
            snprintf(buff, PATH_MAX, CC_CMD_EXPSTATE "=%d", camstate);
            DBG("Send %s to %d clients", buff, nfd - 2);
            for(int i = 2; i < nfd; ++i){
                TIMESTAMP("Send message that all ready");
                cc_sendstrmessage(poll_set[i].fd, buff);
            }
            if(camstate == CAMERA_FRAMERDY && (GP->outfile || GP->outfileprefix)){ // send to all last file name if file saved
                snprintf(buff, PATH_MAX+31, CC_CMD_LASTFNAME "=%s", lastfile);
                for(int i = 2; i < nfd; ++i)
                    cc_sendstrmessage(poll_set[i].fd, buff);
            }
            camstate = CAMERA_IDLE;
        }
        // scan connections
        for(int fdidx = 2; fdidx < nfd; ++fdidx){
            if((poll_set[fdidx].revents & POLLIN) == 0) continue;
            int fd = poll_set[fdidx].fd;
            cc_strbuff *curbuff = buffers[fdidx-1];
            int disconnected = 0;
            if(cc_read2buf(fd, curbuff)){
                size_t got = cc_getline(curbuff);
                if(got >= CLBUFSZ){
                    DBG("Client fd=%d gave buffer overflow", fd);
                    LOGMSG("SERVER client fd=%d buffer overflow", fd);
                }else if(got){
                    if(!parsestring(fd, items, curbuff->string)) disconnected = 1;
                }
            }else disconnected = 1;
            if(disconnected){
                DBG("Client fd=%d disconnected", fd);
                LOGMSG("SERVER client fd=%d disconnected", fd);
                curbuff->buflen = 0; // clear rest of data in buffer
                close(fd);
                // move last FD to current position
                poll_set[fdidx] = poll_set[nfd - 1];
                --nfd;
            }
        }
        // check `infty`
        if(camstate == CAMERA_IDLE && infty){ // start new exposition
            // mark to start new capture in infinity loop when at least one client connected
            if(nfd > 2){
                camflags |= FLAG_STARTCAPTURE;
                TIMESTAMP("start new capture due to `infty`");
                TIMEINIT();
            }
        }
    }
    // never reached
    focclose();
    closewheel();
    closecam();
}

/**
 * @brief makeabspath - convert path to absolute and check it
 * @param path - path to file
 * @param shoulbe - ==1 if file must exists
 * @return abs path or NULL (if can't convert, can't create or file not exists)
 */
char *makeabspath(const char *path, int shouldbe){
    static char buf[PATH_MAX+1];
    if(!path) return NULL;
    char *ret = NULL;
    int unl = 0;
    FILE *f = fopen(path, "r");
    if(!f){
        if(shouldbe) return NULL;
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

// parse string of data (command or key=val)
// the CONTENT of buffer `str` WILL BE BROKEN!
// @return FALSE if client closed (nothing to read)
static int parsestring(int fd, cc_handleritem *handlers, char *str){
    if(fd < 1 || !handlers || !handlers->key || !str || !*str) return FALSE;
    char *val = cc_get_keyval(str);
    if(val){
        DBG("RECEIVE '%s=%s'", str, val);
        LOGDBG("RECEIVE '%s=%s'", str, val);
    }else{
        DBG("RECEIVE '%s'", str);
        LOGDBG("RECEIVE '%s'", str);
    }
    for(cc_handleritem *h = handlers; h->key; ++h){
        if(strcmp(str, h->key)) continue;
        cc_hresult r = RESULT_OK;
        int l = FALSE;
        if(h->chkfunction){
            double t0 = dtime();
            do{ l = lock(); } while(!l && dtime() - t0 < CC_BUSY_TIMEOUT);
            DBG("time: %g", dtime() - t0);
            if(!l){
                WARN("Can't lock mutex"); //signals(1);
                return RESULT_BUSY; // long blocking work
            }
            r = h->chkfunction(val);
        } // else NULL instead of chkfuntion -> don't check and don't lock mutex
        if(r == RESULT_OK){ // no test function or it returns TRUE
            if(h->handler) r = h->handler(fd, str, val);
            else r = RESULT_FAIL;
        }
        if(l) unlock();
        if(r == RESULT_DISCONNECTED){
            DBG("handler return RESULT_DISCONNECTED");
            return FALSE;
        }
        DBG("handler returns with '%s' (%d)", cc_hresult2str(r), r);
        return cc_sendstrmessage(fd, cc_hresult2str(r));
    }
    DBG("Command not found!");
    return cc_sendstrmessage(fd, cc_hresult2str(RESULT_BADKEY));
}
