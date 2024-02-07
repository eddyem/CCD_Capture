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

#pragma once

#include <fitsio.h> // FLEN_CARD
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h> // for size_t

// magic to mark our SHM
#define CC_SHM_MAGIC   (0xdeadbeef)

// base image parameters - sent by socket and stored in shared memory
typedef struct __attribute__((packed, aligned(4))){
    uint32_t MAGICK;            // magick (DEADBEEF) - to mark our shm
    double timestamp;           // timestamp of image taken
    uint8_t bitpix;             // bits per pixel (8 or 16)
    int w, h;                   // image size
    int gotstat;                // stat counted
    uint16_t max, min;          // min/max values
    float avr, std;             // statistics
    size_t bytelen;             // size of image in bytes
    size_t imnumber;            // counter of images captured from server's start
    void *data;                 // pointer to data (next byte after this struct) - only for server
    /* `data` is uint8_t or uint16_t depending on `bitpix` */
} cc_IMG;

typedef struct{
    char* buf;      // databuffer
    size_t bufsize; // size of `buf`
    size_t buflen;  // current buffer length
    char *string;   // last \n-ended string from `buf`
    size_t strlen;  // max length of `string`
    pthread_mutex_t mutex; // mutex for atomic data access
} cc_strbuff;

typedef struct{
    char* buf;      // databuffer
    size_t bufsize; // size of `buf`
    size_t buflen;  // current buffer length
    pthread_mutex_t mutex; // mutex for atomic data access
} cc_charbuff;

#define cc_buff_lock(b) pthread_mutex_lock(&((b)->mutex))
#define cc_buff_trylock(b) pthread_mutex_trylock(&((b)->mutex))
#define cc_buff_unlock(b) pthread_mutex_unlock(&((b)->mutex))

// format of single frame
typedef struct{
    int w; int h;               // width & height
    int xoff; int yoff;         // X and Y offset
} cc_frameformat;

typedef enum{
    SHUTTER_OPEN,       // open shutter now
    SHUTTER_CLOSE,      // close shutter now
    SHUTTER_OPENATLOW,  // ext. expose control @low
    SHUTTER_OPENATHIGH, // -//- @high
    SHUTTER_AMOUNT,     // amount of entries
} cc_shutter_op;

typedef enum{
    CAPTURE_NO,         // no capture initiated
    CAPTURE_PROCESS,    // in progress
    CAPTURE_CANTSTART,  // can't start
    CAPTURE_ABORTED,    // some error - aborted
    CAPTURE_READY,      // ready - user can read image
} cc_capture_status;

typedef enum{
    FAN_OFF,
    FAN_LOW,
    FAN_MID,
    FAN_HIGH,
} cc_fan_speed;

typedef enum{
    RESULT_OK,          // 0: all OK
    RESULT_BUSY,        // 1: camera busy and no setters can be done
    RESULT_FAIL,        // 2: failed running command
    RESULT_BADVAL,      // 3: bad key's value
    RESULT_BADKEY,      // 4: bad key
    RESULT_SILENCE,     // 5: send nothing to client
    RESULT_DISCONNECTED,// 6: client disconnected
    RESULT_NUM
} cc_hresult;

// all setters and getters of Camera, Focuser and cc_Wheel should return TRUE if success or FALSE if failed or unsupported
// camera
typedef struct{
    int (*check)();             // check if the device is available, connect and init
    int Ndevices;               // amount of devices found
    void (*close)();            // disconnect & close device
    int (*startexposition)();   // start exposition
    int (*pollcapture)(cc_capture_status *st, float *remain);// get `st` - status of capture process, `remain` - time remain (s); @return FALSE if error (exp aborted), TRUE while no errors
    int (*capture)(cc_IMG *ima);   // capture an image, struct `ima` should be prepared before
    void (*cancel)();           // cancel exposition
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setbrightness)(float b);
    int (*setexp)(float e);
    int (*setgain)(float g);
    int (*setT)(float t);
    int (*setbin)(int binh, int binv); // binning
    int (*setnflushes)(int N);  // flushes amount
    int (*shuttercmd)(cc_shutter_op s); // work with shutter
    int (*confio)(int s);       // configure IO-port
    int (*setio)(int s);        // set IO-port to given state
    int (*setframetype)(int l); // set frametype: 1 - light, 0 - dark
    int (*setbitdepth)(int h);  // set bit depth : 1 - high (16 bit), 0 - low (8 bit)
    int (*setfastspeed)(int s); // set readout speed: 1 - fast, 0 - low
    // geometry (if TRUE, all args are changed to suitable values)
    int (*setgeometry)(cc_frameformat *fmt); // set geometry in UNBINNED coordinates
    int (*setfanspeed)(cc_fan_speed spd); // set fan speed
    // getters:
    int (*getbitpix)(uint8_t *bp); // get bit depth in bits per pixel (8, 12, 16 etc)
    int (*getbrightness)(float *b);// get brightnes level
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getgain)(float *g);   // get gain value
    int (*getmaxgain)(float *g);// get max available gain value
    // get limits of geometry: maximal values and steps
    int (*getgeomlimits)(cc_frameformat *max, cc_frameformat *step);
    int (*getTcold)(float *t);  // cold-side T
    int (*getThot)(float *t);   // hot-side T
    int (*getTbody)(float *t);  // body T
    int (*getbin)(int *binh, int *binv);
    int (*getio)(int *s);       // get IO-port state
    cc_hresult (*plugincmd)(const char *str, cc_charbuff *ans); // custom camera plugin command (get string as input, send string as output or NULL if failed)
    float pixX, pixY;           // pixel size in um
    cc_frameformat field;          // max field of view (full CCD field without overscans)
    cc_frameformat array;          // array format (with overscans)
    cc_frameformat geometry;       // current geometry settings (as in setgeometry)
} cc_Camera;

// focuser
typedef struct{
    int (*check)();             // check if the device is available
    int Ndevices;
    void (*close)();
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setAbsPos)(int async, float n);// set absolute position (in millimeters!!!)
    int (*home)(int async);     // home device
    // getters:
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getTbody)(float *t);  // body T
    int (*getPos)(float *p);    // current position number (starting from zero)
    int (*getMaxPos)(float *p); // max position
    int (*getMinPos)(float *p); // min position
} cc_Focuser;

// wheel
typedef struct{
    int (*check)();             // check if the device is available
    int Ndevices;
    void (*close)();
    // setters:
    int (*setDevNo)(int n);     // set active device number
    int (*setPos)(int n);       // set absolute position (starting from 0)
    // getters:
    int (*getModelName)(char *n, int l);// string with model name (l - length of n in bytes)
    int (*getTbody)(float *t);  // body T
    int (*getPos)(int *p);      // current position number (starting from zero)
    int (*getMaxPos)(int *p);   // amount of positions
} cc_Wheel;

/****** Content of old socket.h ******/

// max & min TCP socket port number
#define CC_PORTN_MAX   (65535)
#define CC_PORTN_MIN   (1024)

// Max amount of connections
#define CC_MAXCLIENTS  (30)

// wait for mutex locking
#define CC_BUSY_TIMEOUT    (1.0)
// wait for exposition ends (between subsequent check calls)
#define CC_WAIT_TIMEOUT    (2.0)
// client will disconnect after this time from last server message
#define CC_CLIENT_TIMEOUT  (3.0)

// fd - socket fd to send private messages, key, val - key and its value
typedef cc_hresult (*cc_mesghandler)(int fd, const char *key, const char *val);

typedef struct{
    cc_hresult (*chkfunction)(char *val);  // function to check device is ready
    cc_mesghandler handler;                // handler function
    const char *key;                    // keyword
} cc_handleritem;

/****** Content of old server.h ******/

typedef enum{
    CAMERA_IDLE,        // idle state, client send this to cancel capture
    CAMERA_CAPTURE,     // capturing frame, client send this to start capture
    CAMERA_FRAMERDY,    // frame ready to be saved
    CAMERA_ERROR        // can't do exposition
} cc_camera_state;

// common information about everything
#define CC_CMD_INFO        "info"
#define CC_CMD_HELP        "help"
// restart server
#define CC_CMD_RESTART     "restartTheServer"
// get image size in pixels
#define CC_CMD_IMWIDTH     "imwidth"
#define CC_CMD_IMHEIGHT    "imheight"
// get shared memory key
#define CC_CMD_SHMEMKEY    "shmemkey"

// CCD/CMOS
#define CC_CMD_PLUGINCMD   "plugincmd"
#define CC_CMD_CAMLIST     "camlist"
#define CC_CMD_CAMDEVNO    "camdevno"
#define CC_CMD_EXPOSITION  "exptime"
#define CC_CMD_LASTFNAME   "lastfilename"
#define CC_CMD_FILENAME    "filename"
#define CC_CMD_FILENAMEPREFIX "filenameprefix"
// rewrite=1 will rewrite files, =0 - not (only for `filename`)
#define CC_CMD_REWRITE     "rewrite"
#define CC_CMD_HBIN        "hbin"
#define CC_CMD_VBIN        "vbin"
#define CC_CMD_CAMTEMPER   "tcold"
#define CC_CMD_CAMFANSPD   "ccdfanspeed"
#define CC_CMD_SHUTTER     "shutter"
#define CC_CMD_CONFIO      "confio"
#define CC_CMD_IO          "io"
#define CC_CMD_GAIN        "gain"
#define CC_CMD_BRIGHTNESS  "brightness"
#define CC_CMD_FRAMEFORMAT "format"
#define CC_CMD_FRAMEMAX    "maxformat"
#define CC_CMD_NFLUSHES    "nflushes"
// expstate=CAMERA_CAPTURE will start exposition, CAMERA_IDLE - cancel
#define CC_CMD_EXPSTATE    "expstate"
#define CC_CMD_TREMAIN     "tremain"
#define CC_CMD_8BIT        "8bit"
#define CC_CMD_FASTSPD     "fastspeed"
#define CC_CMD_DARK        "dark"
#define CC_CMD_INFTY       "infty"
// FITS file keywords
#define CC_CMD_GETHEADERS  "getheaders"
#define CC_CMD_AUTHOR      "author"
#define CC_CMD_INSTRUMENT  "instrument"
#define CC_CMD_OBSERVER    "observer"
#define CC_CMD_OBJECT      "object"
#define CC_CMD_PROGRAM     "program"
#define CC_CMD_OBJTYPE     "objtype"
#define CC_CMD_HEADERFILES "headerfiles"

// focuser
#define CC_CMD_FOCLIST     "foclist"
#define CC_CMD_FDEVNO      "focdevno"
#define CC_CMD_FGOTO       "focpos"

// wheel
#define CC_CMD_WLIST       "wlist"
#define CC_CMD_WDEVNO      "wdevno"
#define CC_CMD_WPOS        "wpos"

typedef enum{ // parameter type
    CC_PAR_INT,
    CC_PAR_FLOAT,
    CC_PAR_DOUBLE,
} cc_partype_t;

typedef struct{ // custom plugin parameters
    const char *cmd;        // text parameter/command
    const char *helpstring; // help string for this parameter
    cc_hresult (*checker)(const char *str, cc_charbuff *ans); // value checker or custom handler (if don't satisfy common getter/setter); return possible answer in `ans`
    void *ptr;              // pointer to variable (if exists)
    void *min;              // min/max values of `ptr` (or NULL if don't need to check)
    void *max;
    cc_partype_t type;      // argument type
} cc_parhandler_t;

// this record should be last in cc_parhandler_t array for `cc_plugin_customcmd`
#define CC_PARHANDLER_END   {0}

cc_hresult cc_plugin_customcmd(const char *str, cc_parhandler_t *handlers, cc_charbuff *ans);

cc_Focuser *open_focuser(const char *pluginname);
cc_Camera *open_camera(const char *pluginname);
cc_Wheel *open_wheel(const char *pluginname);

cc_charbuff *cc_charbufnew();
int cc_charbuftest(cc_charbuff *b, size_t maxsize);
void cc_charbufclr(cc_charbuff *buf);
void cc_charbufdel(cc_charbuff **buf);
void cc_charbufput(cc_charbuff *b, const char *s, size_t l);
void cc_charbufaddline(cc_charbuff *b, const char *s);
cc_strbuff *cc_strbufnew(size_t size, size_t stringsize);
void cc_strbufdel(cc_strbuff **buf);

const char *cc_hresult2str(cc_hresult r);
cc_hresult cc_str2hresult(const char *str);
int cc_setNtries(int n);
int cc_getNtries();
int cc_setAnsTmout(double t);
double cc_getAnsTmout();
int cc_getNbytes(cc_IMG *image);

int cc_read2buf(int fd, cc_strbuff *buf);
int cc_refreshbuf(int fd, cc_strbuff *buf);
size_t cc_getline(cc_strbuff *b);
int cc_open_socket(int isserver, char *path, int isnet);
int cc_senddata(int fd, void *data, size_t l);
int cc_sendmessage(int fd, const char *msg, int l);
int cc_sendstrmessage(int fd, const char *msg);
char *cc_get_keyval(char *keyval);
cc_IMG *cc_getshm(key_t key, size_t imsize);
int cc_canberead(int fd);
cc_hresult cc_setint(int fd, cc_strbuff *cbuf, const char *cmd, int val);
cc_hresult cc_getint(int fd, cc_strbuff *cbuf, const char *cmd, int *val);
cc_hresult cc_setfloat(int fd, cc_strbuff *cbuf, const char *cmd, float val);
cc_hresult cc_getfloat(int fd, cc_strbuff *cbuf, const char *cmd, float *val);

char *cc_nextkw(char *buf, char record[FLEN_CARD+1], int newlines);
size_t cc_kwfromfile(cc_charbuff *b, char *filename);
int cc_charbuf2kw(cc_charbuff *b, fitsfile *f);
