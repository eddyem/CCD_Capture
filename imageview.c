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

#include <X11/Xlib.h> // XInitThreads();
#include <math.h>     // roundf(), log(), sqrt()
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "ccdfunc.h"
#include "cmdlnopts.h"
#include "imageview.h"
#include "events.h"
#include "omp.h"
//#include "socket.h" // for timestamp

windowData *win = NULL; // main window (common variable for events.c)
static pthread_t GLUTthread = 0; // main GLUT thread
static int imequalize = FALSE;
static int initialized = 0; // ==1 if GLUT is initialized; ==0 after clear_GL_context

static void *Redraw(_U_ void *p);
static void createWindow();
static void RedrawWindow();
static void Resize(int width, int height);

/**
 * Init freeGLUT
 */
static void imageview_init(){
    FNAME();
    char *v[] = {"Image view window", NULL};
    int c = 1;
    if(initialized){
        WARNX(_("Already initialized!"));
        return;
    }
    XInitThreads(); // we need it for threaded windows
    glutInit(&c, v);
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_CONTINUE_EXECUTION);
    initialized = 1;
}

/**
 * calculate window properties on creating & resizing
 */
void calc_win_props(GLfloat *Wortho, GLfloat *Hortho){
    if(!win || ! win->image) return;
    float a, A, w, h, W, H;
    float Zoom = win->zoom;
    w = (float)win->image->w / 2.f;
    h = (float)win->image->h / 2.f;
    W = (float)win->w;
    H =(float) win->h;
    A = W / H;
    a = w / h;
    if(A > a){ // now W & H are parameters for glOrtho
        win->Daspect = (float)h / H * 2.f;
        W = h * A; H = h;
    }else{
        win->Daspect = (float)w / W * 2.f;
        H = w / A; W = w;
    }
    if(Wortho) *Wortho = W;
    if(Hortho) *Hortho = H;
    // calculate coordinates of center
    win->x0 = W/Zoom - w + win->x / Zoom;
    win->y0 = H/Zoom + h - win->y / Zoom;
}

/**
 * create window & run main loop
 */
static void createWindow(){
    DBG("ini=%d, win %s", initialized, win ? "yes" : "no");
    if(!initialized) return;
    if(!win) return;
    int w = win->w, h = win->h;
    DBG("create window with title %s", win->title);
    glutInitWindowSize(w, h);
    win->ID = glutCreateWindow(win->title);
    DBG("created GL_ID=%d", win->ID);
    glutReshapeFunc(Resize);
    glutDisplayFunc(RedrawWindow);
    glutKeyboardFunc(keyPressed);
    //glutSpecialFunc(keySpPressed);
    //glutMouseWheelFunc(mouseWheel);
    glutMouseFunc(mousePressed);
    glutMotionFunc(mouseMove);
    //glutIdleFunc(glutPostRedisplay);
    glutIdleFunc(RedrawWindow);
    DBG("init textures");
    glGenTextures(1, &(win->Tex));
    calc_win_props(NULL, NULL);
    win->zoom = 1. / win->Daspect;
    glEnable(GL_TEXTURE_2D);
    // the hext 4 lines need to unaligned storage
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindTexture(GL_TEXTURE_2D, win->Tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, win->image->w, win->image->h, 0,
            GL_RGB, GL_UNSIGNED_BYTE, win->image->rawdata);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_TEXTURE_2D);
    createMenu();
    DBG("Window opened");
}

static int killwindow(){
    if(!win) return 0;
    if(!win->killthread){
        // say threads to die
        win->killthread = 1;
    }
    //DBG("Lock mutex");
    //pthread_mutex_lock(&win->mutex);
    //pthread_join(win->thread, NULL); // wait while thread dies
    if(win->menu){
        DBG("Destroy menu");
        glutDestroyMenu(win->menu);
    }
    DBG("Destroy window");
    glutDestroyWindow(win->ID);
    DBG("Delete textures");
    glDeleteTextures(1, &(win->Tex));
    DBG("Cancel");
    windowData *old = win;
    win = NULL;
    DBG("free(rawdata)");
    FREE(old->image->rawdata);
    DBG("free(image)");
    FREE(old->image);
    //pthread_mutex_unlock(&old->mutex);
    DBG("free(win)");
    FREE(old);
    DBG("return");
    return 1;
}

/*
static void renderBitmapString(float x, float y, void *font, char *string, GLubyte *color){
    if(!initialized) return;
    char *c;
    int x1=x, W=0;
    for(c = string; *c; c++){
        W += glutBitmapWidth(font,*c);// + 1;
    }
    x1 -= W/2;
    glColor3ubv(color);
    glLoadIdentity();
    glTranslatef(0.,0., -150);
    //glTranslatef(x,y, -4000.);
    for (c = string; *c != '\0'; c++){
        glColor3ubv(color);
        glRasterPos2f(x1,y);
        glutBitmapCharacter(font, *c);
        //glutStrokeCharacter(GLUT_STROKE_ROMAN, *c);
        x1 = x1 + glutBitmapWidth(font,*c);// + 1;
    }
}*/

static void RedrawWindow(){
    //DBG("ini=%d, win=%s", initialized, win ? "yes" : "no");
    if(!initialized || !win || win->killthread) return;
    if(pthread_mutex_trylock(&win->mutex)) return;
    GLfloat w = win->image->w, h = win->image->h;
    glutSetWindow(win->ID);
    glClearColor(0.0, 0.0, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(win->x, win->y, 0.);
    glScalef(-win->zoom, -win->zoom, 1.);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, win->Tex);
    if(win->image->changed){
        DBG("Image changed!");
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, win->image->w, win->image->h, 0,
                GL_RGB, GL_UNSIGNED_BYTE, win->image->rawdata);
       /* glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, win->image->w, win->image->h,
                        GL_RGB, GL_UNSIGNED_BYTE, win->image->rawdata);*/
        win->image->changed = 0;
    }
    w /= 2.f; h /= 2.f;
    float lr = 1., ud = -1.; // flipping coefficients (mirror image around Y by default)
    if(win->flip & WIN_FLIP_LR) lr = -1.;
    if(win->flip & WIN_FLIP_UD) ud = 1.;
    glBegin(GL_QUADS);
        glTexCoord2f(1.0f, 1.0f); glVertex2f( -1.f*lr*w, ud*h ); // top right
        glTexCoord2f(1.0f, 0.0f); glVertex2f( -1.f*lr*w, -1.f*ud*h ); // bottom right
        glTexCoord2f(0.0f, 0.0f); glVertex2f(lr*w, -1.f*ud*h ); // bottom left
        glTexCoord2f(0.0f, 1.0f); glVertex2f(lr*w,  ud*h ); // top left
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glFinish();
    glutSwapBuffers();
    pthread_mutex_unlock(&win->mutex);
    usleep(1000);
}

/**
 * main freeGLUT loop
 * waits for global signals to create windows & make other actions
 */
static void *Redraw(_U_ void *arg){
    FNAME();
    createWindow();
    glutMainLoop();
    return NULL;
}

static void Resize(int width, int height){
    FNAME();
    if(!initialized) return;
    if(!initialized || !win || win->killthread) return;
    glutReshapeWindow(width, height);
    win->w = width;
    win->h = height;
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    GLfloat W, H;
    calc_win_props(&W, &H);
    glOrtho(-W,W, -H,H, -1., 1.);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/**
 * create new window, run thread & return pointer to its structure or NULL
 * asynchroneous call from outside
 * wait for window creating & return its data
 * @param title - header (copyed inside this function)
 * @param w,h   - image size
 * @param rawdata - NULL (then the memory will be allocated here with size w x h)
 *            or allocated outside data
 */
static void createGLwin(char *title, int w, int h, rawimage *rawdata){
    FNAME();
    if(!initialized) imageview_init();
    if(win) killwindow();
    win = MALLOC(windowData, 1);
    rawimage *raw;
    if(rawdata){
        raw = rawdata;
    }else{
        raw = MALLOC(rawimage, 1);
        if(raw){
            raw->rawdata = MALLOC(GLubyte, w*h*3);
            raw->w = w;
            raw->h = h;
            raw->changed = 1;
            // raw->protected is zero automatically
        }
    }
    if(!raw || !raw->rawdata){
        FREE(raw);
        FREE(win);
        return;
    }
    win->title = strdup(title);
    win->image = raw;
    if(pthread_mutex_init(&win->mutex, NULL)){
        WARN(_("Can't init mutex!"));
        killwindow();
        return;
    }
    win->w = w;
    win->h = h;
    pthread_create(&GLUTthread, NULL, &Redraw, NULL);
}

/*
 * Coordinates transformation from CS of drawingArea into CS of picture
 * 		x,y - pointer coordinates
 * 		X,Y - coordinates of appropriate point at picture
 */
void conv_mouse_to_image_coords(int x, int y,
                                float *X, float *Y,
                                windowData *window){
    float a = window->Daspect / window->zoom;
    *X = (float)x * a - window->x0;
    *Y = window->y0 - (float)y * a;
}

void conv_image_to_mouse_coords(float X, float Y,
                                int *x, int *y,
                                windowData *window){
    float a = window->zoom / window->Daspect;
    *x = (int)roundf((X + window->x0) * a);
    *y = (int)roundf((window->y0 - Y) * a);
}

/**
 * Convert gray (unsigned short) into RGB components (GLubyte)
 * @argument L   - gray level (0..1)
 * @argument rgb - rgb array (GLubyte [3])
 */
static void gray2rgb(double gray, GLubyte *rgb){
    int i = gray * 4.;
    double x = (gray - (double)i * .25) * 4.;
    GLubyte r = 0, g = 0, b = 0;
    //r = g = b = (gray < 1) ? gray * 256 : 255;
    switch(i){
        case 0:
            g = (GLubyte)(255. * x);
            b = 255;
        break;
        case 1:
            g = 255;
            b = (GLubyte)(255. * (1. - x));
        break;
        case 2:
            r = (GLubyte)(255. * x);
            g = 255;
        break;
        case 3:
            r = 255;
            g = (GLubyte)(255. * (1. - x));
        break;
        default:
            r = 255;
    }
    *rgb++ = r;
    *rgb++ = g;
    *rgb   = b;
}

typedef enum{
    COLORFN_LINEAR, // linear
    COLORFN_LOG,    // ln
    COLORFN_SQRT,   // sqrt
    COLORFN_POW,    // power
    COLORFN_MAX     // end of list
} colorfn_type;

static colorfn_type ft = COLORFN_LINEAR;

// all colorfun's should get argument in [0, 1] and return in [0, 1]
static double linfun(double arg){ return arg; } // bung for PREVIEW_LINEAR
static double logfun(double arg){ return log(1.+arg) / 0.6931472; } // for PREVIEW_LOG [log_2(x+1)]
static double powfun(double arg){ return arg * arg;}
static double (*colorfun)(double) = linfun; // default function to convert color

static void change_colorfun(colorfn_type f){
    DBG("New colorfn: %d", f);
    const char *cfn = NULL;
    ft = f;
    switch (f){
        case COLORFN_LOG:
            colorfun = logfun;
            cfn = "log";
        break;
        case COLORFN_SQRT:
            colorfun = sqrt;
            cfn = "sqrt";
        break;
        case COLORFN_POW:
            colorfun = powfun;
            cfn = "square";
        break;
        default: // linear
            colorfun = linfun;
            cfn = "linear";
    }
    verbose(1, _("Histogram conversion: %s"), cfn);
}

// cycle switch between palettes
static void roll_colorfun(){
    colorfn_type t = ++ft;
    if(t == COLORFN_MAX) t = COLORFN_LINEAR;
    change_colorfun(t);
}

/**
 * @brief equalize - hystogram equalization
 * @param ori (io) - input/output data
 * @param w,h      - image width and height
 * @return data allocated here
 */
static uint8_t *equalize(uint16_t *ori, int w, int h){
    uint8_t *retn = MALLOC(uint8_t, w*h);
    double orig_hysto[0x10000] = {0.}; // original hystogram
    uint8_t eq_levls[0x10000] = {0};   // levels to convert: newpix = eq_levls[oldpix]
    int s = w*h;
    //double t0 = dtime();
    /* -- takes too long because of sync
#pragma omp parallel
{
    //printf("%d\n", omp_get_thread_num());
    size_t histogram_private[0x10000] = {0};
    #pragma omp for nowait
    for(int i = 0; i < s; ++i){
        ++histogram_private[ori[i]];
    }
    #pragma omp critical
    {
        for(int i = 0; i < 0x10000; ++i) orig_hysto[i] += histogram_private[i];
    }
}*/
    for(int i = 0; i < s; ++i){
        ++orig_hysto[ori[i]];
    }
    //DBG("HISTOGRAM takes %g", dtime() - t0);
    double part = (double)(s + 1) / 0x100, N = 0.;
    for(int i = 0; i <= 0xffff; ++i){
        N += orig_hysto[i];
        eq_levls[i] = (uint8_t)(N/part);
    }
    //OMP_FOR() -- takes too long!
    for(int i = 0; i < s; ++i){
        retn[i] = eq_levls[ori[i]];
    }
    //DBG("EQUALIZATION takes %g", dtime() - t0);
    return retn;
}

static void change_displayed_image(IMG *img){
    if(!win || !img) return;
    pthread_mutex_lock(&win->mutex);
    rawimage *im = win->image;
    DBG("imh=%d, imw=%d, ch=%u, cw=%u", im->h, im->w, img->h, img->w);
    int w = img->w, h = img->h, s = w*h;
    if(!im || (im->h != h) || (im->w != w)){ // realloc image to new size
        DBG("\n\nRealloc rawdata");
        if(!im) im = MALLOC(rawimage, 1);
        if(im->h*im->w < s) im->rawdata = realloc(im->rawdata, s*3);
        im->h = h; im->w = w;
        win->image = im;
        DBG("win->image changed");
    }
    if(imequalize){
        DBG("equalize");
        uint8_t *newima = equalize(img->data, w, h);
        GLubyte *dst = im->rawdata;
        DBG("convert; s=%d, im->s=%d", s, im->h*im->w);
        // openmp would make all calculations MORE SLOW than even in one thread!
        //OMP_FOR()
        for(int i = 0; i < s; ++i){
            gray2rgb(colorfun(newima[i] / 256.), &dst[i*3]);
        }
        FREE(newima);
    }else{
        DBG("convert; s=%d, im->s=%d", s, im->h*im->w);
        GLubyte *dst = im->rawdata;
        //OMP_FOR()
        for(int i = 0; i < s; ++i){
            gray2rgb(colorfun(img->data[i] / 65536.), &dst[i*3]);
        }
    }
    /*
    // mirror image around Y
    int w3 = w*3, h1 = h-1, wsz = w3*sizeof(GLubyte);
#pragma omp parallel
{
    GLubyte *b = MALLOC(GLubyte, w3);
    #pragma omp for nowait
    for(int y = 0; y < h / 2; ++y){
        memcpy(b, &im->rawdata[w3*y], wsz);
        memcpy(&im->rawdata[w3*y], &im->rawdata[w3*(h1-y)], wsz);
        memcpy(&im->rawdata[w3*(h1-y)], b, wsz);
    }
    FREE(b);
}*/
    DBG("Unlock");
    win->image->changed = 1;
    pthread_mutex_unlock(&win->mutex);
}

#if 0
// thread for checking
static void* image_thread(void *data){
    FNAME();
    IMG *(*newimage)(const char *) = (IMG*(*)(const char*)) data;
    IMG *img = NULL;
    while(1){
        if(!win || win->killthread){
            DBG("got killthread");
            clear_GL_context();
            pthread_exit(NULL);
        }
        if(win && win->winevt){
            if(win->winevt & WINEVT_SAVEIMAGE){ // save image
                verbose(2, "Make screenshot\n");
                saveFITS(img, NULL);
                win->winevt &= ~WINEVT_SAVEIMAGE;
            }
            if(win->winevt & WINEVT_ROLLCOLORFUN){
                roll_colorfun();
                win->winevt &= ~WINEVT_ROLLCOLORFUN;
                change_displayed_image(win, img);
            }
            if(win->winevt & WINEVT_EQUALIZE){
                win->winevt &= ~WINEVT_EQUALIZE;
                imequalize = !imequalize;
                verbose(1, _("Equalization of histogram: %s"), imequalize ? N_("on") : N_("off"));
            }
        }
        usleep(10000);
    }
}
#endif

void closeGL(){
    if(win) win->killthread = 1;
    usleep(1000);
    if(!initialized) return;
    initialized = 0;
    camstop(); // cancel expositions
    //DBG("Leave mainloop");
    //glutLeaveMainLoop();
    DBG("kill");
    killwindow();
    DBG("join");
    if(GLUTthread) pthread_join(GLUTthread, NULL); // wait while main thread exits
    DBG("main GL thread cancelled");
    usleep(1000);
}

/**
 * @brief viewer - main viewer process
 * @param newimage - image refresh function
 *              it shouldn't `free` it's argument!!!
 * @return 0 if all OK
 */
int viewer(imagefunc newimage){
    if(!newimage){
        WARNX("No image changing function defined");
        return 1;
    }
    imageview_init();
    DBG("Create new win");
    createGLwin("Sample window", 1024, 1024, NULL);
    if(!win){
        WARNX(_("Can't open OpenGL window, image preview will be inaccessible"));
        return 1;
    }
    IMG *img = NULL;
    //double t0 = dtime();
    while(1){
        if(!win || win->killthread){
            DBG("got killthread");
            newimage((void*)-1);
            signals(0); // just run common cleaner
            return 0;
        }
        if((win->winevt & WINEVT_GETIMAGE) || !(win->winevt & WINEVT_PAUSE)){
            //DBG("CHK new image, t=%g", dtime()-t0);
            if(newimage(&img)){
                //TIMESTAMP("got image -> change");
                win->winevt &= ~WINEVT_GETIMAGE;
                change_displayed_image(img); // change image if refreshed
                //TIMESTAMP("changed");
            }
            //DBG("Next cycle, t=%g", dtime()-t0);
        }
        if(!win->winevt){
            //DBG("No events");
            //usleep(1000);
            continue;
        }
        if(win->winevt & WINEVT_SAVEIMAGE){ // save image
            verbose(2, "Make screenshot\n");
            saveFITS(img, NULL);
            win->winevt &= ~WINEVT_SAVEIMAGE;
        }
        if(win->winevt & WINEVT_ROLLCOLORFUN){
            roll_colorfun();
            win->winevt &= ~WINEVT_ROLLCOLORFUN;
            change_displayed_image(img);
        }
        if(win->winevt & WINEVT_EQUALIZE){
            win->winevt &= ~WINEVT_EQUALIZE;
            imequalize = !imequalize;
            verbose(1, _("Equalization of histogram: %s"), imequalize ? N_("on") : N_("off"));
            change_displayed_image(img);
        }
    }
}

// getter for events.c
windowData* getWin(){ return win;}
