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

#include <dlfcn.h>  // dlopen/close
#include <fitsio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "ccdfunc.h"
#include "cmdlnopts.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif
#include "omp.h"

Camera *camera = NULL;
Focuser *focuser = NULL;
Wheel *wheel = NULL;

static int fitserror = 0;

#define TRYFITS(f, ...)                     \
do{ int status = 0;                         \
    f(__VA_ARGS__, &status);                \
    if(status){                            \
        fits_report_error(stderr, status);  \
        fitserror = status;}                         \
}while(0)
#define WRITEKEY(...)                           \
do{ int status = 0;                             \
    fits_write_key(__VA_ARGS__, &status);       \
    if(status) fits_report_error(stderr, status);\
}while(0)

#define TMBUFSIZ 40

// find plugin
static void *open_plugin(const char *name){
    DBG("try to open lib %s", name);
    void* dlh = dlopen(name, RTLD_NOLOAD); // library may be already opened
    if(!dlh) dlh = dlopen(name, RTLD_NOW);
    if(!dlh){
        WARNX(_("Can't find plugin %s: %s"), name, dlerror());
        return NULL;
    }
    return dlh;
}

static void *init_focuser(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    focuser = (Focuser*) dlsym(dlh, "focuser");
    if(!focuser){
        WARNX(_("Can't find focuser in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return dlh;
}
static void *init_camera(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    camera = (Camera*) dlsym(dlh, "camera");
    if(!camera){
        WARNX(_("Can't find camera in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return dlh;
}
static void *init_wheel(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    wheel = (Wheel*) dlsym(dlh, "wheel");
    if(!wheel){
        WARNX(_("Can't find wheel in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return dlh;
}

/*
static size_t curtime(char *s_time){ // current date/time
    time_t tm = time(NULL);
    return strftime(s_time, TMBUFSIZ, "%d/%m/%Y,%H:%M:%S", localtime(&tm));
}*/

static int check_filename(char *buff, char *outfile, char *ext){
    struct stat filestat;
    int num;
    for(num = 1; num < 10000; num++){
        if(snprintf(buff, PATH_MAX, "%s_%04d.%s", outfile, num, ext) < 1)
            return 0;
        if(stat(buff, &filestat)) // no such file or can't stat()
            return 1;
    }
    return 0;
}

/**
 * @brief addrec - add FITS records from file
 * @param f (i)        - FITS filename
 * @param filename (i) - name of file
 */
static void addrec(fitsfile *f, char *filename){
    FILE *fp = fopen(filename, "r");
    if(!fp) return;
    char buf[2*FLEN_CARD];
    while(fgets(buf, 2*FLEN_CARD, fp)){
        //DBG("check record _%s_", buf);
        int keytype, status = 0;
        char newcard[FLEN_CARD], keyname[FLEN_CARD];
        fits_parse_template(buf, newcard, &keytype, &status);
        if(status){
            fits_report_error(stderr, status);
            continue;
        }
        //DBG("reformatted to _%s_", newcard);
        strncpy(keyname, newcard, FLEN_CARD);
        char *eq = strchr(keyname, '='); if(eq) *eq = 0;
        eq = strchr(keyname, ' '); if(eq) *eq = 0;
        //DBG("keyname: %s", keyname);
        fits_update_card(f, keyname, newcard, &status);
    }
}

void saveFITS(IMG *img, char *filename){
    if(!camera){
        WARNX(_("Camera device unknown"));
        return;
    }
    char buff[PATH_MAX], fnam[PATH_MAX];
    if(filename == NULL) return;
    fitserror = 0;
    if(!check_filename(fnam, filename, "fits") && !GP->rewrite){
        // Не могу сохранить файл
        WARNX(_("Can't save file"));
    }else{
        if(GP->rewrite){
            DBG("REW");
            snprintf(fnam, PATH_MAX, "!%s.fits", filename);
        }
    }
    int width = img->w, height = img->h;
    void *data = (void*) img->data;
    long naxes[2] = {width, height}, tmpl;
    double tmpd = 0.0;
    float tmpf = 0.0;
    int tmpi = 0;
    struct tm *tm_time;
    char bufc[FLEN_CARD];
    time_t savetime = time(NULL);
    fitsfile *fp;
    TRYFITS(fits_create_file, &fp, fnam);
    if(fitserror) goto cloerr;
    TRYFITS(fits_create_img, fp, USHORT_IMG, 2, naxes);
    if(fitserror) goto cloerr;
    // FILE / Input file original name
    WRITEKEY(fp, TSTRING, "FILE", filename, "Input file original name");
    // ORIGIN / organization responsible for the data
    WRITEKEY(fp, TSTRING, "ORIGIN", "SAO RAS", "organization responsible for the data");
    // OBSERVAT / Observatory name
    WRITEKEY(fp, TSTRING, "OBSERVAT", "Special Astrophysical Observatory, Russia", "Observatory name");
    // DETECTOR / detector
    if(camera->getModelName(buff, PATH_MAX)){
        WRITEKEY(fp, TSTRING, "DETECTOR", buff, "Detector model");
    }
    // INSTRUME / Instrument
    if(GP->instrument){
        WRITEKEY(fp, TSTRING, "INSTRUME", GP->instrument, "Instrument");
    }else
        WRITEKEY(fp, TSTRING, "INSTRUME", "direct imaging", "Instrument");
    snprintf(bufc, FLEN_VALUE, "%g x %g", camera->pixX, camera->pixY);
    // PXSIZE / pixel size
    WRITEKEY(fp, TSTRING, "PXSIZE", bufc, "Pixel size in m");
    snprintf(bufc, FLEN_VALUE, "(%d, %d)(%d, %d)", camera->field.xoff, camera->field.yoff,
             camera->field.xoff + camera->field.w, camera->field.yoff + camera->field.h);
    WRITEKEY(fp, TSTRING, "VIEWFLD", bufc, "Camera field of view");
    snprintf(bufc, FLEN_VALUE, "(%d, %d)(%d, %d)", camera->array.xoff, camera->array.yoff,
             camera->array.xoff + camera->array.w, camera->array.yoff + camera->array.h);
    WRITEKEY(fp, TSTRING, "ARRAYFLD", bufc, "Camera full array size");
    // CRVAL1, CRVAL2 / Offset in X, Y
    if(GP->X0 > -1) WRITEKEY(fp, TINT, "X0", &GP->X0, "Subframe left border");
    if(GP->Y0 > -1) WRITEKEY(fp, TINT, "Y0", &GP->Y0, "Subframe upper border");
    if(GP->objtype) strncpy(bufc, GP->objtype, FLEN_CARD-1);
    else if(GP->dark) sprintf(bufc, "dark");
    else sprintf(bufc, "light");
    // IMAGETYP / object, flat, dark, bias, scan, eta, neon, push
    WRITEKEY(fp, TSTRING, "IMAGETYP", bufc, "Image type");
    // DATAMAX, DATAMIN / Max, min pixel value
    tmpi = 0;
    WRITEKEY(fp, TINT, "DATAMIN", &tmpi, "Min pixel value");
    //itmp = GP->fast ? 255 : 65535;
    tmpi = 65535;
    WRITEKEY(fp, TINT, "DATAMAX", &tmpi, "Max pixel value");
    tmpi = img->min;
    WRITEKEY(fp, TUSHORT, "STATMIN", &tmpi, "Min data value");
    tmpi = img->max;
    WRITEKEY(fp, TUSHORT, "STATMAX", &tmpi, "Max data value");
    tmpf = img->avr;
    WRITEKEY(fp, TFLOAT, "STATAVR", &tmpf, "Average data value");
    tmpf = img->std;
    WRITEKEY(fp, TFLOAT, "STATSTD", &tmpf, "Std. of data value");
    WRITEKEY(fp, TFLOAT, "CAMTEMP0", &GP->temperature, "Camera temperature at exp. start, degr C");
    if(camera->getTcold(&tmpf))
        WRITEKEY(fp, TFLOAT, "CAMTEMP", &tmpf, "Camera temperature at exp. end, degr C");
    if(camera->getTbody(&tmpf))
        WRITEKEY(fp, TFLOAT, "BODYTEMP", &tmpf, "Camera body temperature at exp. end, degr C");
    if(camera->getThot(&tmpf))
        WRITEKEY(fp, TFLOAT, "HOTTEMP", &tmpf, "Camera peltier hot side temperature at exp. end, degr C");
    // EXPTIME / actual exposition time (sec)
    tmpd = GP->exptime;
    WRITEKEY(fp, TDOUBLE, "EXPTIME", &tmpd, "Actual exposition time (sec)");
    // DATE / Creation date (YYYY-MM-DDThh:mm:ss, UTC)
    strftime(bufc, FLEN_VALUE, "%Y-%m-%dT%H:%M:%S", gmtime(&savetime));
    WRITEKEY(fp, TSTRING, "DATE", bufc, "Creation date (YYYY-MM-DDThh:mm:ss, UTC)");
    tmpl = (long) savetime;
    tm_time = localtime(&savetime);
    strftime(bufc, FLEN_VALUE, "File creation time (UNIX)", tm_time);
    WRITEKEY(fp, TLONG, "UNIXTIME", &tmpl, bufc);
    strftime(bufc, 80, "%Y/%m/%d", tm_time);
    // DATE-OBS / DATE (YYYY/MM/DD) OF OBS.
    WRITEKEY(fp, TSTRING, "DATE-OBS", bufc, "DATE OF OBS. (YYYY/MM/DD, local)");
    strftime(bufc, 80, "%H:%M:%S", tm_time);
    WRITEKEY(fp, TSTRING, "TIME", bufc, "Creation time (hh:mm:ss, local)");
    // OBJECT  / Object name
    if(GP->objname){
        WRITEKEY(fp, TSTRING, "OBJECT", GP->objname, "Object name");
    }
    // BINNING / Binning
    if(GP->hbin != 1 || GP->vbin != 1){
        snprintf(bufc, 80, "%d x %d", GP->hbin, GP->vbin);
        WRITEKEY(fp, TSTRING, "BINNING", bufc, "Binning (hbin x vbin)");
        tmpi = GP->hbin;
        WRITEKEY(fp, TINT, "XBINNING", &tmpi, "binning factor used on X axis");
        tmpi = GP->vbin;
        WRITEKEY(fp, TINT, "YBINNING", &tmpi, "binning factor used on Y axis");
    }
    // OBSERVER / Observers
    if(GP->observers){
        WRITEKEY(fp, TSTRING, "OBSERVER", GP->observers, "Observers");
    }
    // PROG-ID / Observation program identifier
    if(GP->prog_id){
        WRITEKEY(fp, TSTRING, "PROG-ID", GP->prog_id, "Observation program identifier");
    }
    // AUTHOR / Author of the program
    if(GP->author){
        WRITEKEY(fp, TSTRING, "AUTHOR", GP->author, "Author of the program");
    }
    if(focuser){ // there is a focuser device - add info
        if(focuser->getModelName(buff, PATH_MAX))
            WRITEKEY(fp, TSTRING, "FOCUSER", buff, "Focuser model");
        if(focuser->getPos(&tmpf))
            WRITEKEY(fp, TFLOAT, "FOCUS", &tmpf, "Current focuser position, mm");
        if(focuser->getMinPos(&tmpf))
            WRITEKEY(fp, TFLOAT, "FOCMIN", &tmpf, "Minimal focuser position, mm");
        if(focuser->getMaxPos(&tmpf))
            WRITEKEY(fp, TFLOAT, "FOCMAX", &tmpf, "Maximal focuser position, mm");
        if(focuser->getTbody(&tmpf))
            WRITEKEY(fp, TFLOAT, "FOCTEMP", &tmpf, "Focuser body temperature, degr C");
    }
    if(wheel){ // there is a filter wheel device - add info
        if(wheel->getModelName(buff, PATH_MAX))
            WRITEKEY(fp, TSTRING, "WHEEL", buff, "Filter wheel model");
        if(wheel->getPos(&tmpi))
            WRITEKEY(fp, TINT, "FILTER", &tmpi, "Current filter number");
        if(wheel->getMaxPos(&tmpi))
            WRITEKEY(fp, TINT, "FILTMAX", &tmpi, "Amount of filter positions");
        if(wheel->getTbody(&tmpf))
            WRITEKEY(fp, TFLOAT, "FILTTEMP", &tmpf, "Filter wheel body temperature, degr C");
    }
    if(GP->addhdr){ // add records from files
        char **nxtfile = GP->addhdr;
        while(*nxtfile){
            addrec(fp, *nxtfile++);
        }
    }
    TRYFITS(fits_write_img, fp, TUSHORT, 1, width * height, data);
    if(fitserror) goto cloerr;
    TRYFITS(fits_close_file, fp);
cloerr:
    if(fitserror == 0){
        verbose(1, _("File saved as '%s'"), fnam);
    }else{
        WARNX(_("Error saving file"));
        fitserror = 0;
    }
}

static void calculate_stat(IMG *image){
    uint64_t Noverld = 0L, size = image->h*image->w;
    double sum = 0., sum2 = 0.;
    uint16_t max = 0, min = 65535;
#pragma omp parallel
{
    uint16_t maxpriv = 0, minpriv = 65535;
    uint64_t ovrpriv = 0;
    double sumpriv = 0., sum2priv = 0.;
    #pragma omp for nowait
    for(uint64_t i = 0; i < size; ++i){
        uint16_t val = image->data[i];
        float pv = (float) val;
        sum += pv;
        sum2 += (pv * pv);
        if(max < val) max = val;
        if(min > val) min = val;
        if(val >= 65530) ovrpriv++;
    }
    #pragma omp critical
    {
        if(max < maxpriv) max = maxpriv;
        if(min > minpriv) min = minpriv;
        sum += sumpriv;
        sum2 += sum2priv;
        Noverld += ovrpriv;
    }
}
    double sz = (float)size;
    double avr = sum/sz;
    image->avr = avr;
    image->std = sqrt(fabs(sum2/sz - avr*avr));
    if(GP->verbose){
        printf(_("Image stat:\n"));
        printf("avr = %.1f, std = %.1f, Noverload = %ld\n", avr, image->std, Noverld);
        printf("max = %u, min = %u, size = %ld\n", max, min, size);
    }
}

/*
 * Find focusers and work with each of them
 */
void focusers(){
    FNAME();
    void *dlh = NULL;
    if(!GP->focuserdev && !GP->commondev){
        verbose(3, _("Focuser device not pointed"));
        return;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->focuserdev;
        if(!(dlh = init_focuser(plugin))) return;
    }
    if(!focuser->check()){
        verbose(3, _("No focusers found"));
        focuser = NULL;
        return;
    }
    if(GP->listdevices){
        for(int i = 0; i < focuser->Ndevices; ++i){
            char modname[256];
            focuser->getModelName(modname, 255);
            printf("Found focuser #%d: %s\n", i, modname);
        }
    }
    int num = GP->focdevno;
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
    focuser->close();
    dlclose(dlh);
    focuser = NULL;
}

/*
 * Find wheels and work with each of them
 */
void wheels(){
    FNAME();
    void *dlh = NULL;
    if(!GP->wheeldev && !GP->commondev){
        verbose(3, _("Wheel device not pointed"));
        return;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->wheeldev;
        if(!(dlh = init_wheel(plugin))) return;
    }
    if(!wheel->check()){
        verbose(3, _("No wheels found"));
        wheel = NULL;
        return;
    }
    if(GP->listdevices){
        for(int i = 0; i < wheel->Ndevices; ++i){
            char modname[256];
            wheel->getModelName(modname, 255);
            printf("Found wheel #%d: %s\n", i, modname);
        }
    }
    int num = GP->whldevno;
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
        verbose(2, "Wheel model: %s", buf);
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
    wheel->close();
    dlclose(dlh);
    wheel = NULL;
}
/*
static void closeall(){
    if(camera){camera->close(); camera = NULL;}
    if(focuser){focuser->close(); focuser = NULL;}
    if(wheel){wheel->close(); wheel = NULL;}
}*/

static capture_status capt(){
    capture_status cs;
    float tleave, tmpf;
    while(camera->pollcapture(&cs, &tleave)){
        if(cs != CAPTURE_PROCESS) break;
        if(tleave > 0.1){
            verbose(2, _("%.1f seconds till exposition ends"), tleave);
            if(camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f", tmpf);
            if(camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f", tmpf);
        }
        if(tleave > 6.) sleep(5);
        else if(tleave > 0.9) sleep((int)(tleave+0.99));
        else usleep((int)(1e6*tleave) + 100000);
        if(!camera) return CAPTURE_ABORTED;
    }
    return cs;
}

/*
 * Find CCDs and work with each of them
 */
void ccds(){
    FNAME();
    float tmpf;
    int tmpi;
    void *dlh = NULL;
    if(!GP->cameradev && !GP->commondev){
        verbose(3, _("Camera device not pointed"));
        return;
    }else{
        char *plugin = GP->commondev ? GP->commondev : GP->cameradev;
        if(!(dlh = init_camera(plugin))) return;
    }
    if(!camera->check()){
        verbose(3, _("No cameras found"));
        return;
    }
    if(GP->listdevices){
        for(int i = 0; i < camera->Ndevices; ++i){
            char modname[256];
            camera->getModelName(modname, 255);
            printf("Found camera #%d: %s\n", i, modname);
        }
    }
    int num = GP->camdevno;
    if(num > camera->Ndevices - 1){
        WARNX(_("Found %d cameras, you point number %d"), camera->Ndevices, num);
        goto retn;
    }
    if(!camera->setDevNo(num)){
        WARNX(_("Can't set active camera number"));
        goto retn;
    }
    if(GP->fanspeed > -1){
        if(GP->fanspeed > FAN_HIGH) GP->fanspeed = FAN_HIGH;
        if(!camera->setfanspeed((fan_speed)GP->fanspeed))
            WARNX(_("Can't set fan speed"));
        else verbose(0, _("Set fan speed to %d"), GP->fanspeed);
    }
    int x0,x1, y0,y1;
    char buf[BUFSIZ];
    if(camera->getModelName(buf, BUFSIZ))
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
    if(GP->temperature < 40.){
        if(!camera->setT((float)GP->temperature))
            WARNX(_("Can't set T to %g degC"), GP->temperature);
        verbose(3, "SetT=%.1f", GP->temperature);
    }
    if(camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f", tmpf);
    if(camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f", tmpf);
    if(GP->shtr_cmd > -1 && GP->shtr_cmd < SHUTTER_AMOUNT){
        const char *str[] = {"open", "close", "expose @high", "expose @low"};
        verbose(1, _("Shutter command: %s\n"), str[GP->shtr_cmd]);
        if(!camera->shuttercmd((shutter_op)GP->shtr_cmd))
            WARNX(_("Can't run shutter command %s (unsupported?)"), str[GP->shtr_cmd]);
    }
    if(GP->confio > -1){
        // "Попытка сконфигурировать порт I/O как %d\n"
        verbose(1, _("Try to configure I/O port as %d"), GP->confio);
        if(!camera->confio(GP->confio))
            WARNX(_("Can't configure (unsupported?)"));
    }
    if(GP->getio){
        if(camera->getio(&tmpi))
            verbose(0, "CCDIOPORT=0x%02X\n", tmpi);
        else
            WARNX(_("Can't get IOport state (unsupported?)"));
    }
    if(GP->setio > -1){
        // "Попытка записи %d в порт I/O\n"
        verbose(1, _("Try to write %d to I/O port"), GP->setio);
        if(!camera->setio(GP->setio))
            WARNX(_("Can't set IOport"));
    }
    if(GP->exptime < 0.) goto retn;
    /*********************** expose control ***********************/
    // cancel previous exp
    camera->cancel();
    if(!camera->setbin(GP->hbin, GP->vbin))
        WARNX(_("Can't set binning %dx%d"), GP->hbin, GP->vbin);
    if(GP->fullframe){
        DBG("FULLFRAME");
        GP->X0 = x0; GP->Y0 = y0; GP->X1 = x1; GP->Y1 = y1;
    }
    if(GP->X0 == -1) GP->X0 = x0; // default values
    if(GP->Y0 == -1) GP->Y0 = y0;
    if(GP->X1 == -1) GP->X1 = x1;
    else if(GP->X1 > x1) GP->X1 = x1;
    if(GP->Y1 == -1) GP->Y1 = y1;
    else if(GP->Y1 > y1) GP->Y1 = y1;
    frameformat fmt = {.w = GP->X1 - GP->X0, .h = GP->Y1 - GP->Y0, .xoff = GP->X0, .yoff = GP->Y0};
    int raw_width = fmt.w / GP->hbin,  raw_height = fmt.h / GP->vbin;
    if(!camera->setgeometry(&fmt))
        WARNX(_("Can't set given geometry"));
    verbose(3, "Geometry: off=%d/%d, wh=%d/%d", fmt.xoff, fmt.yoff, fmt.w, fmt.h);
    if(!camera->setnflushes(GP->nflushes))
        WARNX(_("Can't set %d flushes"), GP->nflushes);
    verbose(3, "Nflushes=%d", GP->nflushes);
    if(!camera->setexp(GP->exptime))
        WARNX(_("Can't set exposure time to %f seconds"), GP->exptime);
    tmpi = (GP->dark) ? 0 : 1;
    if(!camera->setframetype(tmpi))
        WARNX(_("Can't change frame type"));
    tmpi = (GP->_8bit) ? 0 : 1;
    if(!camera->setbitdepth(tmpi))
        WARNX(_("Can't set bit depth"));
    if(!camera->setfastspeed(GP->fast))
        WARNX(_("Can't set readout speed"));
    else verbose(1, _("Readout mode: %s"), GP->fast ? "fast" : "normal");
    if(!GP->outfile) verbose(1, _("Only show statistics"));
    if(!camera->getbin(&GP->hbin, &GP->vbin)) // GET binning should be AFTER setgeometry!
        WARNX(_("Can't get current binning"));
    verbose(2, "Binning: %d x %d", GP->hbin, GP->vbin);


    uint16_t *img = MALLOC(uint16_t, raw_width * raw_height);
    IMG ima = {.data = img, .w = raw_width, .h = raw_height};
#ifdef IMAGEVIEW
    windowData *mainwin = NULL;
    if(GP->showimage){
        imageview_init();
        DBG("Create new win");
        mainwin = createGLwin("Sample window", raw_width, raw_height, NULL);
        if(!mainwin){
            WARNX(_("Can't open OpenGL window, image preview will be inaccessible"));
        }else
            pthread_create(&mainwin->thread, NULL, &image_thread, (void*)&ima);
    }
#endif
    for(int j = 0; j < GP->nframes; ++j){
        // Захват кадра %d\n
        verbose(1, _("Capture frame %d"), j);
        if(!camera->startexposition()){
            WARNX(_("Can't start exposition"));
            break;
        }
        if(capt() != CAPTURE_READY){
            WARNX(_("Can't capture image"));
            break;
        }
        verbose(2, _("Read grabbed image"));
        //if(!camera) return;
        if(!camera->capture(&ima)){
            WARNX(_("Can't grab image"));
            break;
        }
        calculate_stat(&ima);
        saveFITS(&ima, GP->outfile);
#ifdef IMAGEVIEW
        if(GP->showimage){ // display image
            if((mainwin = getWin())){
                DBG("change image");
                change_displayed_image(mainwin, &ima);
                while((mainwin = getWin())){ // test paused state & grabbing custom frames
                    if((mainwin->winevt & WINEVT_PAUSE) == 0) break;
                    if(mainwin->winevt & WINEVT_GETIMAGE){
                        mainwin->winevt &= ~WINEVT_GETIMAGE;
                        //if(!camera) return;
                        if(capt() != CAPTURE_READY){
                            WARNX(_("Can't capture image"));
                        }else{
                            //if(!camera) return;
                            if(!camera->capture(&ima)){
                                WARNX(_("Can't grab image"));
                            }
                            else{
                                calculate_stat(&ima);
                                change_displayed_image(mainwin, &ima);
                            }
                        }
                    }
                    usleep(10000);
                }
            }else break; // stop capturing when window closed
        }
#endif
        if(GP->pause_len && j != (GP->nframes - 1)){
            double delta, time1 = dtime() + GP->pause_len;
            while((delta = time1 - dtime()) > 0.){
                // %d секунд до окончания паузы\n
                verbose(1, _("%d seconds till pause ends\n"), (int)delta);
                if(camera->getTcold(&tmpf)) verbose(1, "CCDTEMP=%.1f\n", tmpf);
                if(camera->getTbody(&tmpf)) verbose(1, "BODYTEMP=%.1f\n", tmpf);
                if(delta > 6.) sleep(5);
                else if(delta > 0.9) sleep((int)(delta+0.99));
                else usleep((int)(delta*1e6 + 1));
            }
        }
    }
#ifdef IMAGEVIEW
    if(GP->showimage){
        if((mainwin = getWin())) mainwin->winevt |= WINEVT_PAUSE;
        DBG("Waiting");
        while((mainwin = getWin())){
            //if(mainwin->killthread) break;
            if(mainwin->winevt & WINEVT_GETIMAGE){
                DBG("GRAB");
                mainwin->winevt &= ~WINEVT_GETIMAGE;
                //if(!camera) return;
                if(capt() != CAPTURE_READY){
                    WARNX(_("Can't capture image"));
                }else{
                    //if(!camera) return;
                    if(!camera->capture(&ima)){
                        WARNX(_("Can't grab image"));
                    }
                    else{
                        calculate_stat(&ima);
                        change_displayed_image(mainwin, &ima);
                    }
                }
            }
        }
        DBG("Close window");
        usleep(10000);
    }
#endif
    DBG("FREE img");
    FREE(img);
retn:
    DBG("Close cam");
    camera->close();
    DBG("close dlh");
    dlclose(dlh);
    camera = NULL;
}

void cancel(){
    if(camera){
        camera->cancel();
    }
}
