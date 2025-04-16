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
#include <signal.h> // pthread_kill
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
#include "omp.h"

cc_Camera *camera = NULL;
cc_Focuser *focuser = NULL;
cc_Wheel *wheel = NULL;

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
    for(int num = 1; num < 10000; num++){
        if(snprintf(buff, buflen-1, "%s_%04d.fits", GP->outfileprefix, num) < 1)
            return FALSE;
        struct stat filestat;
        if(stat(buff, &filestat)) // no such file or can't stat()
            return TRUE;
    }
    return FALSE;
}

cc_charbuff *getFITSheader(cc_IMG *img){
    static cc_charbuff charbuf = {0};
    cc_charbufclr(&charbuf);
    char card[FLEN_CARD+1], templ[2*FLEN_CARD+1], bufc[FLEN_CARD+1];
#define FORMKW(in)                              \
do{ int status = 0, kt = 0;                      \
    fits_parse_template(in, card, &kt, &status);  \
    if(status) fits_report_error(stderr, status);\
    else{cc_charbufaddline(&charbuf, card);} \
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
    calculate_stat(img);
    float tmpf = 0.0;
    int tmpi = 0;
   /* FORMKW("COMMENT this is just a test comment - 0");
    FORMKW("COMMENT this is just a test comment - 1");
    FORMKW("COMMENT this is just a test comment - 2");
    FORMKW("HIERARCH longsome1 = 'this is just a test comment'");
    FORMKW("HIERARCH longsome2 = 'this is just a test comment'");
    FORMKW("HIERARCH longsome3 = 'this is just a test comment'");*/
    FORMKW("ORIGIN = 'SAO RAS' / Organization responsible for the data");
    FORMKW("OBSERVAT = 'Special Astrophysical Observatory, Russia' / Observatory name");
    FORMKW("INSTRUME = 'direct imaging'  / Instrument");
    snprintf(templ, FLEN_CARD, "VIEWFLD = '(%d, %d)(%d, %d)' / Camera maximal field of view", camera->field.xoff, camera->field.yoff,
             camera->field.xoff + camera->field.w, camera->field.yoff + camera->field.h);
    FORMKW(templ);
    snprintf(templ, FLEN_CARD, "ARRAYFLD = '(%d, %d)(%d, %d)' / Camera full array size (with overscans)", camera->array.xoff, camera->array.yoff,
             camera->array.xoff + camera->array.w, camera->array.yoff + camera->array.h);
    FORMKW(templ);
    snprintf(templ, FLEN_CARD, "GEOMETRY = '(%d, %d)(%d, %d)' / Camera current frame geometry", camera->geometry.xoff, camera->geometry.yoff,
             camera->geometry.xoff + camera->geometry.w, camera->geometry.yoff + camera->geometry.h);
    FORMKW(templ);
    if(GP->X0 > -1) FORMINT("X0", GP->X0, "Subframe left border without binning");
    if(GP->Y0 > -1) FORMINT("Y0", GP->Y0, "Subframe upper border without binning");
    if(GP->dark) sprintf(bufc, "dark");
    else if(GP->objtype) strncpy(bufc, GP->objtype, FLEN_CARD);
    else sprintf(bufc, "light");
    FORMSTR("IMAGETYP", bufc, "Image type");
    FORMINT("DATAMIN", 0, "Min pixel value");
    FORMINT("DATAMAX", (1<<img->bitpix) - 1, "Max pixel value");
    FORMINT("STATMIN", img->min, "Min data value");
    FORMINT("STATMAX", img->max, "Max data value");
    FORMFLT("STATAVR", img->avr, "Average data value");
    FORMFLT("STATSTD", img->std, "Std. of data value");
    if(camera->getTcold && camera->getTcold(&tmpf))
        FORMFLT("CAMTEMP", tmpf, "Camera temperature at exp. end, degr C");
    if(camera->getTbody && camera->getTbody(&tmpf))
        FORMFLT("BODYTEMP", tmpf, "Camera body temperature at exp. end, degr C");
    if(camera->getThot && camera->getThot(&tmpf))
        FORMFLT("HOTTEMP", tmpf, "Camera peltier hot side temperature at exp. end, degr C");
    FORMFLT( "EXPTIME", GP->exptime, "Actual exposition time (sec)");
    if(camera->getgain && camera->getgain(&tmpf))
        FORMFLT("CAMGAIN", tmpf, "CMOS gain value");
    if(camera->getbrightness && camera->getbrightness(&tmpf))
        FORMFLT("CAMBRIGH", tmpf, "CMOS brightness value");

    snprintf(templ, 2*FLEN_CARD, "TIMESTAM = %.3f / Time of acquisition end (UNIX)", img->timestamp);
    FORMKW(templ);
    // BINNING / Binning
    snprintf(templ, FLEN_CARD, "BINNING = '%d x %d' / Binning (hbin x vbin)", GP->hbin, GP->vbin);
    FORMKW(templ);
    FORMINT("XBINNING", GP->hbin, "Binning factor used on X axis");
    FORMINT("YBINNING", GP->vbin, "Binning factor used on Y axis");
    if(focuser){ // there is a focuser device - add info
        if(focuser->getModelName && focuser->getModelName(bufc, FLEN_CARD))
            FORMSTR("FOCUSER", bufc, "Focuser model");
        if(focuser->getPos && focuser->getPos(&tmpf))
            FORMFLT("FOCUS", tmpf, "Current focuser position, mm");
        if(focuser->getMinPos && focuser->getMinPos(&tmpf))
            FORMFLT("FOCMIN", tmpf, "Minimal focuser position, mm");
        if(focuser->getMaxPos && focuser->getMaxPos(&tmpf))
            FORMFLT("FOCMAX", tmpf, "Maximal focuser position, mm");
        if(focuser->getTbody && focuser->getTbody(&tmpf))
            FORMFLT("FOCTEMP", tmpf, "Focuser body temperature, degr C");
    }
    if(wheel){ // there is a filter wheel device - add info
        if(wheel->getModelName && wheel->getModelName(bufc, FLEN_CARD)){
            FORMSTR("WHEEL", bufc, "Filter wheel model");
        }
        if(wheel->getPos && wheel->getPos(&tmpi))
            FORMINT("FILTER", tmpi, "Current filter number");
        if(wheel->getMaxPos && wheel->getMaxPos(&tmpi))
            FORMINT("FILTMAX", tmpi, "Amount of filter positions");
        if(wheel->getTbody && wheel->getTbody(&tmpf))
            FORMFLT("FILTTEMP", tmpf, "Filter wheel body temperature, degr C");
    }
    if(GP->addhdr){ // add records from files
        char **nxtfile = GP->addhdr;
        while(*nxtfile){
            cc_kwfromfile(&charbuf, *(nxtfile++));
        }
    }
    // add these keywords after all to override records from files
    if(GP->observers) FORMSTR("OBSERVER", GP->observers, "Observers");
    if(GP->prog_id) FORMSTR("PROG-ID", GP->prog_id, "Observation program identifier");
    if(GP->author) FORMSTR("AUTHOR", GP->author, "Author of the program");
    if(GP->objname) FORMSTR("OBJECT", GP->objname, "Object name");
    if(camera->getModelName && camera->getModelName(bufc, FLEN_CARD))
        FORMSTR("DETECTOR", bufc, "Detector model");
    if(GP->instrument) FORMSTR("INSTRUME", GP->instrument, "Instrument");
    return &charbuf;
}

// save FITS file `img` into GP->outfile or GP->outfileprefix_XXXX.fits
// if outp != NULL, put into it strdup() of last file name
// return FALSE if failed
int saveFITS(cc_IMG *img, char **outp){
    int ret = FALSE;
    if(!camera){
        LOGERR("Can't save image: no camera device");
        WARNX(_("Camera device unknown"));
        return FALSE;
    }
    char fnam[PATH_MAX+1];
    if(!GP->outfile && !GP->outfileprefix){
        LOGWARN("Image not saved: neither filename nor filename prefix pointed");
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
                WARNX("File %s exists!", GP->outfile);
                return FALSE;
            }
            snprintf(fnam, PATH_MAX, "!%s", GP->outfile);
        }
    }else{ // user pointed output file prefix
        if(!check_filenameprefix(fnam, PATH_MAX)){
            WARNX(_("Can't save file with prefix %s"), GP->outfileprefix);
            LOGERR("Can't save image with prefix %s", GP->outfileprefix);
        }
    }
    int width = img->w, height = img->h;
    long naxes[2] = {width, height};
    struct tm *tm_time;
    char bufc[FLEN_CARD];
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
    cc_charbuff *bufhdr = getFITSheader(img);
    cc_charbuf2kw(bufhdr, fp);
    // creation date/time
    int s = 0;
    fits_write_date(fp, &s);
/*
s = 0;
fits_write_comment(fp, bufhdr->buf, &s);
s = 0;
fits_write_history(fp, bufhdr->buf, &s);
*/
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
    if(fitserror) goto cloerr;
    TRYFITS(fits_close_file, fp);
cloerr:
    if(fitserror == 0){
        LOGMSG("Save file '%s'", fnam);
        verbose(1, _("File saved as '%s'"), fnam);
        DBG("file %s saved", fnam);
        if(outp){
            FREE(*outp);
            *outp = strdup(fnam);
        }
        ret = TRUE;
    }else{
        LOGERR("Can't save %s", fnam);
        WARNX(_("Error saving file"));
        fitserror = 0;
    }
    return ret;
}

static void stat8(cc_IMG *image){
    double sum = 0., sum2 = 0.;
    size_t size = image->w * image->h;
    uint8_t max = 0, min = UINT8_MAX;
    uint8_t *idata = (uint8_t*)image->data;
#pragma omp parallel
{
    uint8_t maxpriv = 0, minpriv = UINT8_MAX;
    double sumpriv = 0., sum2priv = 0.;
    #pragma omp for nowait
    for(size_t i = 0; i < size; ++i){
        uint8_t val = idata[i];
        float pv = (float) val;
        sum += pv;
        sum2 += (pv * pv);
        if(max < val) max = val;
        if(min > val) min = val;
    }
    #pragma omp critical
    {
        if(max < maxpriv) max = maxpriv;
        if(min > minpriv) min = minpriv;
        sum += sumpriv;
        sum2 += sum2priv;
    }
}
    double sz = (float)size;
    double avr = sum/sz;
    image->avr = avr;
    image->std = sqrt(fabs(sum2/sz - avr*avr));
    image->max = max; image->min = min;
}
static void stat16(cc_IMG *image){
    double sum = 0., sum2 = 0.;
    size_t size = image->w * image->h;
    uint16_t max = 0, min = UINT16_MAX;
    uint16_t *idata = (uint16_t*)image->data;
#pragma omp parallel
{
    uint16_t maxpriv = 0, minpriv = UINT16_MAX;
    double sumpriv = 0., sum2priv = 0.;
    #pragma omp for nowait
    for(size_t i = 0; i < size; ++i){
        uint16_t val = idata[i];
        float pv = (float) val;
        sum += pv;
        sum2 += (pv * pv);
        if(max < val) max = val;
        if(min > val) min = val;
    }
    #pragma omp critical
    {
        if(max < maxpriv) max = maxpriv;
        if(min > minpriv) min = minpriv;
        sum += sumpriv;
        sum2 += sum2priv;
    }
}
    double sz = (float)size;
    double avr = sum/sz;
    image->avr = avr;
    image->std = sqrt(fabs(sum2/sz - avr*avr));
    image->max = max; image->min = min;
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
        verbose(3, _("Focuser device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->focuserdev;
        if(!(focuser = open_focuser(plugin))) return NULL;
    }
    if(!focuser->check()){
        verbose(3, _("No focusers found"));
        focuser = NULL;
        return NULL;
    }
    return focuser;
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
    void *dlh = NULL;
    if(!startFocuser(&dlh)) return;
    if(GP->listdevices){
        for(int i = 0; i < focuser->Ndevices; ++i){
            if(!focuser->setDevNo(i)) continue;
            char modname[256];
            focuser->getModelName(modname, 255);
            printf("Found focuser #%d: %s\n", i, modname);
        }
    }
    int num = GP->focdevno;
    if(num < 0) num = 0;
    if(num > focuser->Ndevices - 1){
        WARNX(_("Found %d focusers, you point number %d"), focuser->Ndevices, num);
        goto retn;
    }
    if(!focuser->setDevNo(num)){
        WARNX(_("Can't set active focuser number"));
        goto retn;
    }
    char buf[BUFSIZ];
    if(focuser->getModelName(buf, BUFSIZ)){
        verbose(2, "Focuser model: %s", buf);
    }
    float t;
    if(focuser->getTbody(&t)){
        verbose(1, "FOCTEMP=%.1f", t);
        DBG("FOCTEMP=%.1f", t);
    }
    float minpos, maxpos, curpos;
    if(!focuser->getMinPos(&minpos) || !focuser->getMaxPos(&maxpos)){
        WARNX(_("Can't get focuser limit positions"));
        goto retn;
    }
    verbose(1, "FOCMINPOS=%g", minpos);
    verbose(1, "FOCMAXPOS=%g", maxpos);
    DBG("FOCMINPOS=%g, FOCMAXPOS=%g", minpos, maxpos);
    if(!focuser->getPos(&curpos)){
        WARNX(_("Can't get current focuser position"));
        goto retn;
    }
    verbose(1, "FOCPOS=%g", curpos);
    DBG("Curpos = %g", curpos);
    if(isnan(GP->gotopos) && isnan(GP->addsteps)) goto retn; // no focuser commands
    float tagpos = 0.;
    if(!isnan(GP->gotopos)){ // set absolute position
        tagpos = GP->gotopos;
    }else{ // relative
        tagpos = curpos + GP->addsteps;
    }
    DBG("tagpos: %g", tagpos);
    if(tagpos < minpos || tagpos > maxpos){
        WARNX(_("Can't set position %g: out of limits [%g, %g]"), tagpos, minpos, maxpos);
        goto retn;
    }
    if(tagpos - minpos < __FLT_EPSILON__){
        if(!focuser->home(GP->async)) WARNX(_("Can't home focuser"));
    }else{
        if(!focuser->setAbsPos(GP->async, tagpos)) WARNX(_("Can't set position %g"), tagpos);
    }
retn:
    focclose();
}

cc_Wheel *startWheel(){
    if(!GP->wheeldev && !GP->commondev){
        verbose(3, _("Wheel device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->wheeldev;
        if(!(wheel = open_wheel(plugin))) return NULL;
    }
    if(!wheel->check()){
        verbose(3, _("No wheels found"));
        wheel = NULL;
        return NULL;
    }
    return wheel;
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
    void *dlh = NULL;
    if(!startWheel(&dlh)) return;
    if(GP->listdevices){
        for(int i = 0; i < wheel->Ndevices; ++i){
            if(!wheel->setDevNo(i)) continue;
            char modname[256];
            wheel->getModelName(modname, 255);
            printf("Found wheel #%d: %s\n", i, modname);
        }
    }
    int num = GP->whldevno;
    if(num < 0) num = 0;
    if(num > wheel->Ndevices - 1){
        WARNX(_("Found %d wheels, you point number %d"), wheel->Ndevices, num);
        goto retn;
    }
    if(!wheel->setDevNo(num)){
        WARNX(_("Can't set active wheel number"));
        goto retn;
    }
    char buf[BUFSIZ];
    if(wheel->getModelName(buf, BUFSIZ)){
        verbose(2, "cc_Wheel model: %s", buf);
    }
    float t;
    if(wheel->getTbody(&t)){
        verbose(1, "WHEELTEMP=%.1f", t);
    }
    int pos, maxpos;
    if(wheel->getPos(&pos)){
        verbose(1, "WHEELPOS=%d", pos);
    }else WARNX("Can't get current wheel position");
    if(!wheel->getMaxPos(&maxpos)){
        WARNX(_("Can't get max wheel position"));
        goto retn;
    }
    verbose(1, "WHEELMAXPOS=%d", maxpos);
    pos = GP->setwheel;
    if(pos == -1) goto retn; // no wheel commands
    if(pos < 0 || pos > maxpos){
        WARNX(_("Wheel position should be from 0 to %d"), maxpos);
        goto retn;
    }
    if(!wheel->setPos(pos))
        WARNX(_("Can't set wheel position %d"), pos);
retn:
    closewheel();
}
/*
static void closeall(){
    if(camera){camera->close(); camera = NULL;}
    if(focuser){focuser->close(); focuser = NULL;}
    if(wheel){wheel->close(); wheel = NULL;}
}*/

static cc_capture_status capt(){
    cc_capture_status cs;
    float tremain, tmpf;
    if(!camera->pollcapture){
        WARNX(_("Camera plugin have no capture polling funtion."));
        return CAPTURE_ABORTED;
    }
    while(camera->pollcapture(&cs, &tremain)){
        if(cs != CAPTURE_PROCESS) break;
        if(tremain > 0.1){
            verbose(2, _("%.1f seconds till exposition ends"), tremain);
            if(camera->getTcold && camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f", tmpf);
            if(camera->getTbody && camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f", tmpf);
        }
        if(tremain > 6.) sleep(5);
        else if(tremain > 0.9) sleep((int)(tremain+0.99));
        else usleep((int)(1e6*tremain) + 100000);
        if(!camera) return CAPTURE_ABORTED;
    }
    DBG("Poll ends with %d", cs);
    return cs;
}

cc_Camera *startCCD(){
    if(!GP->cameradev && !GP->commondev){
        verbose(3, _("Camera device not pointed"));
        return NULL;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->cameradev;
        if(!(camera = open_camera(plugin))) return NULL;
    }
    if(!camera->check()){
        verbose(3, _("No cameras found"));
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
    int rtn = FALSE;
    void *dlh = NULL;
    if(!startCCD(&dlh)) return FALSE;
    if(GP->listdevices){
        if(!camera->getModelName) WARNX(_("Camera plugin have no model name getter"));
        else for(int i = 0; i < camera->Ndevices; ++i){
            if(camera->setDevNo && !camera->setDevNo(i)) continue;
            char modname[256];
            camera->getModelName(modname, 255);
            printf("Found camera #%d: %s\n", i, modname);
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
            DBG("got %s", *p);
            int stop = FALSE;
            while(p && *p){
                cc_charbufclr(b);
                cc_hresult r = camera->plugincmd(*p, b);
                if(r == CC_RESULT_OK || r == CC_RESULT_SILENCE) green("Command '%s'", *p);
                else{
                    stop = TRUE;
                    red("Command '%s'", *p);
                }
                if(r != CC_RESULT_SILENCE) printf(" returns \"%s\"", cc_hresult2str(r));
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
    int x0,x1, y0,y1;
    char buf[BUFSIZ];
    if(camera->getModelName && camera->getModelName(buf, BUFSIZ))
        verbose(2, _("Camera model: %s"), buf);
    verbose(2, _("Pixel size: %g x %g"), camera->pixX, camera->pixY);
    x0 = camera->array.xoff;
    y0 = camera->array.yoff;
    x1 = camera->array.xoff + camera->array.w;
    y1 = camera->array.yoff + camera->array.h;
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", x0, y0, x1, y1);
    verbose(2, _("Full array: %s"), buf);
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", camera->field.xoff, camera->field.yoff,
                camera->field.xoff + camera->field.w, camera->field.yoff + camera->field.h);
    verbose(2, _("Field of view: %s"), buf);
    snprintf(buf, BUFSIZ, "(%d, %d)(%d, %d)", camera->geometry.xoff, camera->geometry.yoff,
                camera->geometry.xoff + camera->geometry.w, camera->geometry.yoff + camera->geometry.h);
    verbose(2, _("Current format: %s"), buf);
    if(!isnan(GP->temperature)){
        if(!camera->setT)WARNX(_("Camera plugin have no temperature setter"));
        else{if(!camera->setT((float)GP->temperature)) WARNX(_("Can't set T to %g degC"), GP->temperature);
            else verbose(3, "SetT=%.1f", GP->temperature);
        }
    }
    float tmpf;
    if(camera->getTcold && camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f", tmpf);
    if(camera->getTbody && camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f", tmpf);
    if(GP->shtr_cmd > -1 && GP->shtr_cmd < SHUTTER_AMOUNT){
        const char *str[] = {"open", "close", "expose @high", "expose @low"};
        verbose(1, _("Shutter command: %s\n"), str[GP->shtr_cmd]);
        if(!camera->shuttercmd || !camera->shuttercmd((cc_shutter_op)GP->shtr_cmd))
            WARNX(_("Can't run shutter command %s (unsupported?)"), str[GP->shtr_cmd]);
    }
    if(GP->confio > -1){
        verbose(1, _("Try to configure I/O port as %d"), GP->confio);
        if(!camera->confio || !camera->confio(GP->confio))
            WARNX(_("Can't configure (unsupported?)"));
    }
    int tmpi;
    if(GP->getio){
        if(camera->getio && camera->getio(&tmpi))
            verbose(0, "CCDIOPORT=0x%02X\n", tmpi);
        else
            WARNX(_("Can't get IOport state (unsupported?)"));
    }
    if(GP->setio > -1){
        verbose(1, _("Try to write %d to I/O port"), GP->setio);
        if(!camera->setio || !camera->setio(GP->setio))
            WARNX(_("Can't set IOport"));
    }
    if(GP->exptime < 0.) goto retn;
    if(!isnan(GP->gain)){
        DBG("Change gain to %g", GP->gain);
        if(camera->setgain && camera->setgain(GP->gain)){
            if(camera->getgain) camera->getgain(&GP->gain);
            verbose(1, _("Set gain to %g"), GP->gain);
        }else WARNX(_("Can't set gain to %g"), GP->gain);
    }
    if(!isnan(GP->brightness)){
        if(camera->setbrightness && camera->setbrightness(GP->brightness)){
            if(camera->getbrightness) camera->getbrightness(&GP->brightness);
            verbose(1, _("Set brightness to %g"), GP->brightness);
        }else WARNX(_("Can't set brightness to %g"), GP->brightness);
    }
    /*********************** expose control ***********************/
    if(GP->hbin < 1) GP->hbin = 1;
    if(GP->vbin < 1) GP->vbin = 1;
    if(!camera->setbin || !camera->setbin(GP->hbin, GP->vbin)){
        WARNX(_("Can't set binning %dx%d"), GP->hbin, GP->vbin);
        if(camera->getbin) camera->getbin(&GP->hbin, &GP->vbin);
    }
    if(GP->X0 < 0) GP->X0 = x0; // default values
    else if(GP->X0 > x1-1) GP->X0 = x1-1;
    if(GP->Y0 < 0) GP->Y0 = y0;
    else if(GP->Y0 > y1-1) GP->Y0 = y1-1;
    if(GP->X1 < GP->X0+1 || GP->X1 > x1) GP->X1 = x1;
    if(GP->Y1 < GP->Y0+1 || GP->Y1 > y1) GP->Y1 = y1;
    DBG("x1/x0: %d/%d", GP->X1, GP->X0);
    cc_frameformat fmt = {.w = GP->X1 - GP->X0, .h = GP->Y1 - GP->Y0, .xoff = GP->X0, .yoff = GP->Y0};
    if(!camera->setgeometry || !camera->setgeometry(&fmt))
        WARNX(_("Can't set given geometry"));
    verbose(3, "Geometry: off=%d/%d, wh=%d/%d", fmt.xoff, fmt.yoff, fmt.w, fmt.h);
    if(GP->nflushes > 0){
        if(!camera->setnflushes || !camera->setnflushes(GP->nflushes))
            WARNX(_("Can't set %d flushes"), GP->nflushes);
        else verbose(3, "Nflushes=%d", GP->nflushes);
    }
    if(!camera->setexp) ERRX(_("Camera plugin have no exposition setter"));
    if(!camera->setexp(GP->exptime))
        WARNX(_("Can't set exposure time to %f seconds"), GP->exptime);
    tmpi = (GP->dark) ? 0 : 1;
    if(!camera->setframetype || !camera->setframetype(tmpi))
        WARNX(_("Can't change frame type"));
    tmpi = (GP->_8bit) ? 0 : 1;
    if(!camera->setbitdepth || !camera->setbitdepth(tmpi))
        WARNX(_("Can't set bit depth"));
    if(!camera->setfastspeed || !camera->setfastspeed(GP->fast))
        WARNX(_("Can't set readout speed"));
    else verbose(1, _("Readout mode: %s"), GP->fast ? "fast" : "normal");
    if(!GP->outfile) verbose(1, _("Only show statistics"));
    // GET binning should be AFTER setgeometry!
    if(!camera->getbin || !camera->getbin(&GP->hbin, &GP->vbin))
        WARNX(_("Can't get current binning"));
    verbose(2, "Binning: %d x %d", GP->hbin, GP->vbin);
    rtn = TRUE;
retn:
    if(!rtn) closecam();
    return rtn;
}

/*
 * Main CCD process in standalone mode without viewer: get N images and save them
 */
void ccds(){
    FNAME();
    cc_frameformat fmt = camera->geometry;
    int raw_width = fmt.w / GP->hbin,  raw_height = fmt.h / GP->vbin;
DBG("w=%d, h=%d", raw_width, raw_height);
    // allocate maximum available memory - for 16bit image
    uint16_t *img = MALLOC(uint16_t, raw_width * raw_height);
    DBG("\n\nAllocated image 2x%dx%d=%d", raw_width, raw_height, 2 * raw_width * raw_height);
    cc_IMG ima = {.data = img, .w = raw_width, .h = raw_height};
    if(GP->nframes < 1) GP->nframes = 1;
    for(int j = 0; j < GP->nframes; ++j){
        TIMESTAMP("Start next cycle");
        TIMEINIT();
        verbose(1, _("Capture frame %d"), j);
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
        verbose(2, _("Read grabbed image"));
        TIMESTAMP("Read grabbed");
        if(!camera->capture) ERRX(_("Camera plugin have no function `capture`"));
        if(!camera->capture(&ima)){
            WARNX(_("Can't grab image"));
            break;
        }
        ima.gotstat = 0;
        TIMESTAMP("Calc stat");
        calculate_stat(&ima);
        TIMESTAMP("Save fits");
        saveFITS(&ima, NULL);
        TIMESTAMP("Ready");
        if(GP->pause_len && j != (GP->nframes - 1)){
            double delta, time1 = sl_dtime() + GP->pause_len;
            while((delta = time1 - sl_dtime()) > 0.){
                verbose(1, _("%d seconds till pause ends\n"), (int)delta);
                float tmpf;
                if(camera->getTcold && camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f\n", tmpf);
                if(camera->getTbody && camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f\n", tmpf);
                if(delta > 6.) sleep(5);
                else if(delta > 0.9) sleep((int)(delta+0.99));
                else usleep((int)(delta*1e6 + 1));
            }
        }
    }
    DBG("FREE img");
    FREE(img);
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
    green("Framerate=%.2f (%g seconds for exp); mean framerate=%.2f\n", 1./dT, dT, NFRM/sumn);
    tlast = t;
}

static volatile int exitgrab = FALSE;
static volatile size_t lastgrabno = 0;
static void *grabnext(void *arg){
    FNAME();
    cc_IMG *ima = (cc_IMG*) arg;
    do{
        if(exitgrab) return NULL;
        TIMESTAMP("Start next exp");
        TIMEINIT();
        if(!ima || !camera) return NULL;
        if(!camera->startexposition) ERRX(_("Camera plugin have no function `start exposition`"));
        if(!camera->startexposition()){
            WARNX(_("Can't start exposition"));
            usleep(10000);
            continue;
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
}



/**
 * @brief ccdcaptured - get new image data for viewer
 * @param img - pointer to cc_IMG* (if cc_IMG* is NULL, will be allocated here)
 * @return TRUE if new image available
 */
int ccdcaptured(cc_IMG **imgptr){
    if(!imgptr) return FALSE;
    //TIMESTAMP("ccdcaptured() start");
    static pthread_t grabthread = 0;
    if(imgptr == (void*)-1){ // kill `grabnext`
        DBG("Wait for grabbing thread ends");
        if(grabthread){
            exitgrab = TRUE;
            //pthread_cancel(grabthread); // this kills some cameras
            //pthread_timedjoin_np
            pthread_join(grabthread, NULL);
            grabthread = 0;
        }
        DBG("OK");
        return FALSE;
    }
    cc_frameformat fmt = camera->geometry;
    int raw_width = fmt.w / GP->hbin,  raw_height = fmt.h / GP->vbin;
    cc_IMG *ima = NULL;
    if(*imgptr && ((*imgptr)->w != raw_width || (*imgptr)->h != raw_height)) FREE(*imgptr);
    if(!*imgptr){
        uint16_t *img = MALLOC(uint16_t, raw_width * raw_height);
        DBG("\n\nAllocated image 2x%dx%d=%d", raw_width, raw_height, 2 * raw_width * raw_height);
        ima = MALLOC(cc_IMG, 1);
        ima->data = img;
        ima->w = raw_width;
        ima->h = raw_height;
        *imgptr = ima;
    }else ima = *imgptr;

    if(!grabthread){ // start new grab
        TIMESTAMP("Start new grab");
        TIMEINIT();
        if(pthread_create(&grabthread, NULL, &grabnext, (void*)ima)){
            WARN("Can't create grabbing thread");
            grabthread = 0;
        }
    }else{ // grab in process
        if(ima->imnumber != lastgrabno){ // done
            /*ssize_t delta = ima->imnumber - lastgrabno;
            if(delta > 0 && delta != 1) WARNX("ccdcaptured(): missed %zd images", delta-1);*/
            lastgrabno = ima->imnumber;
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
    else ERRX("Point network port or UNIX-socket path");
    int sock = cc_open_socket(isserver, path, isnet), imsock = -1;
    if(sock < 0){
        LOGERR("Can't open socket");
        ERRX("start_socket(): can't open socket");
    }
    if(isserver){
        imsock = cc_open_socket(TRUE, GP->imageport, 2); // image socket should be networked
        server(sock, imsock);
    }else{
#ifdef IMAGEVIEW
        if(GP->showimage){
            if(!GP->viewer && GP->exptime < 0.00001) ERRX("Need exposition time!");
            init_grab_sock(sock);
            viewer(sockcaptured); // start viewer with socket client parser
            DBG("done");
        }else
#endif
            client(sock);
    }
    DBG("Close socket");
    close(sock);
    if(isserver){
        close(imsock);
        signals(0);
    }
    return 0;
}
