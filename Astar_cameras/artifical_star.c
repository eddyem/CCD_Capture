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

#include <fcntl.h>
#include <improclib.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "ccdcapture.h"
#include "omp.h"

extern cc_Camera camera;
extern cc_Focuser focuser;
extern cc_Wheel wheel;

// array size
#define ARRAYH  (1050)
#define ARRAYW  (1050)
// amount of stars
#define MAX_STARS   (32)
#ifndef _STR
#define _STR(x)  #x
#endif
#ifndef STR
#define STR(x) _STR(x)
#endif

static const int filtermax = 5;
static const float focmaxpos = 10.;
static int filterpos = 0;
static float focuserpos = 1., brightness = 1., gain = 0.;
static float camtemp = -30., exptime = 0.1;
static cc_capture_status capstat = CAPTURE_NO;
static double texpstart = 0.;
static uint8_t bitpix = 16; // bit depth: 8 or 16
static il_Image *imagemask = NULL, *imagebg; // mask & background
static char *maskfilename = NULL, *bgfilename = NULL; // filenames

typedef struct{
    int Nstars; // amount of stars (1..MAX_STARS)
    int curstarno; // current star number
    int x0, y0; // center of field in array coordinates
    double rotan0; // starting rotation angle
    double xs[MAX_STARS], ys[MAX_STARS]; // center of star field in array coordinates
    double fwhm; // stars min FWHM, arcsec
    double beta; // Moffat `beta` parameter
    //double theta; // Moffat `theta`, arcsec
    double scale; // CCD scale: arcsec/pix
    double mag[MAX_STARS]; // star magnitude: 0m is 0xffff/0xff ADUs per second
    double vX; // X axe drift speed (arcsec/s)
    double vY; // Y -//-
    double vR; // rotation speed (arcsec/s)
    double fluct; // stars position fluctuations (arcsec/sec)
    double noiselambda; // poisson noice lambda value per second
    double darklambda; // poisson noice lambda value for dark noise
} settings_t;

static settings_t settings = {
    .Nstars = 1,
    .x0 = 512, .y0 = 512,
    .fwhm = 1.5, .beta = 1., .scale = 0.03,
    .fluct = 0.3, .noiselambda = 1.1, .darklambda = 1.
};
// min/max for parameters
static int nummin = 1, nummax = MAX_STARS, nomin = 0, nomax = MAX_STARS - 1;
static const double fwhmmin = 0.1, fwhmmax = 10., scalemin = 0.001, scalemax = 3600., magmin = -30., magmax = 30.;
static const double vmin = -20., vmax = 20., fluctmin = 0., fluctmax = 3., betamin = 0.5;
static const double vrotmin = -36000., vrotmax = 36000.; // limit rotator speed to 10 degrees per second
static const double rotanmin = 0., rotanmax = 1295999; // 0..360degr-1''
static double dX = 0., dY = 0.; // current "sky" coordinates (arcsec) relative to field center (according vdrift)
static double rotangle = 0., sinr = 0., cosr = 1.; // current rotation angle (arcsec) around x0/y0 and its sin/cos
static int Xc = 0, Yc = 0; // current pixel coordinates of "sky" center (due to current image size, clip and scale) + fluctuations
static double Tstart = -1.; // global acquisition start
static double Xfluct = 0., Yfluct = 0.; // fluctuation additions in arcsec
static il_Image *star = NULL; // template of star 0m
static double FWHM0 = 0., scale0 = 0., BETA0 = 0.; // template fwhm/scale
static int templ_wh = 0; // template width/height in pixels
static double lambdamin = 1.;

/**
 * @brief test_template - test star template and recalculate new if need
 */
static void test_template(){
    if(star && FWHM0 == settings.fwhm && scale0 == settings.scale && BETA0 == settings.beta) return;
    templ_wh = (1 + 6*settings.fwhm/settings.scale); // +1 for center
    FWHM0 = settings.fwhm;
    scale0 = settings.scale;
    BETA0 = settings.beta;
    il_Image_free(&star);
DBG("MAKE STAR, wh=%d, beta=%g", templ_wh, settings.beta);
    star = il_Image_star(IMTYPE_D, templ_wh, templ_wh, settings.fwhm, settings.beta);
    if(!star) ERRX(_("Can't generate star template"));
    //il_Image_minmax(star);
    //DBG("STAR: %dx%d, max=%g, min=%g, %d bytes per pix, type %d; templ_wh=%d", star->height, star->width, star->maxval, star->minval, star->pixbytes, star->type, templ_wh);
    double sum = 0., *ptr = (double*)star->data;
    int l = templ_wh * templ_wh;
    for(int i = 0; i < l; ++i) sum += ptr[i];
    //green("sum=%g\n", sum);
    OMP_FOR()
    for(int i = 0; i < l; ++i) ptr[i] /= sum;
}

/*
 * About star[s] model
 * Star magnitude 0m is approximately full signal level over square second.
 * Star image generates in circle with raduis 3FWHM'' (3FWHM/scale pix), so amplitude
 *  of star (max value) will be calculated as
 *   0xffff (0xff for 8bit image) / sum(I1(x,y)), where I1 is generated star image with amplitude 1.
 * Flux for magnutude `mag`: Im = I0 * 10^(-0.4mag)
 * 1. Generate `star` with ampl=1 in radius 3FWHM/scale pixels for image in `double`.
 * 2. Calculate amplitude I = 1/sum(I1(x,y)).
 * 3. Multiply all template values by I.
 * 4. Fill every `star` from this template with factor 0xffff[0xff]*10^(-0.4mag).
 * 5. Add background and noice.
 */

#define GEN(ima, type, maxval) \
    register int w = ima->w;\
    int h = ima->h, tw2 = templ_wh/2, X0,Y0, X1,Y1, x0,y0;\
    for(int N = 0; N < settings.Nstars; ++N){\
        int Xstar = Xc + (settings.xs[N]*cosr - settings.ys[N]*sinr)/settings.scale; \
        int Ystar = Yc + (settings.ys[N]*cosr + settings.xs[N]*sinr)/settings.scale;\
        fprintf(stderr, "Xstar=%d, Ystar=%d\t", Xstar, Ystar);\
        if(Xstar - tw2 < 0){\
            X0 = tw2 - Xstar;\
            x0 = 0;\
        }else{\
            X0 = 0; x0 = Xstar - tw2;\
        }\
        if(Ystar - tw2 < 0){\
            Y0 = tw2 - Ystar;\
            y0 = 0;\
        }else{\
            Y0 = 0; y0 = Ystar - tw2;\
        }\
        if(Xstar + tw2 > w-1){\
     /* templ_wh - (Xc + tw2 - (w - 1))*/ \
            X1 = templ_wh - Xstar - tw2 + w - 1; \
        }else X1 = templ_wh - 1;\
        if(Ystar + tw2 > h-1){\
            Y1 = templ_wh - Ystar - tw2 + h - 1;\
        }else Y1 = templ_wh - 1;\
        double mul = 100. * exptime * maxval * pow(10, -0.4*settings.mag[N]); /* multiplier due to "star" magnitude */ \
        /* check if the 'star' out of frame */ \
        fprintf(stderr, "X0=%d, X1=%d, Y0=%d, Y1=%d, x0=%d, y0=%d, mul=%g\n", X0,X1,Y0,Y1,x0,y0, mul);\
        if(X0 < 0 || X0 > templ_wh - 1 || Y0 < 0 || Y0 > templ_wh - 1) continue;\
        if(x0 < 0 || x0 > w-1 || y0 < 0 || y0 > h-1) continue;\
        if(X1 < 0 || X1 > templ_wh || Y1 < 0 || Y1 > templ_wh) continue;\
        if(X0 > X1 || Y0 > Y1) return;\
        OMP_FOR()\
        for(int y = Y0; y < Y1; ++y){\
            type *out = &((type*)ima->data)[(y-Y0+y0)*w + x0];\
            double *in = &((double*)star->data)[y*templ_wh + X0];\
            for(int x = X0; x < X1; ++x, ++in, ++out){\
                double val = *out + *in * mul;\
                *out = (val > maxval) ? maxval : (type)val;\
            }\
        }\
    }\
    if(imagebg){ /* add background */ \
        X0 = camera.geometry.xoff; Y0 = camera.geometry.yoff; \
        X1 = imagebg->width; Y1 = imagebg->height; \
        if(X1-X0 > w) X1 = X0 + w;\
        if(Y1-Y0 > h) Y1 = Y0 + h; \
        OMP_FOR()\
        for(int y = Y0; y < Y1; ++y){\
            type *out = &((type*)ima->data)[(y-Y0)*w];\
            uint8_t *in = &((uint8_t*)imagebg->data)[y*imagemask->width + X0];\
            for(int x = X0; x < X1; ++x, ++in, ++out){\
                *out = (*out + *in > maxval) ? maxval : *out + *in;\
            }\
        } \
    }\
    if(imagemask){ /* apply mask */ \
        X0 = camera.geometry.xoff; Y0 = camera.geometry.yoff; \
        X1 = imagemask->width; Y1 = imagemask->height; \
        if(X1-X0 > w) X1 = X0 + w;\
        if(Y1-Y0 > h) Y1 = Y0 + h; \
        OMP_FOR()\
        for(int y = Y0; y < Y1; ++y){\
            type *out = &((type*)ima->data)[(y-Y0)*w];\
            uint8_t *in = &((uint8_t*)imagemask->data)[y*imagemask->width + X0];\
            for(int x = X0; x < X1; ++x, ++in, ++out){\
                if(*in == 0) *out = 0;\
            }\
        } \
    }\
    if(settings.noiselambda > 1. || settings.darklambda > 1.){ /* apply noise */ \
        w *= h; \
        type *out = (type*)ima->data;\
        OMP_FOR()\
        for(int i = 0; i < w; ++i){\
            type p = il_Poisson((settings.noiselambda - 1.)*exptime + settings.darklambda); \
            /* type p = rand() % 5; */ \
            out[i] = (out[i] + p > maxval) ? maxval : out[i] + p; \
        }\
    }\


static void gen16(cc_IMG *ima){
    GEN(ima, uint16_t, 0xffff);
}

static void gen8(cc_IMG *ima){
    GEN(ima, uint8_t, 0xff);
}

static int campoll(cc_capture_status *st, float *remain){
    if(capstat != CAPTURE_PROCESS){
        if(st) *st = capstat;
        if(remain) *remain = 0.;
        return TRUE;
    }
    if(sl_dtime() - texpstart > exptime){
        if(st) *st = CAPTURE_READY;
        if(remain) *remain = 0.;
        capstat = CAPTURE_NO;
        return TRUE;
    }
    if(st) *st = capstat;
    if(remain) *remain = exptime + texpstart - sl_dtime();
    return TRUE;
}

static int startexp(){
    if(capstat == CAPTURE_PROCESS) return FALSE;
    capstat = CAPTURE_PROCESS;
    double Tnow = sl_dtime(), dT = Tnow - texpstart, Xcd, Ycd;
    if(dT < 0.) dT = 0.;
    else if(dT > 1.) dT = 1.; // dT for fluctuations amplitude
    if(Tstart < 0.) Tstart = Tnow;
    texpstart = Tnow;
    double Tfromstart = Tnow - Tstart;
    // recalculate center of field coordinates at moment of exp start
    dX = Tfromstart * settings.vX;
    dY = Tfromstart * settings.vY;
    rotangle = settings.rotan0 + Tfromstart * settings.vR;
    if(rotangle < rotanmin) rotangle += 360.*3600.;
    else if(rotangle > rotanmax) rotangle -= 360.*3600.;
    sincos(rotangle * M_PI/3600./180., &sinr, &cosr);
    double xx = dX/settings.scale, yy = dY/settings.scale;
    Xcd = xx*cosr - yy*sinr + settings.x0 - camera.array.xoff - camera.geometry.xoff;
    Ycd = yy*cosr + xx*sinr + settings.y0 - camera.array.yoff - camera.geometry.yoff;
    DBG("dX=%g, dY=%g; Xc=%g, Yc=%g", dX, dY, Xcd, Ycd);
    // add fluctuations
    double fx = settings.fluct * dT * (2.*drand48() - 1.); // [-fluct*dT, +fluct*dT]
    double fy = settings.fluct * dT * (2.*drand48() - 1.);
    DBG("fx=%g, fy=%g, Xfluct=%g, Yfluct=%g", fx,fy,Xfluct,Yfluct);
    if(Xfluct + fx < -settings.fluct || Xfluct + fx > settings.fluct) Xfluct -= fx;
    else Xfluct += fx;
    if(Yfluct + fy < -settings.fluct || Yfluct + fy > settings.fluct) Yfluct -= fy;
    else Yfluct += fy;
    DBG("Xfluct=%g, Yfluct=%g, pix: %g/%g\n\n", Xfluct, Yfluct, Xfluct/settings.scale, Yfluct/settings.scale);
    Xcd += Xfluct/settings.scale; Ycd += Yfluct/settings.scale;
    Xc = (int) Xcd; Yc = (int) Ycd;
    test_template();
    return TRUE;
}


static int camcapt(cc_IMG *ima){
    DBG("Prepare, xc=%d, yc=%d, bitpix=%d", Xc, Yc, bitpix);
    if(!ima || !ima->data) return FALSE;
#ifdef EBUG
    double t0 = sl_dtime();
#endif
    ima->bitpix = bitpix;
    ima->w = camera.geometry.w;
    ima->h = camera.geometry.h;
    ima->bytelen = ima->w*ima->h*cc_getNbytes(ima);
    bzero(ima->data, ima->h*ima->w*cc_getNbytes(ima));
    if(!star) ERRX(_("No star template - die"));
    if(bitpix == 16) gen16(ima);
    else gen8(ima);
    DBG("Time of capture: %g", sl_dtime() - t0);
    return TRUE;
}

static int camsetbit(int b){
    bitpix = (b) ? 16 : 8;
    return TRUE;
}

static int camgetbp(uint8_t *bp){
    if(bp) *bp = bitpix;
    return TRUE;
}

static void camcancel(){
    capstat = CAPTURE_NO;
}

static int setdevno(int n){
    if(n) return FALSE;
    return TRUE;
}

static int camsetbrig(float b){
    brightness = b;
    return TRUE;
}

static int camgetbrig(float *b){
    if(!b) return FALSE;
    *b = brightness;
    return TRUE;
}

static int camsetexp(float t){
    exptime = t;
    return TRUE;
}
static int camsetgain(float g){
    gain = g;
    return TRUE;
}

static int camgetgain(float *g){
    if(g) *g = gain;
    return TRUE;
}

static int camsett(float t){
    camtemp = t;
    return TRUE;
}

static int camgett(float *t){
    if(t) *t = camtemp;
    return TRUE;
}

static int gett(float *t){
    if(t) *t = M_PI;
    return TRUE;
}

// Binning not supported, change scale instead!
static int camsetbin(int _U_ h, int _U_ v){
    return FALSE;
}

static int camshutter(_U_ cc_shutter_op s){
    return TRUE;
}

static int camsetgeom(cc_frameformat *f){
    if(!f) return FALSE;
    if(f->xoff > ARRAYW-2 || f->yoff > ARRAYH-2) return FALSE;
    if(f->xoff < 0 || f->yoff < 0 || f->h < 0 || f->w < 0) return FALSE;
    if(f->h + f->yoff > ARRAYH || f->w + f->xoff > ARRAYW) return FALSE;
    camera.geometry = *f;
    return TRUE;
}

static int camgetnam(char *n, int l){
    strncpy(n, "Star generator", l);
    return TRUE;
}

static int camgmg(float *mg){
    if(mg) *mg = 10.;
    return TRUE;
}

static int camggl(cc_frameformat *max, cc_frameformat *step){
    if(max) *max = camera.array;
    if(step) *step = (cc_frameformat){1,1,1,1};
    return TRUE;
}

static int camgetbin(int *binh, int *binv){
    if(binh) *binh = 1;
    if(binv) *binv = 1;
    return TRUE;
}

static int camgetio(int *io){
    if(io) *io = 0xDEADBEEF;
    return TRUE;
}

static int camfan(_U_ cc_fan_speed spd){return TRUE;}

static int focsetpos(_U_ int a, float n){
    if(n < 0. || n > focmaxpos) return FALSE;
    focuserpos = n;
    return TRUE;
}

static int fochome(_U_ int a){
    focuserpos = 0.;
    return TRUE;
}

static int focgetnam(char *n, int l){
    strncpy(n, "Dummy focuser", l);
    return TRUE;
}

static int focpos(float *p){
    if(p) *p = focuserpos;
    return TRUE;
}

static int focMp(float *p){
    if(p) *p = focmaxpos;
    return TRUE;
}

static int focmp(float *p){
    if(p) *p = 0.;
    return TRUE;
}

static int whlsetpos(int n){
    if(n >= filtermax || n < 0) return FALSE;
    filterpos = n;
    return TRUE;
}

static int whlgetpos(int *n){
    if(n) *n = filterpos;
    return TRUE;
}

static int whlgmp(int *n){
    if(n) *n = filtermax;
    return TRUE;
}

static int whlgetnam(char *n, int l){
    strncpy(n, "Dummy filter wheel", l);
    return TRUE;
}
/*
static cc_hresult setstarsamount(const char *str, cc_charbuff *ans){
    char buf[32], *bptr = buf;
    strncpy(buf, str, 31);
    char *val = cc_get_keyval(&bptr);
    if(val){ // setter
        ;
    }
    snprintf(buf, 31, "nstars=%d", settings.Nstars);
    cc_charbufaddline(ans, buf);
    return CC_RESULT_SILENCE;
}

static cc_hresult setstarno(const char *str, cc_charbuff *ans){
    return CC_RESULT_SILENCE;
}*/

static cc_hresult setXYs(const char *str, cc_charbuff *ans){
    char buf[256], *bptr = buf;
    strncpy(buf, str, 255);
    char *val = cc_get_keyval(&bptr);
    if(!val){ // getter - show all
        for(int i = 0; i < settings.Nstars; ++i){
            snprintf(buf, 255, "x[%d]=%g, y[%d]=%g\n", i, settings.xs[i], i, settings.ys[i]);
            cc_charbufaddline(ans, buf);
        }
        return CC_RESULT_SILENCE;
    }
    double dval = atof(val);
    if(strcmp(bptr, "x") == 0){
        DBG("x[%d]=%g", settings.curstarno, dval);
        settings.xs[settings.curstarno] = dval;
        snprintf(buf, 255, "x[%d]=%g\n", settings.curstarno, dval);
        cc_charbufaddline(ans, buf);
    } else if(strcmp(bptr, "y") == 0){
        DBG("y[%d]=%g", settings.curstarno, dval);
        settings.ys[settings.curstarno] = dval;
        snprintf(buf, 255, "y[%d]=%g\n", settings.curstarno, dval);
        cc_charbufaddline(ans, buf);
    }
    else{ return CC_RESULT_BADKEY;} // unreachable

    return CC_RESULT_SILENCE;
}

static cc_hresult setmag(const char *str, cc_charbuff *ans){
    char buf[256], *bptr = buf;
    strncpy(buf, str, 255);
    char *val = cc_get_keyval(&bptr);
    if(!val){ // getter
        for(int i = 0; i < settings.Nstars; ++i){
            snprintf(buf, 255, "mag[%d]=%g", i, settings.mag[i]);
            cc_charbufaddline(ans, buf);
        }
        return CC_RESULT_SILENCE;
    }
    double dval = atof(val);
    if(strcmp(bptr, "mag") != 0) return CC_RESULT_BADKEY;
    if(dval > magmax || dval < magmin){
        snprintf(buf, 255, "%g < mag < %g", magmin, magmax);
        cc_charbufaddline(ans, buf);
        return CC_RESULT_BADVAL;
    }
    DBG("mag[%d]=%g", settings.curstarno, dval);
    settings.mag[settings.curstarno] = dval;
    snprintf(buf, 255, "mag[%d]=%g\n", settings.curstarno, dval);
    cc_charbufaddline(ans, buf);
    return CC_RESULT_SILENCE;
}

static cc_hresult loadmask(const char *str, cc_charbuff *ans){
    char buf[FILENAME_MAX+32], *bptr = buf;
    strncpy(buf, str, FILENAME_MAX+31);
    char *val = cc_get_keyval(&bptr);
    if(strcmp(bptr, "mask") != 0) return CC_RESULT_BADKEY;
    if(imagemask) il_Image_free(&imagemask);
    imagemask = il_Image_read(val);
    char *nm = strdup (val);
    cc_hresult res = CC_RESULT_OK;
    if(!imagemask){
        snprintf(buf, FILENAME_MAX, "Can't read image '%s'", nm);
        res = CC_RESULT_FAIL;
    }else{
        if(imagemask->pixbytes != 1){
            snprintf(buf, FILENAME_MAX, "Image '%s' isn't a 8-bit image", nm);
            res = CC_RESULT_FAIL;
        }else
            snprintf(buf, FILENAME_MAX, "Got image '%s'; w=%d, h=%d, type=%d (impix=%d)", nm, imagemask->width, imagemask->height, imagemask->type, imagemask->pixbytes);
    }
    cc_charbufaddline(ans, buf);
    FREE(nm);
    return res;
}

static cc_hresult loadbg(const char *str, cc_charbuff *ans){
    char buf[FILENAME_MAX+32], *bptr = buf;
    strncpy(buf, str, FILENAME_MAX+31);
    char *val = cc_get_keyval(&bptr);
    if(strcmp(bptr, "bkg") != 0) return CC_RESULT_BADKEY;
    if(imagebg) il_Image_free(&imagebg);
    imagebg = il_Image_read(val);
    char *nm = strdup (val);
    cc_hresult res = CC_RESULT_OK;
    if(!imagebg){
        snprintf(buf, FILENAME_MAX, "Can't read image '%s'", nm);
        res = CC_RESULT_FAIL;
    }else{
        snprintf(buf, FILENAME_MAX, "Got image '%s'; w=%d, h=%d, type=%d (impix=%d)", nm, imagebg->width, imagebg->height, imagebg->type, imagebg->pixbytes);
    }
    cc_charbufaddline(ans, buf);
    FREE(nm);
    return res;
}

// cmd, help, handler, ptr, min, max, type
static cc_parhandler_t handlers[] = {
    {"beta", "Moffat `beta` parameter", NULL, (void*)&settings.beta, (void*)&betamin, NULL, CC_PAR_DOUBLE},
    {"bkg", "load background image", loadbg, (void*)&bgfilename, NULL, NULL, CC_PAR_STRING},
    {"curstar", "set number of current star to change parameters", NULL, (void*)&settings.curstarno, (void*)&nomin, (void*)&nomax, CC_PAR_INT},
    {"fluct", "stars position fluctuations (arcsec per sec)", NULL, (void*)&settings.fluct, (void*)&fluctmin, (void*)&fluctmax, CC_PAR_DOUBLE},
    {"fwhm", "stars min FWHM, arcsec", NULL, (void*)&settings.fwhm, (void*)&fwhmmin, (void*)&fwhmmax, CC_PAR_DOUBLE},
    {"lambda", "Poisson noice lambda value (>1) per second", NULL, (void*)&settings.noiselambda, (void*)&lambdamin, NULL, CC_PAR_DOUBLE},
    {"lambda0", "Poisson noice lambda value (>1) for dark noise", NULL, (void*)&settings.darklambda, (void*)&lambdamin, NULL, CC_PAR_DOUBLE},
    {"mag", "Next star magnitude: 0m is 0xffff/0xff (16/8 bit) ADUs per second", setmag, NULL, (void*)&magmin, (void*)&magmax, CC_PAR_DOUBLE},
    {"mask", "load mask image (binary, ANDed)", loadmask, (void*)&maskfilename, NULL, NULL, CC_PAR_STRING},
    {"nstars", "set amount of stars (not more than " Stringify(MAX_STARS) ")", NULL, (void*)&settings.Nstars, (void*)&nummin, (void*)&nummax, CC_PAR_INT},
    {"rotangle", "Starting rotation angle (arcsec)", NULL, (void*)&settings.rotan0, (void*)&rotanmin, (void*)&rotanmax, CC_PAR_DOUBLE},
    {"scale", "CCD scale: arcsec/pix", NULL, (void*)&settings.scale, (void*)&scalemin, (void*)&scalemax, CC_PAR_DOUBLE},
    {"vr", "rotation speed (arcsec/s)", NULL, (void*)&settings.vR, (void*)&vrotmin, (void*)&vrotmax, CC_PAR_DOUBLE},
    {"vx", "X axe drift speed (arcsec/s)", NULL, (void*)&settings.vX, (void*)&vmin, (void*)&vmax, CC_PAR_DOUBLE},
    {"vy", "Y axe drift speed (arcsec/s)", NULL, (void*)&settings.vY, (void*)&vmin, (void*)&vmax, CC_PAR_DOUBLE},
    {"x", "X coordinate of next star (arcsec, in field coordinate system)", setXYs, NULL, NULL, NULL, CC_PAR_DOUBLE},
    {"y", "Y coordinate of next star (arcsec, in field coordinate system)", setXYs, NULL, NULL, NULL, CC_PAR_DOUBLE},
    {"xc", "x center of field in array coordinates", NULL, (void*)&settings.x0, NULL, NULL, CC_PAR_INT},
    {"yc", "y center of field in array coordinates", NULL, (void*)&settings.y0, NULL, NULL, CC_PAR_INT},
    //{"", "", NULL, (void*)&settings., (void*)&, (void*)&, CC_PAR_DOUBLE},
    CC_PARHANDLER_END
};

static cc_hresult plugincmd(const char *str, cc_charbuff *buf){
    return cc_plugin_customcmd(str, handlers, buf);
}

static int stub(){
    return TRUE;
}

static void vstub(){
    FNAME();
    return;
}
static int istub(_U_ int N){return TRUE;}

/*
 * Global objects: camera, focuser and wheel
 */
__attribute__ ((visibility("default"))) cc_Camera camera = {
    .check = stub,
    .Ndevices = 1,
    .close = vstub,
    .pollcapture = campoll,
    .capture = camcapt,
    .cancel = camcancel,
    .startexposition = startexp,
    .plugincmd = plugincmd,
    // setters:
    .setDevNo = setdevno,
    .setbrightness = camsetbrig,
    .setexp = camsetexp,
    .setgain = camsetgain,
    .setT = camsett,
    .setbin = camsetbin,
    .setnflushes = istub,
    .shuttercmd = camshutter,
    .confio = istub,
    .setio = istub,
    .setframetype = istub,
    .setbitdepth = camsetbit,
    .setfastspeed = istub,
    .setgeometry = camsetgeom,
    .setfanspeed = camfan,
    // getters:
    .getbitpix = camgetbp,
    .getbrightness = camgetbrig,
    .getModelName = camgetnam,
    .getgain = camgetgain,
    .getmaxgain = camgmg,
    .getgeomlimits = camggl,
    .getTcold = camgett,
    .getThot = camgett,
    .getTbody = gett,
    .getbin = camgetbin,
    .getio = camgetio,
    .pixX = 10.,
    .pixY = 10.,
    .field = (cc_frameformat){.h = 1024, .w = 1024, .xoff = 10, .yoff = 10},
    .array = (cc_frameformat){.h = ARRAYH, .w = ARRAYW, .xoff = 0, .yoff = 0},
    .geometry = {.xoff = 10, .yoff = 10, .h = 1024, .w = 1024},
};

__attribute__ ((visibility("default"))) cc_Focuser focuser = {
    .check = stub,
    .Ndevices = 1,
    .close = vstub,
    // setters:
    .setDevNo = setdevno,
    .setAbsPos = focsetpos,
    .home = fochome,
    // getters:
    .getModelName = focgetnam,
    .getTbody = gett,
    .getPos = focpos,
    .getMaxPos = focMp,
    .getMinPos = focmp,
};

__attribute__ ((visibility("default"))) cc_Wheel wheel = {
    .check = stub,
    .Ndevices = 1,
    .close = vstub,
    // setters
    .setDevNo = setdevno,
    .setPos = whlsetpos,
    // getters
    .getModelName = whlgetnam,
    .getTbody = gett,
    .getPos = whlgetpos,
    .getMaxPos = whlgmp,
};
