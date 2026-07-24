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

#include <fitsio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "ccdfunc.h"
#include "cmdlnopts.h"
#include "socket.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif

cc_Camera *camera = NULL;
cc_Focuser *focuser = NULL;
cc_Wheel *wheel = NULL;

static float focmaxpos = 0.f, focminpos = 0.f; // focuser extremal positions
static int wmaxpos = 0; // wheel max pos

static int fitserror = 0;

#define TRYFITS(f, ...)                     \
do{ int status = 0;                         \
    f(__VA_ARGS__, &status);                \
    if(status){                             \
        fits_report_error(stderr, status);  \
        LOGERR("Fits error %d", status);    \
        fitserror = status;}                \
}while(0)
#define WRITEKEY(...)                           \
do{ int status = 0;                             \
    fits_update_key(__VA_ARGS__, &status);       \
    if(status) fits_report_error(stderr, status);\
}while(0)

#define TMBUFSIZ 40

/*
static size_t curtime(char *s_time){ // current date/time
    time_t tm = time(NULL);
    return strftime(s_time, TMBUFSIZ, "%d/%m/%Y,%H:%M:%S", localtime(&tm));
}*/

// check if I can create file prefix_XXXX.fits
static int check_filenameprefix(char *buff, int buflen){
    for(int num = 1; num < 1000000; num++){
        if(snprintf(buff, buflen-1, "%s_%06d.fits", GP->outfileprefix, num) < 1)
            return FALSE;
        struct stat filestat;
        if(stat(buff, &filestat)) // no such file or can't stat()
            return TRUE;
    }
    return FALSE;
}

// save FITS file `img` into GP->outfile or GP->outfileprefix_XXXX.fits
// if outp != NULL, put into it strdup() of last file name
// return FALSE if failed
int saveFITS(cc_IMG *img, char **outp){
    int ret = FALSE;
    if(!img || !img->data){
        WARNX("Bad data");
        return FALSE;
    }
    char fnam[PATH_MAX+1];
    if(!GP->outfile && !GP->outfileprefix){
        LOGWARN("Image not saved: neither filename nor filename prefix pointed");
        WARNX(_("Image not saved: neither filename nor filename prefix pointed"));
        return FALSE;
    }
    if(GP->outfile){ // pointed specific output file name like "file.fits", check it
        struct stat filestat;
        int s = stat(GP->outfile, &filestat);
        if(s){ // not exists
            snprintf(fnam, PATH_MAX, "%s", GP->outfile);
        }else{ // exists
            if(!GP->rewrite){
                LOGERR("Can't save image: file %s exists", GP->outfile);
                WARNX(_("File %s exists!"), GP->outfile);
                return FALSE;
            }
            snprintf(fnam, PATH_MAX, "!%s", GP->outfile);
        }
        DBG("Will save as %s", GP->outfile);
    }else{ // user pointed output file prefix
        if(!check_filenameprefix(fnam, PATH_MAX)){
            WARNX(_("Can't save file with prefix %s"), GP->outfileprefix);
            LOGERR("Can't save image with prefix %s", GP->outfileprefix);
            return FALSE;
        }
        DBG("Will save with prefix %s", GP->outfileprefix);
    }
    pthread_mutex_lock(&img->mutex);
    int width = img->w, height = img->h;
    long naxes[2] = {width, height};
    struct tm *tm_time;
    double dsavetime = sl_dtime();
    time_t savetime = time(NULL);
    fitsfile *fp;
    fitserror = 0;
    TRYFITS(fits_create_file, &fp, fnam);
    if(fitserror) goto cloerr;
    int nbytes = cc_getNbytes(img);
    if(nbytes == 1) TRYFITS(fits_create_img, fp, BYTE_IMG, 2, naxes);
    else TRYFITS(fits_create_img, fp, USHORT_IMG, 2, naxes);
    if(fitserror) goto cloerr;
    // write header
    char card[FLEN_CARD], templ[2*FLEN_CARD], bufc[FLEN_CARD];
    calculate_stat(img);
#define FORMKW(in)                              \
    do{ int status = 0, kt = 0;                      \
            fits_parse_template(in, card, &kt, &status);  \
            if(status) fits_report_error(stderr, status);\
            else cc_addrecord(fp, card); \
    }while(0)
#define FORMINT(key, val, comment) do{ \
        snprintf(templ, FLEN_CARD, "%s = %d / %s", key, val, comment); \
        FORMKW(templ); \
}while(0)
#define FORMFLT(key, val, comment) do{ \
    snprintf(templ, FLEN_CARD, "%s = %g / %s", key, val, comment); \
    FORMKW(templ); \
}while(0)
#define FORMSTR(key, val, comment) do{ \
    snprintf(templ, 2*FLEN_CARD, "%s = '%s' / %s", key, val, comment); \
    FORMKW(templ); \
}while(0)

    FORMKW("ORIGIN = 'SAO RAS' / Organization responsible for the data");
    FORMKW("OBSERVAT = 'Special Astrophysical Observatory, Russia' / Observatory name");
    if(img->model[0]) FORMSTR("DETECTOR", img->model, "Detector model");
    if(GP->instrument) FORMSTR("INSTRUME", GP->instrument, "Instrument");
    else FORMKW("INSTRUME = 'direct imaging'  / Instrument");
    FORMFLT("EXPTIME", img->exposure_time, "Actual exposition time (sec)");
    // BINNING / Binning
    snprintf(templ, FLEN_CARD, "BINNING = '%d x %d' / Binning (hbin x vbin)", img->bin_x, img->bin_y);
    FORMKW(templ);
    FORMINT("XBINNING", img->bin_x, "Binning factor used on X axis");
    FORMINT("YBINNING", img->bin_y, "Binning factor used on Y axis");
    // pixel size
    if(!isnan(img->pixel_x)){
        FORMFLT("PIXSIZEX", img->pixel_x, "Pixel size X (um)");
        if(!isnan(img->pixel_y)){
            FORMFLT("PIXSIZEY", img->pixel_y, "Pixel size Y (um)");
            snprintf(templ, FLEN_CARD, "PIXSIZE = '%.1f x %.1f' / Pixel size (h x v), um", img->pixel_x, img->pixel_y);
            FORMKW(templ);
        }
    }
    // imtype
    if(img->flags.dark) sprintf(bufc, "dark");
    else if(GP->objtype) strncpy(bufc, GP->objtype, FLEN_CARD);
    else sprintf(bufc, "light");
    FORMSTR("IMAGETYP", bufc, "Image type");
    // geometry
    snprintf(templ, FLEN_CARD, "VIEWFLD = '(%d, %d)(%d, %d)' / Camera maximal field of view", img->field.xoff, img->field.yoff,
             img->field.xoff + img->field.w - 1, img->field.yoff + img->field.h - 1);
    FORMKW(templ);
    snprintf(templ, FLEN_CARD, "ARRAYFLD = '(%d, %d)(%d, %d)' / Camera full array size (with overscans)", img->array.xoff, img->array.yoff,
             img->array.xoff + img->array.w - 1, img->array.yoff + img->array.h - 1);
    FORMKW(templ);
    snprintf(templ, FLEN_CARD, "GEOMETRY = '(%d, %d)(%d, %d)' / Camera current frame geometry", img->geometry.xoff, img->geometry.yoff,
             img->geometry.xoff + img->geometry.w - 1, img->geometry.yoff + img->geometry.h - 1);
    FORMKW(templ);
    FORMINT("X0", img->geometry.xoff, "Subframe left border without binning");
    FORMINT("Y0", img->geometry.yoff, "Subframe upper border without binning");
    // stat
    FORMINT("DATAMIN", 0, "Min pixel value");
    FORMINT("DATAMAX", (1<<img->bitpix) - 1, "Max pixel value");
    if(img->gotstat){
        FORMINT("STATMIN", img->min, "Min data value");
        FORMINT("STATMAX", img->max, "Max data value");
        FORMFLT("STATAVR", img->avr, "Average data value");
        FORMFLT("STATSTD", img->std, "Std. of data value");
    }
    // camera parameters
    if(!isnan(img->gain)) FORMFLT("CAMGAIN", img->gain, "CMOS gain value");
    if(!isnan(img->brightness)) FORMFLT("CAMBRIGH", img->brightness, "CMOS brightness value");
    if(!isnan(img->ccd_temp)) FORMFLT("CAMTEMP", img->ccd_temp, "Camera temperature at exp. end, degr C");
    if(!isnan(img->tbody)) FORMFLT("BODYTEMP", img->tbody, "Camera body temperature at exp. end, degr C");
    if(!isnan(img->thot)) FORMFLT("HOTTEMP", img->thot, "Camera peltier hot side temperature at exp. end, degr C");
    // wheel
    if(img->flags.havewheel){
        if(img->wmodel[0]) FORMSTR("WHEEL", img->wmodel, "Filter wheel model");
        if(img->wheelmax > 0) FORMINT("FILTMAX", img->wheelmax, "Amount of filter positions");
        FORMINT("FILTER", img->wheelpos, "Current filter position");
        if(!isnan(img->wheel_temp)) FORMFLT("FILTTEMP", img->wheel_temp, "Filter wheel body temperature, degr C");
    }
    // focuser
    if(img->flags.havefocuser){
        if(img->fmodel[0]) FORMSTR("FOCUSER", img->fmodel, "Focuser model");
        if(!isnan(img->focpos)) FORMFLT("FOCUS", img->focpos, "Current focuser position, mm");
        if(!isnan(img->focmin)) FORMFLT("FOCMIN", img->focmin, "Minimal focuser position, mm");
        if(!isnan(img->focmax)) FORMFLT("FOCMAX", img->focmax, "Maximal focuser position, mm");
        if(!isnan(img->foc_temp)) FORMFLT("FOCTEMP", img->foc_temp, "Focuser body temperature, degr C");
    }
    snprintf(templ, 2*FLEN_CARD, "TIMESTAM = %.6f / Time of acquisition end (UNIX)", img->timestamp);
    FORMKW(templ);
    FORMINT("IMSEQNO", (int)img->imnumber, "Number of image in full sequence");

    if(GP->addhdr){ // add records from server-side files
        char **nxtfile = GP->addhdr;
        while(*nxtfile){
            int got = cc_kwfromfile(fp, *nxtfile);
            if(got == 0) WARNX(_("Added 0 FITS records from %s"), *nxtfile);
            else verbose(VERBOSE_SECONDARY, _("Added %d FITS records from %s"), got, *nxtfile);
            ++nxtfile;
        }
    }
    // add these keywords after all to override records from files
    if(GP->observers) FORMSTR("OBSERVER", GP->observers, "Observers");
    if(GP->prog_id) FORMSTR("PROG-ID", GP->prog_id, "Observation program identifier");
    if(GP->author) FORMSTR("AUTHOR", GP->author, "Author of the program");
    if(GP->objname) FORMSTR("OBJECT", GP->objname, "Object name");

    // creation date/time
    int s = 0;
    fits_write_date(fp, &s);

    tm_time = localtime(&savetime);
    WRITEKEY(fp, TDOUBLE, "UNIXTIME", &dsavetime, "File creation time (UNIX)");
    strftime(bufc, FLEN_VALUE, "%Y/%m/%d", tm_time);
    WRITEKEY(fp, TSTRING, "DATE-OBS", bufc, "Date of observation (YYYY/MM/DD, local)");
    strftime(bufc, FLEN_VALUE, "%H:%M:%S", tm_time);
    WRITEKEY(fp, TSTRING, "TIME", bufc, "Creation time (hh:mm:ss, local)");
    // FILE / Input file original name
    char *n = fnam;
    if(*n == '!') ++n;
    s = 0; fits_write_comment(fp, "Input file original name:", &s);
    s = 0; fits_write_comment(fp, n, &s);
    //WRITEKEY(fp, TSTRING, "FILE", n, "Input file original name");
    if(nbytes == 1) TRYFITS(fits_write_img, fp, TBYTE, 1, width * height, img->data);
    else TRYFITS(fits_write_img, fp, TUSHORT, 1, width * height, img->data);

cloerr:
    TRYFITS(fits_close_file, fp);
    pthread_mutex_unlock(&img->mutex);
    if(fitserror == 0){
        LOGMSG("Save file '%s'", fnam);
        verbose(VERBOSE_PRIMARY, _("File saved as '%s'"), fnam);
        DBG("file %s saved", fnam);
        if(outp){
            FREE(*outp);
            *outp = strdup(fnam);
        }
        ret = TRUE;
    }else{
        LOGERR("Can't save %s", fnam);
        WARNX(_("Error saving file %s"), fnam);
        fitserror = 0;
    }
    return ret;
}

static void stat8(cc_IMG *image){
    double sum = 0., sum2 = 0.;
    size_t size = image->w * image->h;
    uint8_t max = 0, min = UINT8_MAX;
    uint8_t *idata = (uint8_t*)image->data;
#pragma omp parallel for reduction(+:sum, sum2) reduction(max:max) reduction(min:min)
    for(size_t i = 0; i < size; ++i){
        uint8_t val = idata[i];
        double pv = (double) val;
        sum  += pv;
        sum2 += pv * pv;
        if (val > max) max = val;
        if (val < min) min = val;
    }
    double sz = (double) size;
    image->avr = sum / sz;
    image->std = sqrt(fabs(sum2 / sz - image->avr * image->avr));
    image->max = max;
    image->min = min;
}

static void stat16(cc_IMG *image) {
    size_t size = image->w * image->h;
    uint16_t *idata = (uint16_t*) image->data;
    double sum = 0.0, sum2 = 0.0;
    uint16_t max = 0, min = UINT16_MAX;
#pragma omp parallel for reduction(+:sum, sum2) reduction(max:max) reduction(min:min)
    for(size_t i = 0; i < size; ++i){
        uint16_t val = idata[i];
        double pv = (double) val;
        sum  += pv;
        sum2 += pv * pv;
        if (val > max) max = val;
        if (val < min) min = val;
    }
    double sz = (double) size;
    image->avr = sum / sz;
    image->std = sqrt(fabs(sum2 / sz - image->avr * image->avr));
    image->max = max;
    image->min = min;
}


void calculate_stat(cc_IMG *image){
    if(!image || image->gotstat) return;
    int nbytes = ((7 + image->bitpix) / 8);
    if(nbytes == 1) stat8(image);
    else stat16(image);
    if(GP->verbose){
        printf(_("Image stat:\n"));
        printf("avr = %.1f, std = %.1f\n", image->avr, image->std);
        printf("max = %u, min = %u, size = %d pix\n", image->max, image->min, image->w * image->h);
    }
    image->gotstat = 1;
}

cc_Focuser *startFocuser(){
    if(!GP->focuserdev && !GP->commondev){
        verbose(VERBOSE_MESG, _("Focuser device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->focuserdev;
        if(!(focuser = cc_open_focuser(plugin))) return NULL;
    }
    if(!focuser->check()){
        verbose(VERBOSE_MESG, _("No focusers found"));
        focuser = NULL;
        return NULL;
    }
    return focuser;
}

/**
 * @brief setFocuserNo - set active focuser device number
 * @param num - new number
 * @param min (o) - minimal position
 * @param max (o) - maximal position
 * @return FALSE if failed
 */
int setFocuserNo(int num, float *min, float *max){
    if(!focuser) return FALSE;
    if(num < 0) num = 0;
    if(num > focuser->Ndevices - 1){
        WARNX(_("Found %d focusers, you point number %d"), focuser->Ndevices, num);
        LOGERR("Found %d focusers, user pointed number %d", focuser->Ndevices, num);
        return FALSE;
    }
    if(!focuser->setDevNo(num)){
        WARNX(_("Can't set active focuser number"));
        LOGERR("Can't set active focuser number");
        return FALSE;
    }
    LOGMSG("Set focuser device number to %d", num);
    if(!focuser->getMinPos || !focuser->getMinPos(&focminpos) ||
        !focuser->getMaxPos || !focuser->getMaxPos(&focmaxpos)){
        WARNX(_("Can't get focuser limit positions"));
        LOGWARN("Can't get focuser limit positions");
    }else{
        if(min) *min = focminpos;
        if(max) *max = focmaxpos;
    }
    return TRUE;
}

void focclose(){
    if(!focuser) return;
    focuser->close();
    focuser = NULL;
}

/*
 * Find focusers and work with each of them
 */
void focusers(){
    FNAME();
    if(!startFocuser()) return;
    if(GP->listdevices){
        for(int i = 0; i < focuser->Ndevices; ++i){
            if(!focuser->setDevNo(i)) continue;
            char modname[256];
            focuser->getModelName(modname, 255);
            verbose(VERBOSE_PRIMARY, _("Found focuser #%d: %s\n"), i, modname);
        }
    }
    if(!setFocuserNo(GP->focdevno, NULL, NULL)) return;
    char buf[BUFSIZ];
    if(focuser->getModelName(buf, BUFSIZ)){
        verbose(VERBOSE_SECONDARY, "Focuser model: %s", buf);
    }
    float t;
    if(focuser->getTbody(&t)){
        verbose(VERBOSE_PRIMARY, "FOCTEMP=%.1f", t);
        DBG("FOCTEMP=%.1f", t);
    }
    verbose(VERBOSE_PRIMARY, "FOCMINPOS=%g", focminpos);
    verbose(VERBOSE_PRIMARY, "FOCMAXPOS=%g", focmaxpos);
    float curpos;
    DBG("FOCMINPOS=%g, FOCMAXPOS=%g", focminpos, focmaxpos);
    if(!focuser->getPos(&curpos)){
        WARNX(_("Can't get current focuser position"));
        return;
    }
    verbose(VERBOSE_PRIMARY, "FOCPOS=%g", curpos);
    DBG("Curpos = %g", curpos);
    if(isnan(GP->gotopos) && isnan(GP->addsteps)) return; // no focuser commands
    float tagpos = 0.;
    if(!isnan(GP->gotopos)){ // set absolute position
        tagpos = GP->gotopos;
    }else{ // relative
        tagpos = curpos + GP->addsteps;
    }
    DBG("tagpos: %g", tagpos);
    if(tagpos < focminpos || tagpos > focmaxpos){
        WARNX(_("Can't set position %g: out of limits [%g, %g]"), tagpos, focminpos, focmaxpos);
        return;
    }
    if(tagpos - focminpos < __FLT_EPSILON__){
        if(!focuser->home(GP->async)) WARNX(_("Can't home focuser"));
    }else{
        if(!focuser->setAbsPos(GP->async, tagpos)) WARNX(_("Can't set position %g"), tagpos);
    }
}

cc_Wheel *startWheel(){
    if(!GP->wheeldev && !GP->commondev){
        verbose(VERBOSE_MESG, _("Wheel device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->wheeldev;
        if(!(wheel = cc_open_wheel(plugin))) return NULL;
    }
    if(!wheel->check()){
        verbose(VERBOSE_MESG, _("No wheels found"));
        wheel = NULL;
        return NULL;
    }
    return wheel;
}

/**
 * @brief setWheelNo - set number of active wheel
 * @param num - number
 * @param max (o) - if !NULL - maxpos
 * @return FALSE if failed
 */
int setWheelNo(int num, int *max){
    if(!wheel) return FALSE;
    if(num < 0) num = 0;
    if(num > wheel->Ndevices - 1){
        WARNX(_("Found %d wheels, you point number %d"), wheel->Ndevices, num);
        LOGERR("Found %d wheels, user pointed number %d", wheel->Ndevices, num);
        return FALSE;
    }
    if(!wheel->setDevNo(num)){
        WARNX(_("Can't set active wheel number"));
        LOGERR("Can't set active wheel number");
        return FALSE;
    }
    LOGMSG("Set wheel device number to %d", num);
    if(!wheel->getMaxPos || !wheel->getMaxPos(&wmaxpos)){
        WARNX(_("Can't get max wheel position"));
        LOGERR("Can't get max wheel position");
    }else{
        if(max) *max = wmaxpos;
    }
    return TRUE;
}

void closewheel(){
    if(!wheel) return;
    wheel->close();
    wheel = NULL;
}

/*
 * Find wheels and work with each of them
 */
void wheels(){
    FNAME();
    if(!startWheel()) return;
    if(GP->listdevices){
        for(int i = 0; i < wheel->Ndevices; ++i){
            if(!wheel->setDevNo(i)) continue;
            char modname[256];
            wheel->getModelName(modname, 255);
            verbose(VERBOSE_PRIMARY, _("Found wheel #%d: %s\n"), i, modname);
        }
    }
    if(!setWheelNo(GP->whldevno, NULL)){
        closewheel();
        return;
    }
    char buf[BUFSIZ];
    if(wheel->getModelName(buf, BUFSIZ)){
        verbose(VERBOSE_SECONDARY, _("Wheel model: %s"), buf);
    }
    float t;
    if(wheel->getTbody(&t)){
        verbose(VERBOSE_PRIMARY, "WHEELTEMP=%.1f", t);
    }
    int pos;
    if(wheel->getPos(&pos)){
        verbose(VERBOSE_PRIMARY, "WHEELPOS=%d", pos);
    }else WARNX(_("Can't get current wheel position"));
    verbose(VERBOSE_PRIMARY, "WHEELMAXPOS=%d", wmaxpos);
    pos = GP->setwheel;
    if(pos == -1) return; // no wheel commands
    if(pos < 0 || pos > wmaxpos){
        WARNX(_("Wheel position should be from 0 to %d"), wmaxpos);
        return;
    }
    if(!wheel->setPos(pos))
        WARNX(_("Can't set wheel position %d"), pos);
}

static cc_capture_status capt(){
    cc_capture_status cs;
    float tremain, tmpf;
    if(!camera || !camera->pollcapture){
        WARNX(_("Camera plugin have no capture polling funtion."));
        return CAPTURE_ABORTED;
    }
    while(camera->pollcapture(&cs, &tremain)){
        if(cs != CAPTURE_PROCESS) break;
        if(tremain > 0.1){
            verbose(VERBOSE_SECONDARY, _("%.1f seconds till exposition ends"), tremain);
            if(camera->getTcold && camera->getTcold(&tmpf)) verbose(VERBOSE_PRIMARY, "CCDTEMP=%.1f", tmpf);
            if(camera->getTbody && camera->getTbody(&tmpf)) verbose(VERBOSE_PRIMARY, "BODYTEMP=%.1f", tmpf);
        }
        if(tremain > 6.) sleep(5);
        else if(tremain > 0.999999) sleep((int)(tremain+1e-6));
        else usleep((int)(1e6*tremain));
        if(!camera) return CAPTURE_ABORTED;
    }
    DBG("Poll ends with %d", cs);
    return cs;
}

cc_Camera *startCCD(){
    if(!GP->cameradev && !GP->commondev){
        verbose(VERBOSE_MESG, _("Camera device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->cameradev;
        if(!(camera = cc_open_camera(plugin))) return NULL;
    }
    if(!camera->check()){
        WARNX(_("No cameras found"));
        LOGWARN(_("No cameras found"));
        return NULL;
    }
    return camera;
}

void closecam(){
    if(!camera) return;
    DBG("Close cam");
    if(camera->close) camera->close();
    camera = NULL;
}

// make base settings; return TRUE if all OK
int prepare_ccds(){
    FNAME();
    char buf[BUFSIZ];
    int rtn = FALSE;
    if(!startCCD()) return FALSE;
    if(GP->listdevices){
        if(!camera->getModelName) WARNX(_("Camera plugin have no model name getter"));
        else for(int i = 0; i < camera->Ndevices; ++i){
            if(camera->setDevNo && !camera->setDevNo(i)) continue;
            // if camera have no setDevNo function, it could have getModelName
            camera->getModelName(buf, BUFSIZ-1);
            verbose(VERBOSE_PRIMARY, _("Found camera #%d: %s\n"), i, buf);
        }
    }
    int num = GP->camdevno;
    if(num < 0) num = 0;
    if(num > camera->Ndevices - 1){
        WARNX(_("Found %d cameras, you point number %d"), camera->Ndevices, num);
        goto retn;
    }
    if(camera->setDevNo && !camera->setDevNo(num)){
        WARNX(_("Can't set active camera number"));
        goto retn;
    }
    // run plugincmd handler if available
    if(GP->plugincmd){
        DBG("Plugincmd");
        if(!camera->plugincmd) ERRX(_("Camera plugin have no custom commands"));
        else{
            char **p = GP->plugincmd;
            cc_charbuff *b = cc_charbufnew();
            int stop = FALSE;
            while(p && *p){
                DBG("got %s", *p);
                cc_charbufclr(b);
                cc_hresult r = camera->plugincmd(*p, b);
                if(r == CC_RESULT_OK || r == CC_RESULT_SILENCE) green(_("Command '%s'"), *p);
                else{
                    stop = TRUE;
                    red(_("Command '%s'"), *p);
                }
                if(r != CC_RESULT_SILENCE) printf(_(" returns \"%s\""), cc_hresult2str(r));
                if(b->buflen) printf("\n%s", b->buf);
                else printf("\n");
                ++p;
            }
            cc_charbufdel(&b);
            if(stop) signals(9);
        }
    }
    if(GP->fanspeed > -1){
        if(GP->fanspeed > FAN_HIGH) GP->fanspeed = FAN_HIGH;
        if(!camera->setfanspeed) WARNX(_("Camera plugin have no fun speed setter"));
        else{
            if(!camera->setfanspeed((cc_fan_speed)GP->fanspeed))
                WARNX(_("Can't set fan speed"));
            else verbose(0, _("Set fan speed to %d"), GP->fanspeed);
        }
    }
    int x0,x1, y0,y1; // will be inited here and used later
    if(camera->getModelName && camera->getModelName(buf, BUFSIZ))
        verbose(VERBOSE_SECONDARY, _("Camera model: %s"), buf);
    verbose(VERBOSE_SECONDARY, _("Pixel size: %g x %g"), camera->pixX, camera->pixY);
    x0 = camera->array.xoff;
    y0 = camera->array.yoff;
    x1 = camera->array.xoff + camera->array.w - 1;
    y1 = camera->array.yoff + camera->array.h - 1;
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", x0, y0, x1, y1);
    verbose(VERBOSE_SECONDARY, _("Full array: %s"), buf);
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", camera->field.xoff, camera->field.yoff,
                camera->field.xoff + camera->field.w - 1, camera->field.yoff + camera->field.h - 1);
    verbose(VERBOSE_SECONDARY, _("Field of view: %s"), buf);
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", camera->geometry.xoff, camera->geometry.yoff,
                camera->geometry.xoff + camera->geometry.w - 1, camera->geometry.yoff + camera->geometry.h - 1);
    verbose(VERBOSE_SECONDARY, _("Current format: %s"), buf);
    cc_frameformat max, step;
    if(camera->getgeomlimits && camera->getgeomlimits(&max, &step)){
        verbose(VERBOSE_SECONDARY, _("Maximal geometry (WxH): %dx%d"), max.w, max.h);
        verbose(VERBOSE_SECONDARY, _("Minimal offset: %dx%d"), max.xoff, max.yoff);
        verbose(VERBOSE_SECONDARY, _("Geometry steps: offset - %dx%d, size - %dx%d"), step.xoff, step.yoff, step.w, step.h);
    }
    int tmpi, tmpi1;
    if(camera->getbin && camera->getbin(&tmpi, &tmpi1))
        verbose(VERBOSE_SECONDARY, _("Current binning: %d x %d"), tmpi, tmpi1);
    uint8_t u8;
    if(camera->getbitpix && camera->getbitpix(&u8))
        verbose(VERBOSE_SECONDARY, _("Current settings: %d bits per pixel"), u8);
    float tmpf;
    if(camera->getbrightness && camera->getbrightness(&tmpf))
        verbose(VERBOSE_SECONDARY, _("Current brightness: %g"), tmpf);
    if(camera->getgain && camera->getgain(&tmpf))
        verbose(VERBOSE_SECONDARY, _("Current gain: %g"), &tmpf);
    if(camera->getmaxgain && camera->getmaxgain(&tmpf))
        verbose(VERBOSE_SECONDARY, _("Maximal gain: %g"), tmpf);
    if(!isnan(GP->temperature)){
        if(!camera->setT)WARNX(_("Camera plugin have no temperature setter"));
        else{if(!camera->setT((float)GP->temperature)) WARNX(_("Can't set T to %g degC"), GP->temperature);
            else verbose(VERBOSE_MESG, "SetT=%.1f", GP->temperature);
        }
    }
    if(camera->getTcold && camera->getTcold(&tmpf)) verbose(VERBOSE_PRIMARY, "CCDTEMP=%.1f", tmpf);
    if(camera->getTbody && camera->getTbody(&tmpf)) verbose(VERBOSE_PRIMARY, "BODYTEMP=%.1f", tmpf);
    if(GP->shtr_cmd > -1 && GP->shtr_cmd < SHUTTER_AMOUNT){
        const char *str[] = {"open", "close", "expose @high", "expose @low"};
        verbose(VERBOSE_PRIMARY, _("Shutter command: %s\n"), str[GP->shtr_cmd]);
        if(!camera->shuttercmd || !camera->shuttercmd((cc_shutter_op)GP->shtr_cmd))
            WARNX(_("Can't run shutter command %s (unsupported?)"), str[GP->shtr_cmd]);
    }
    if(GP->confio > -1){
        verbose(VERBOSE_PRIMARY, _("Try to configure I/O port as %d"), GP->confio);
        if(!camera->confio || !camera->confio(GP->confio))
            WARNX(_("Can't configure (unsupported?)"));
    }
    if(GP->getio){
        if(camera->getio && camera->getio(&tmpi))
            verbose(0, "CCDIOPORT=0x%02X\n", tmpi);
        else
            WARNX(_("Can't get IOport state (unsupported?)"));
    }
    if(GP->setio > -1){
        verbose(VERBOSE_PRIMARY, _("Try to write %d to I/O port"), GP->setio);
        if(!camera->setio || !camera->setio(GP->setio))
            WARNX(_("Can't set IOport"));
    }
    if(GP->exptime < 0.) goto retn;
    if(!isnan(GP->gain)){
        DBG("Change gain to %g", GP->gain);
        if(camera->setgain && camera->setgain(GP->gain)){
            if(camera->getgain) camera->getgain(&GP->gain);
            verbose(VERBOSE_PRIMARY, _("Set gain to %g"), GP->gain);
        }else WARNX(_("Can't set gain to %g"), GP->gain);
    }
    if(!isnan(GP->brightness)){
        if(camera->setbrightness && camera->setbrightness(GP->brightness)){
            if(camera->getbrightness) camera->getbrightness(&GP->brightness);
            verbose(VERBOSE_PRIMARY, _("Set brightness to %g"), GP->brightness);
        }else WARNX(_("Can't set brightness to %g"), GP->brightness);
    }
    /*********************** expose control ***********************/
    if(GP->hbin < 1) GP->hbin = 1;
    if(GP->vbin < 1) GP->vbin = 1;
    if(!camera->setbin || !camera->setbin(GP->hbin, GP->vbin)){
        WARNX(_("Can't set binning %dx%d"), GP->hbin, GP->vbin);
        if(camera->getbin) camera->getbin(&GP->hbin, &GP->vbin);
    }
    if(GP->X0 < x0) GP->X0 = x0; // default values
    else if(GP->X0 > x1-1) GP->X0 = x1-1;
    if(GP->Y0 < y0) GP->Y0 = y0;
    else if(GP->Y0 > y1-1) GP->Y0 = y1-1;
    if(GP->X1 < GP->X0+1 || GP->X1 > x1) GP->X1 = x1;
    if(GP->Y1 < GP->Y0+1 || GP->Y1 > y1) GP->Y1 = y1;
    DBG("x1/x0: %d/%d", GP->X1, GP->X0);
    cc_frameformat fmt = {.w = GP->X1 - GP->X0 + 1, .h = GP->Y1 - GP->Y0 + 1, .xoff = GP->X0, .yoff = GP->Y0};
    if(!camera->setgeometry || !camera->setgeometry(&fmt))
        ERRX(_("Can't set given geometry"));
    verbose(VERBOSE_MESG, "Geometry: off=%d/%d, wh=%d/%d", fmt.xoff, fmt.yoff, fmt.w, fmt.h);
    if(GP->nflushes > 0){
        if(!camera->setnflushes || !camera->setnflushes(GP->nflushes))
            WARNX(_("Can't set %d flushes"), GP->nflushes);
        else verbose(VERBOSE_MESG, "Nflushes=%d", GP->nflushes);
    }
    if(!camera->setexp) ERRX(_("Camera plugin have no exposition setter"));
    if(!camera->setexp(GP->exptime))
        ERRX(_("Can't set exposure time to %f seconds"), GP->exptime);
    tmpi = (GP->dark) ? 0 : 1;
    if(!camera->setframetype || !camera->setframetype(tmpi))
        WARNX(_("Can't change frame type"));
    tmpi = (GP->_8bit) ? 0 : 1;
    if(!camera->setbitdepth || !camera->setbitdepth(tmpi))
        WARNX(_("Can't set bit depth"));
    if(!camera->setfastspeed || !camera->setfastspeed(GP->fast))
        WARNX(_("Can't set readout speed"));
    else verbose(VERBOSE_PRIMARY, _("Readout mode: %s"), GP->fast ? "fast" : "normal");
    if(!GP->outfile) verbose(VERBOSE_PRIMARY, _("Only show statistics"));
    // GET binning should be AFTER setgeometry!
    if(!camera->getbin || !camera->getbin(&GP->hbin, &GP->vbin))
        WARNX(_("Can't get current binning"));
    verbose(VERBOSE_SECONDARY, "Binning: %d x %d", GP->hbin, GP->vbin);
    rtn = TRUE;
retn:
    if(!rtn) closecam();
    return rtn;
}

/**
 * @brief image_init_camdata - init image fields with base camera data
 * @param img (io) - image
 * @param cam etc - camera, focuser and wheel devices
 * @return FALSE if failed
 */
int image_init_camdata(cc_IMG *ima){
    if(!ima ) return FALSE;
    if(camera->getModelName){
        if(camera->getModelName(ima->model, MODELNM_SZ-1)) ima->model[MODELNM_SZ-1] = 0;
        else ima->model[0] = 0;
    }
    ima->pixel_x = camera->pixX;
    ima->pixel_y = camera->pixY;
    // fill constant parameters
    if(wheel){
        int i;
        if(wheel->getModelName){
            if(wheel->getModelName(ima->wmodel, MODELNM_SZ-1)) ima->wmodel[MODELNM_SZ-1] = 0;
            else ima->wmodel[0] = 0;
        }
        if(wheel->getMaxPos && wheel->getMaxPos(&i)) ima->wheelmax = i;
        else ima->wheelmax = 0;
    }
    if(focuser){
        if(focuser->getModelName){
            if(focuser->getModelName(ima->fmodel, MODELNM_SZ-1)) ima->fmodel[MODELNM_SZ-1] = 0;
            else ima->fmodel[0] = 0;
        }
        if(!focuser->getMinPos || !focuser->getMinPos(&ima->focmin)) ima->focmin = NAN;
        if(!focuser->getMaxPos || !focuser->getMaxPos(&ima->focmax)) ima->focmax = NAN;
    }
    return TRUE;
}

// fill base fields of fresh image
void fill_image_fields(cc_IMG *ima){
    if(!ima) return;
    ima->gotstat = 0; // fresh image without statistics - recalculate when save
    ima->timestamp = sl_dtime(); // set timestamp
    if(!camera->getgain || !camera->getgain(&ima->gain)) ima->gain = NAN;
    if(!camera->getbrightness || !camera->getbrightness(&ima->brightness)) ima->brightness = NAN;
    if(!camera->getTcold || !camera->getTcold(&ima->ccd_temp)){
        DBG("Can't get CCD temperature");
        ima->ccd_temp = NAN;
    }else DBG("CCD Temperature=%g", ima->ccd_temp);
    if(!camera->getTbody || !camera->getTbody(&ima->tbody)){
        DBG("Can't get body temperature");
        ima->tbody = NAN;
    }else DBG("Body Temperature=%g", ima->tbody);
    if(!camera->getThot || !camera->getThot(&ima->thot)){
        DBG("Can't get Thot");
        ima->thot = NAN;
    }else DBG("Hot Temperature=%g", ima->thot);
    ima->flags.dark = GP->dark;
    ima->field = camera->field;
    ima->array = camera->array;
    ima->geometry = camera->geometry;
    DBG("Geom: off(%d, %d), size(%d, %d)", ima->geometry.xoff, ima->geometry.yoff,
            ima->geometry.w, ima->geometry.h);
    if(wheel){
        int i;
        ima->flags.havewheel = 1;
        if(wheel->getPos(&i)) ima->wheelpos = (uint8_t) i;
        ima->wheelmax = wmaxpos;
        if(!wheel->getTbody || !wheel->getTbody(&ima->wheel_temp)) ima->wheel_temp = NAN;
    }
    if(focuser){
        ima->flags.havefocuser = 1;
        ima->focmin = focminpos;
        ima->focmax = focmaxpos;
        if(!focuser->getPos || !focuser->getPos(&ima->focpos)) ima->focpos = NAN;
        if(!focuser->getTbody || !focuser->getTbody(&ima->foc_temp)) ima->foc_temp = NAN;
    }
}

/*
 * Main CCD process in standalone mode without viewer: get N images and save them
 */
void ccds(){
    FNAME();
    cc_frameformat fmt = camera->geometry;
    int raw_width = fmt.w / GP->hbin,  raw_height = fmt.h / GP->vbin;
    DBG("w=%d, h=%d", raw_width, raw_height);
    uint8_t bitpix = 16;
    if(camera->getbitpix) camera->getbitpix(&bitpix);
    cc_IMG *image = cc_newimage(bitpix, raw_width, raw_height);
    if(!image) ERRX(_("Can't allocate image memory"));
    if(!image_init_camdata(image)) WARNX(_("Can't fill headers with camera data"));
    image->exposure_time = GP->exptime;
    image->bin_x = GP->hbin;
    image->bin_y = GP->vbin;
    if(GP->nframes < 1) GP->nframes = 1;
    for(int j = 0; j < GP->nframes; ++j){
        TIMEINIT();
        TIMESTAMP("Start next cycle");
        verbose(VERBOSE_PRIMARY, _("Capture frame %d"), j);
        if(!camera->startexposition) ERRX(_("Camera plugin have no function `start exposition`"));
        if(!camera->startexposition()){
            WARNX(_("Can't start exposition"));
            break;
        }
        TIMESTAMP("Check capture");
        if(capt() != CAPTURE_READY){
            WARNX(_("Can't capture image"));
            break;
        }
        verbose(VERBOSE_SECONDARY, _("Read grabbed image"));
        TIMESTAMP("Read grabbed");
        if(!camera->capture) ERRX(_("Camera plugin have no function `capture`"));
        if(!camera->capture(image)){
            WARNX(_("Can't grab image"));
            break;
        }
        fill_image_fields(image);
        ++image->imnumber;
        TIMESTAMP("Calc stat");
        calculate_stat(image);
        TIMESTAMP("Save fits");
        saveFITS(image, NULL);
        TIMESTAMP("Ready");
        if(GP->pause_len && j != (GP->nframes - 1)){
            double delta, time1 = sl_dtime() + GP->pause_len;
            while((delta = time1 - sl_dtime()) > 0.){
                verbose(VERBOSE_PRIMARY, _("%d seconds till pause ends\n"), (int)delta);
                float tmpf;
                if(camera->getTcold && camera->getTcold(&tmpf)) verbose(VERBOSE_PRIMARY, "CCDTEMP=%.1f\n", tmpf);
                if(camera->getTbody && camera->getTbody(&tmpf)) verbose(VERBOSE_PRIMARY, "BODYTEMP=%.1f\n", tmpf);
                if(delta > 6.) sleep(5);
                else if(delta > 0.9) sleep((int)(delta+0.99));
                else usleep((int)(delta*1e6 + 1));
            }
        }
    }
    DBG("FREE img");
    cc_freeimage(&image);
    closecam();
}

// cancel expositions and close camera devise
void camstop(){
    if(camera){
        if(camera->cancel) camera->cancel();
        if(camera->close) camera->close();
    }
}

#ifdef IMAGEVIEW
#define NFRM        (10)
void framerate(){
    if(GP->verbose == 0) return;
    static double tlast = 0., lastn[NFRM] = {0.}, sumn = 0.;
    static int lastidx = 0;
    if(tlast == 0.){tlast = sl_dtime(); return;}
    double t = sl_dtime(), dT = t-tlast;
    if(++lastidx > NFRM-1) lastidx = 0;
    sumn = sumn - lastn[lastidx] + dT;
    lastn[lastidx] = dT;
    //for(int i = 0; i < NFRM; ++i) printf("last[%d]=%g\n", i, lastn[i]);
    verbose(VERBOSE_SECONDARY, _("Framerate=%.2f (%g seconds for exp); mean framerate=%.2f"), 1./dT, dT, NFRM/sumn);
    tlast = t;
}

static volatile int exitgrab = FALSE;
static volatile size_t lastgrabno = 0;
static void *grabnext(void *arg){
    FNAME();
    if(!arg) return NULL;
    cc_IMG *ima = (cc_IMG*)arg;
    do{
        if(exitgrab) return NULL;
        TIMESTAMP("Start next exp");
        TIMEINIT();
        if(!camera) return NULL;
        if(!camera->startexposition) ERRX(_("Camera plugin have no function `start exposition`"));
        if(!camera->startexposition()){
            ERRX(_("Can't start exposition"));
        }
        cc_capture_status cs = CAPTURE_ABORTED;
        TIMESTAMP("Poll");
        while(camera->pollcapture && camera->pollcapture(&cs, NULL)){
            if(cs != CAPTURE_PROCESS) break;
            usleep(10000);
            if(!camera) return NULL;
        }
        if(cs != CAPTURE_READY){ WARNX(_("Some error when capture")); return NULL;}
        TIMESTAMP("get");
        if(!camera->capture) ERRX(_("Camera plugin have no function `capture`"));
        if(!camera->capture(ima)){ WARNX(_("Can't grab image")); continue; }
        ++ima->imnumber;
        //calculate_stat(ima);
        TIMESTAMP("OK");
    }while(1);
    return NULL;
}

/**
 * @brief ccdcaptured - get new image data for viewer
 * @param img - pointer to cc_IMG* (if cc_IMG* is NULL, will be allocated here)
 * @return TRUE if new image available
 */
int ccdcaptured(cc_IMG *imgptr){
    if(!imgptr) return FALSE;
    //TIMESTAMP("ccdcaptured() start");
    static pthread_t grabthread = 0;
    if(imgptr == (void*)-1){ // kill `grabnext`
        DBG("Wait for grabbing thread ends");
        if(grabthread){
            exitgrab = TRUE;
            pthread_join(grabthread, NULL);
            grabthread = 0;
        }
        DBG("OK");
        return FALSE;
    }

    if(!grabthread){ // start new grab
        TIMESTAMP("Start new grab");
        TIMEINIT();
        if(pthread_create(&grabthread, NULL, &grabnext, (void*)imgptr)){
            WARN(_("Can't create grabbing thread"));
            grabthread = 0;
        }
    }else{ // grab in process
        if(imgptr->imnumber != lastgrabno){ // done
            lastgrabno = imgptr->imnumber;
            TIMESTAMP("Got exp #%zd", lastgrabno);
            framerate();
            return TRUE;
        }
    }
    //TIMESTAMP("ccdcaptured() end");
    return FALSE;
}
#endif

// common part of client-server
#include "client.h"
#include "server.h"
/**
 * @brief start_socket - create socket and run client or server
 * @param isserver  - TRUE for server, FALSE for client
 * @return 0 if OK
 */
int start_socket(int isserver){
    char *path = NULL;
    int isnet = 0;
    if(GP->path) path = GP->path;
    else if(GP->port){ path = GP->port; isnet = 1; }
    else ERRX(_("Point network port or UNIX-socket path"));
    int sock = cc_open_socket(isserver, path, isnet), imsock = -1;
    if(sock < 0){
        LOGERR("Can't open socket");
        ERRX(_("start_socket(): can't open socket"));
    }
    if(isserver){
        imsock = cc_open_socket(TRUE, GP->imageport, 2); // image socket should be networked
        DBG("imsock=%d, image port=%s", imsock, GP->imageport);
        server(sock, imsock);
    }else{
#ifdef IMAGEVIEW
        if(GP->showimage){
            if(!GP->viewer && GP->exptime < 0.00001) ERRX(_("Need exposition time!"));
            init_grab_sock(sock);
            viewer(sockcaptured); // start viewer with socket client parser
            DBG("done");
        }else
#endif
            client(sock);
    }
    DBG("Close socket");
    close(sock);
    if(isserver) close(imsock);
    return 0;
}
