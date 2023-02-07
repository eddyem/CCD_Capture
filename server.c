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
#define FLAG_RESTARTSERVER      (1<<2)
static atomic_int camflags = 0, camfanspd = 0, confio = 0, nflushes;
static char *outfile = NULL, *lastfile = NULL; // current output file name/prefix; last name of saved file
//static pthread_mutex_t locmutex = PTHREAD_MUTEX_INITIALIZER; // mutex for wheel/camera/focuser functions
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
    { CMD_8BIT,         "run in 8 bit mode instead of 16 bit" },
    { CMD_AUTHOR,       "FITS 'AUTHOR' field" },
    { CMD_BRIGHTNESS,   "camera brightness" },
    { CMD_CAMDEVNO,     "camera device number" },
    { CMD_CAMLIST,      "list all connected cameras" },
    { CMD_CAMFANSPD,    "fan speed of camera" },
    { CMD_CONFIO,       "camera IO configuration" },
    { CMD_DARK,         "don't open shutter @ exposure" },
    { CMD_EXPSTATE,     "get exposition state" },
    { CMD_EXPOSITION,   "exposition time" },
    { CMD_FASTSPD,      "fast readout speed" },
    { CMD_FILENAME,     "save file with this name, like file.fits" },
    { CMD_FILENAMEPREFIX,"prefix of files, like ex (will be saved as exXXXX.fits)" },
    { CMD_FDEVNO,       "focuser device number" },
    { CMD_FOCLIST,      "list all connected focusers" },
    { CMD_FGOTO,        "focuser position" },
    { CMD_FRAMEFORMAT,  "camera frame format (X0,Y0,X1,Y1)" },
    { CMD_GAIN,         "camera gain" },
    { CMD_HBIN,         "horizontal binning" },
    { CMD_HEADERFILES,  "add FITS records from these files (comma-separated list)" },
    { CMD_HELP,         "show this help" },
    { CMD_INFO,         "connected devices state" },
    { CMD_INSTRUMENT,   "FITS 'INSTRUME' field" },
    { CMD_IO,           "get/set camera IO" },
    { CMD_LASTFNAME,    "path to last saved file"},
    { CMD_FRAMEMAX,     "camera maximal available format" },
    { CMD_NFLUSHES,     "camera number of preflushes" },
    { CMD_OBJECT,       "FITS 'OBJECT' field" },
    { CMD_OBJTYPE,      "FITS 'IMAGETYP' field" },
    { CMD_OBSERVER,     "FITS 'OBSERVER' field" },
    { CMD_PROGRAM,      "FITS 'PROG-ID' field" },
    { CMD_RESTART,      "restart server" },
    { CMD_REWRITE,      "rewrite file (if give `filename`, not `filenameprefix`" },
    { CMD_SHUTTER,      "camera shutter's operations" },
    { CMD_CAMTEMPER,    "camera chip temperature" },
    { CMD_TREMAIN,      "time (in seconds) of exposition remained" },
    { CMD_VBIN,         "vertical binning" },
    { CMD_WDEVNO,       "wheel device number" },
    { CMD_WLIST,        "list all connected wheels" },
    { CMD_WPOS,         "wheel position" },
    {NULL, NULL},
};

static IMG ima = {0};
static void fixima(){
    FNAME();
    if(!camera) return;
    int raw_width = curformat.w / GP->hbin,  raw_height = curformat.h / GP->vbin;
    if(!ima.data) ima.data = MALLOC(uint16_t,  camera->array.h * camera->array.w);
    if(ima.data && raw_width == ima.w && raw_height == ima.h) return; // all OK
    DBG("curformat: %dx%d", curformat.w, curformat.h);
    ima.h = raw_height;
    ima.w = raw_width;
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
        LOGMSG("User canceled exposition");
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
                    if(saveFITS(&ima, &lastfile)){
                        DBG("LAST file name changed");
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
        if(0 == pthread_mutex_trylock(&locmutex)){
            // log
            if(dtime() - logt > TLOG_PAUSE){
                logt = dtime();
                float t;
                if(camera->getTcold(&t)){
                    LOGMSG("CCDTEMP=%.1f", t);
                }
                if(camera->getThot(&t)){
                   LOGMSG("HOTTEMP=%.1f", t);
                }
                if(camera->getTbody(&t)){
                   LOGMSG("BODYTEMP=%.1f", t);
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
            pthread_mutex_unlock(&locmutex);
        }
    }
    return NULL;
}

// functions running @ each devno change
static int camdevini(int n){
    if(!camera) return FALSE;
    if(!camera->setDevNo(n)){
        LOGERR("Can't set active camera number");
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
static hresult restarthandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    camflags |= FLAG_RESTARTSERVER;
    return RESULT_OK;
}

/*******************************************************************************
 *************************** CCD/CMOS handlers *********************************
 ******************************************************************************/
static hresult camlisthandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[BUFSIZ], modname[256];
    for(int i = 0; i < camera->Ndevices; ++i){
        if(!camera->setDevNo(i)) continue;
        camera->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_CAMLIST "='%s'", modname);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(camdevno > -1) camera->setDevNo(camdevno);
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
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// exposition time setter/getter
static hresult exphandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        DBG("setexp to %s", val);
        double v = atof(val);
        if(v < DBL_EPSILON) return RESULT_BADVAL;
        if(camera->setexp(v)){
            GP->exptime = v;
        }else LOGWARN("Can't set exptime to %g", v);
    }
    DBG("expt: %g", GP->exptime);
    snprintf(buf, 63, CMD_EXPOSITION "=%g", GP->exptime);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// show last filename of saved FITS
static hresult lastfnamehandler(int fd, _U_ const char *key, _U_ const char *val){
    char buf[PATH_MAX+32];
    snprintf(buf, PATH_MAX+31, CMD_LASTFNAME "=%s", lastfile);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// filename setter/getter
static hresult namehandler(int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+32];
    if(val){
        char *path = makeabspath(val, FALSE);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfile = outfile;
        GP->outfileprefix = NULL;
    }
    if(!GP->outfile) return RESULT_FAIL;
    snprintf(buf, PATH_MAX+31, CMD_FILENAME "=%s", GP->outfile);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// filename prefix
static hresult nameprefixhandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[PATH_MAX+32];
    if(val){
        char *path = makeabspath(val, FALSE);
        if(!path){
            LOGERR("Can't create file '%s'", val);
            //pthread_mutex_unlock(&locmutex);
            return RESULT_BADVAL;
        }
        FREE(outfile);
        outfile = strdup(path);
        GP->outfileprefix = outfile;
        GP->outfile = NULL;
    }
    if(!GP->outfileprefix) return RESULT_FAIL;
    snprintf(buf, PATH_MAX+31, CMD_FILENAMEPREFIX "=%s", GP->outfileprefix);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
// rewrite
static hresult rewritefilehandler(_U_ int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n < 0 || n > 1) return RESULT_BADVAL;
        GP->rewrite = n;
    }
    snprintf(buf, 63, CMD_REWRITE "=%d", GP->rewrite);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult binhandler(_U_ int fd, const char *key, const char *val){
    char buf[64];
    if(val){
        int b = atoi(val);
        if(b < 1) return RESULT_BADVAL;
        if(0 == strcmp(key, CMD_HBIN)) GP->hbin = b;
        else GP->vbin = b;
        if(!camera->setbin(GP->hbin, GP->vbin)){
            return RESULT_BADVAL;
        }
    }
    int r = camera->getbin(&GP->hbin, &GP->vbin);
    if(r){
        if(0 == strcmp(key, CMD_HBIN)) snprintf(buf, 63, "%s=%d", key, GP->hbin);
        else snprintf(buf, 63, "%s=%d", key, GP->vbin);
        if(val) fixima();
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
        r = camera->setT((float)f);
        if(!r){
            LOGWARN("Can't set camera T to %.1f", f);
            return RESULT_FAIL;
        }
        LOGMSG("Set camera T to %.1f", f);
    }
    r = camera->getTcold(&f);
    if(r){
        snprintf(buf, 63, CMD_CAMTEMPER "=%.1f", f);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        r = camera->getTbody(&f);
        if(r){
            snprintf(buf, 63, "tbody=%.1f", f);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        r = camera->getThot(&f);
        if(r){
            snprintf(buf, 63, "thot=%.1f", f);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
        int r = camera->setfanspeed((fan_speed)spd);
        if(!r) return RESULT_FAIL;
        camfanspd = spd;
    }
    snprintf(buf, 63, CMD_CAMFANSPD "=%d", camfanspd);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
const char *shutterstr[] = {"open", "close", "expose @high", "expose @low"};
static hresult shutterhandler(_U_ int fd, _U_ const char *key, const char *val){
    if(val){
        int x = atoi(val);
        if(x < 0 || x >= SHUTTER_AMOUNT) return RESULT_BADVAL;
        int r = camera->shuttercmd((shutter_op)x);
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
        int r = camera->confio(io);
        if(!r) return RESULT_FAIL;
        confio = io;
    }
    snprintf(buf, 63, CMD_CONFIO "=%d", confio);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult iohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    int io;
    if(val){
        io = atoi(val);
        int r = camera->setio(io);
        if(!r) return RESULT_FAIL;
    }
    int r = camera->getio(&io);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_IO "=%d", io);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult gainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float f;
    if(val){
        f = atof(val);
        int r = camera->setgain(f);
        if(!r) return RESULT_FAIL;
    }
    int r = camera->getgain(&f);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_GAIN "=%.1f", f);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult brightnesshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    float b;
    if(val){
        b = atof(val);
        int r = camera->setbrightness(b);
        if(!r) return RESULT_FAIL;
    }
    int r = camera->getbrightness(&b);
    if(!r) return RESULT_FAIL;
    snprintf(buf, 63, CMD_BRIGHTNESS "=%.1f", b);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
        int r = camera->setgeometry(&fmt);
        if(!r) return RESULT_FAIL;
        curformat = fmt;
        fixima();
    }
    if(0 == strcmp(key, CMD_FRAMEMAX)) snprintf(buf, 63, CMD_FRAMEMAX "=%d,%d,%d,%d",
        frmformatmax.xoff, frmformatmax.yoff, frmformatmax.xoff+frmformatmax.w, frmformatmax.yoff+frmformatmax.h);
    else snprintf(buf, 63, CMD_FRAMEFORMAT "=%d,%d,%d,%d",
        camera->geometry.xoff, camera->geometry.yoff, camera->geometry.xoff+camera->geometry.w, camera->geometry.yoff+camera->geometry.h);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult nflusheshandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    if(val){
        int n = atoi(val);
        if(n < 1) return RESULT_BADVAL;
        if(!camera->setnflushes(n)){
            return RESULT_FAIL;
        }
        nflushes = n;
    }
    snprintf(buf, 63, CMD_NFLUSHES "=%d", nflushes);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    snprintf(buf, 63, "camflags=%d", camflags);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult tremainhandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
    char buf[64];
    snprintf(buf, 63, CMD_TREMAIN "=%g", tremain);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult _8bithandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int s = atoi(val);
        if(s != 0 && s != 1) return RESULT_BADVAL;
        GP->_8bit = s;
        s = !s;
        if(!camera->setbitdepth(s)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_8BIT "=%d", GP->_8bit);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult fastspdhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int b = atoi(val);
        if(b != 0 && b != 1) return RESULT_BADVAL;
        GP->fast = b;
        if(!camera->setfastspeed(b)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_FASTSPD "=%d", GP->fast);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult darkhandler(int fd, _U_ const char *key, const char *val){
    char buf[64];
    if(val){
        int d = atoi(val);
        if(d != 0 && d != 1) return RESULT_BADVAL;
        GP->dark = d;
        d = !d;
        if(!camera->setframetype(d)) return RESULT_FAIL;
    }
    snprintf(buf, 63, CMD_DARK "=%d", GP->dark);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult FITSparhandler(int fd, const char *key, const char *val){
    char buf[256], **fitskey = NULL;
    if(0 == strcmp(key, CMD_AUTHOR)){
        fitskey = &GP->author;
    }else if(0 == strcmp(key, CMD_INSTRUMENT)){
        fitskey = &GP->instrument;
    }else if(0 == strcmp(key, CMD_OBSERVER)){
        fitskey = &GP->observers;
    }else if(0 == strcmp(key, CMD_OBJECT)){
        fitskey = &GP->objname;
    }else if(0 == strcmp(key, CMD_PROGRAM)){
        fitskey = &GP->prog_id;
    }else if(0 == strcmp(key, CMD_OBJTYPE)){
        fitskey = &GP->objtype;
    }else return RESULT_BADKEY;
    if(val){
        FREE(*fitskey);
        *fitskey = strdup(val);
    }
    snprintf(buf, 255, "%s=%s", key, *fitskey);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult FITSheaderhandler(int fd, _U_ const char *key, const char *val){
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
                //pthread_mutex_unlock(&locmutex);
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
    snprintf(buf, BUFSIZ-1, CMD_HEADERFILES "=%s", curhdr);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
    for(int i = 0; i < wheel->Ndevices; ++i){
        if(!wheel->setDevNo(i)) continue;
        char modname[256], buf[BUFSIZ];
        wheel->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_WLIST "='%s'", modname);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(wheeldevno > -1) wheel->setDevNo(wheeldevno);
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
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

static hresult wgotohandler(_U_ int fd, _U_ const char *key, _U_ const char *val){
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
    snprintf(buf, 63, CMD_WPOS "=%d", pos);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}

/*******************************************************************************
 **************************** Focuser handlers *********************************
 ******************************************************************************/

static hresult foclisthandler(int fd, _U_ const char *key, _U_ const char *val){
    if(focuser->Ndevices < 1) return RESULT_FAIL;
    for(int i = 0; i < focuser->Ndevices; ++i){
        char modname[256], buf[BUFSIZ];
        if(!focuser->setDevNo(i)) continue;
        focuser->getModelName(modname, 255);
        snprintf(buf, BUFSIZ-1, CMD_FOCLIST "='%s'", modname);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(focdevno > -1) focuser->setDevNo(focdevno);
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
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    return RESULT_SILENCE;
}
static hresult fgotohandler(int fd, _U_ const char *key, const char *val){
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
    snprintf(buf, 63, CMD_FGOTO "=%g", f);
    if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
#define RUN(f, arg) do{if(RESULT_DISCONNECTED == f(fd, arg, NULL)) return RESULT_DISCONNECTED;}while(0)
        RUN(namehandler, CMD_FILENAME);
        RUN(binhandler, CMD_HBIN);
        RUN(binhandler, CMD_VBIN);
        RUN(temphandler, CMD_CAMTEMPER);
        RUN(exphandler, CMD_EXPOSITION);
        RUN(lastfnamehandler, CMD_LASTFNAME);
        RUN(expstatehandler, CMD_EXPSTATE);
#undef RUN
    }
    DBG("chk wheel");
    if(wheel){
        DBG("Wname");
        if(wheel->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CMD_WLIST "='%s'", buf1);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(wheel->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "wtemp=%.1f", f);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(wheel->getPos(&i)){
            snprintf(buf, BUFSIZ-1, CMD_WPOS "=%d", i);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        snprintf(buf, BUFSIZ-1, "wmaxpos=%d", wmaxpos);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
    }
    if(focuser){
        if(focuser->getModelName(buf1, 255)){
            snprintf(buf, BUFSIZ-1, CMD_FOCLIST "='%s'", buf1);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        if(focuser->getTbody(&f)){
            snprintf(buf, BUFSIZ-1, "foctemp=%.1f", f);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        }
        snprintf(buf, BUFSIZ-1, "focminpos=%g", focminpos);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        snprintf(buf, BUFSIZ-1, "focmaxpos=%g", focmaxpos);
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        if(focuser->getPos(&f)){
            snprintf(buf, BUFSIZ-1, CMD_FGOTO "=%g", f);
            if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
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
        if(!sendstrmessage(fd, buf)) return RESULT_DISCONNECTED;
        ++ptr;
    }
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
static hresult chktrue(_U_ char *val){ // dummy check for `infohandler` (need to lock mutex anymore)
    return RESULT_OK;
}
static hresult chkcam(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(camera) return RESULT_OK;
    return RESULT_FAIL;
}
static hresult chkwhl(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(wheel) return RESULT_OK;
    return RESULT_FAIL;
}
static hresult chkfoc(char *val){
    if(val && CAMbusy()) return RESULT_BUSY;
    if(focuser) return RESULT_OK;
    return RESULT_FAIL;
}
static handleritem items[] = {
    {chktrue,infohandler, CMD_INFO},
    {NULL,   helphandler, CMD_HELP},
    {NULL,   restarthandler, CMD_RESTART},
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
    {NULL,   expstatehandler, CMD_EXPSTATE},
    {chkcam, nameprefixhandler, CMD_FILENAMEPREFIX},
    {chkcam, rewritefilehandler, CMD_REWRITE},
    {chkcam, _8bithandler, CMD_8BIT},
    {chkcam, fastspdhandler, CMD_FASTSPD},
    {chkcam, darkhandler, CMD_DARK},
    {NULL,   tremainhandler, CMD_TREMAIN},
    {NULL,   FITSparhandler, CMD_AUTHOR},
    {NULL,   FITSparhandler, CMD_INSTRUMENT},
    {NULL,   FITSparhandler, CMD_OBSERVER},
    {NULL,   FITSparhandler, CMD_OBJECT},
    {NULL,   FITSparhandler, CMD_PROGRAM},
    {NULL,   FITSparhandler, CMD_OBJTYPE},
    {NULL,   FITSheaderhandler, CMD_HEADERFILES},
    {NULL,   lastfnamehandler, CMD_LASTFNAME},
    {chkfoc, foclisthandler, CMD_FOCLIST},
    {chkfoc, fsetNhandler, CMD_FDEVNO},
    {chkfoc, fgotohandler, CMD_FGOTO},
    {chkwhl, wlisthandler, CMD_WLIST},
    {chkwhl, wsetNhandler, CMD_WDEVNO},
    {chkwhl, wgotohandler, CMD_WPOS},
    {NULL, NULL, NULL},
};

#define CLBUFSZ     BUFSIZ

void server(int sock){
    // init everything
    startFocuser(&focdev);
    focdevini(0);
    startWheel(&wheeldev);
    wheeldevini(0);
    startCCD(&camdev);
    camdevini(0);
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
            char buff[PATH_MAX+32];
            snprintf(buff, PATH_MAX, CMD_EXPSTATE "=%d", camstate);
            DBG("Send %s to %d clients", buff, nfd - 1);
            for(int i = 1; i < nfd; ++i)
                sendstrmessage(poll_set[i].fd, buff);
            if(camstate == CAMERA_FRAMERDY){ // send to all last file name
                snprintf(buff, PATH_MAX+31, CMD_LASTFNAME "=%s", lastfile);
                for(int i = 1; i < nfd; ++i)
                    sendstrmessage(poll_set[i].fd, buff);
            }
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
