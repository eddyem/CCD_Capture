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
#include <stdatomic.h>
#include <math.h>  // isnan
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <usefull_macros.h>

#include "ccdfunc.h" // framerate
#include "client.h"
#include "cmdlnopts.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif
#include "server.h" // for common commands names
#include "socket.h"

extern double answer_timeout;

static char sendbuf[BUFSIZ];
static char *lastfilename = NULL;
// send message and wait any answer
#define SENDMSG(...) do{DBG("SENDMSG"); snprintf(sendbuf, BUFSIZ-1, __VA_ARGS__); verbose(VERBOSE_SECONDARY, "\t> %s", sendbuf); if(!cc_sendstrmessage(sock, sendbuf)) ERRX(_("Server disconnected")); getans(sock, NULL);} while(0)
// send message and wait answer starting with 'cmd'
#define SENDMSGW(cmd, ...) do{DBG("SENDMSGW"); snprintf(sendbuf, BUFSIZ-1, cmd __VA_ARGS__); verbose(VERBOSE_SECONDARY, "\t> %s", sendbuf); if(!cc_sendstrmessage(sock, sendbuf)) ERRX(_("Server disconnected")); else getans(sock, cmd);}while(0)
// send command and wait for answer on it
#define SENDCMDW(cmd) do{DBG("SENDCMDW"); strncpy(sendbuf, cmd, BUFSIZ-1); verbose(VERBOSE_SECONDARY, "\t> %s", sendbuf); if(!cc_sendstrmessage(sock, sendbuf)) ERRX(_("Server disconnected")); else getans(sock, cmd);}while(0)
static volatile atomic_int expstate = CAMERA_CAPTURE;
static int xm0,ym0,xm1,ym1; // max format
static int xc0,yc0,xc1,yc1; // current format

static volatile atomic_int grabno = 0;
static int oldgrabno = 0;
// IPC key for shared memory
static cc_IMG *locima = NULL, *shmima = NULL; // ima - local storage, shmima - shm (if available)
static volatile atomic_int current_image_number = -1; // for net-parser - last number of exposed image

#if 0
// read message from queue or file descriptor
static char *readmsg(int fd){
    static cc_strbuff *buf = NULL;
    if(!buf) buf = cc_strbufnew(BUFSIZ, 256);
    int test(){
        size_t got = cc_getline(buf);
        if(got > 255){
            DBG("Client fd=%d gave buffer overflow", fd);
            LOGMSG("SERVER client fd=%d buffer overflow", fd);
        }else if(got){
            return TRUE;
        }
        return FALSE;
    }
    if(test()) return buf->string;
    if(1 == sl_canread(fd)){
        if(cc_read2buf(fd, buf)){
            if(test()) return buf->string;
        }else ERRX("Server disconnected");
    }
    return NULL;
}
#endif

#define CMP_ANS(cmd, ans)   strncmp(cmd, ans, sizeof(cmd)-1)
// parser of CCD server messages; return parsing result
static cc_hresult parseans(char *ans){
    if(!ans) return CC_RESULT_BADKEY;
    //TIMESTAMP("parseans() begin");
    //DBG("Parsing of '%s'", ans);
    if(0 == strcmp(cc_hresult2str(CC_RESULT_OK), ans)) return CC_RESULT_OK;
    for(cc_hresult res = CC_RESULT_BUSY; res < CC_RESULT_NUM; ++res){
        const char *resmsg = cc_hresult2str(res);
        if(0 == strcmp(resmsg, ans)){
            verbose(VERBOSE_PRIMARY, "Server answered: %s", resmsg);
            return res;
        }
    }
    char *val = cc_get_keyval(&ans); // now `ans` is a key and `val` its value
    if(0 == CMP_ANS(CC_CMD_EXPSTATE, ans)){
        int st = atoi(val), oldst = atomic_load(&expstate);
        if(oldst != CAMERA_FRAMERDY){
            atomic_store(&expstate, st);
            DBG("Current state: %d", atomic_load(&expstate));
        }
    }else if(0 == CMP_ANS(CC_CMD_FRAMEMAX, ans)){
        sscanf(val, "%d,%d,%d,%d", &xm0, &ym0, &xm1, &ym1);
        DBG("Got maxformat: %d,%d,%d,%d", xm0, ym0, xm1, ym1);
    }else if(0 == CMP_ANS(CC_CMD_FRAMEFORMAT, ans)){
        sscanf(val, "%d,%d,%d,%d", &xc0, &yc0, &xc1, &yc1);
        DBG("Got current format: %d,%d,%d,%d", xc0, yc0, xc1, yc1);
    }else if(0 == CMP_ANS(CC_CMD_IMNUMBER, ans)){
        atomic_store(&current_image_number, atoi(val));
    }
    //TIMESTAMP("parseans() end");
    return CC_RESULT_SILENCE; // echo of sent command or something else
}

// read until timeout all messages from server; return FALSE if there was no messages from server
// if msg != NULL - wait for it in answer
static int getans(int sock, const char *msg){
    double t0 = sl_dtime();
    static sl_ringbuffer_t *RB = NULL;
    if(!RB){
        RB = sl_RB_new(BUFSIZ);
        if(!RB){
            WARNX(_("Can't allocate buffer"));
            return FALSE;
        }
    }
    TIMESTAMP("GetAns(%s)", msg);
    double tmout = answer_timeout + 5.; // make first timeout larger for slow networks
    // TODO: increase first timeout up to 15-30s (add key?)
    cc_hresult res = CC_RESULT_FAIL;
    char buf[256];
    while(sl_dtime() - t0 < tmout){
        if(1 == sl_canread(sock)){
            ssize_t got = read(sock, buf, 255);
            if(got > 0){
                DBG("Got %zd bytes (%s)", got, buf);
                if(got != (ssize_t)sl_RB_write(RB, (uint8_t*)buf, got)){
                    WARNX("Ringbuffer overflow??");
                    sl_RB_clearbuf(RB);
                    return FALSE;
                }
            }else if(got < 0){
                DBG("ERR: got=%zd", got);
                if(errno == EAGAIN){
                    usleep(1000);
                    continue;
                }
                WARNX(_("Server disconnected?"));
                LOGWARN("Server disconnected?");
                return FALSE;
            }
        }
        if(sl_RB_hasbyte(RB, '\n') < 0) continue;
        DBG("Got newline in RB");
        if(sl_RB_readline(RB, buf, 255) < 1){
            WARNX(_("Got empty string"));
            continue;
        }
        // got answer -> make timeout less
        tmout = answer_timeout;
        t0 = sl_dtime();
        TIMESTAMP("Got from server: %s", buf);
        verbose(VERBOSE_PRIMARY, "\t%s", buf);
        DBG("1 msg-> %s, ans -> %s", msg, buf);
        res = parseans(buf);
        DBG("2 msg-> %s, ans -> %s; result: %d", msg, buf, res);
        if(msg){
            if(res != CC_RESULT_SILENCE || strncmp(buf, msg, strlen(msg))) continue;
            else res = CC_RESULT_OK;
        }
        DBG("Got answer -> break");
        break;
    }
    TIMESTAMP("GetAns(%s), result: %d", msg, res);
    if(res == CC_RESULT_OK) return TRUE;
    return FALSE;
}

/**
 * @brief processData - process here some actions and make messages for server
 */
static void send_headers(int sock){
    if(GP->plugincmd){
        char **p = GP->plugincmd;
        verbose(VERBOSE_PRIMARY, _("Send custom plugin commands\n"));
        while(p && *p){
            verbose(VERBOSE_PRIMARY, "\t%s\n", *p);
            SENDMSGW(CC_CMD_PLUGINCMD, "=%s", *p);
            ++p;
        }
        while(getans(sock, NULL));
    }
    if(GP->exptime > -DBL_EPSILON) SENDMSGW(CC_CMD_EXPOSITION, "=%g", GP->exptime);
    DBG("infty=%d", GP->infty);
    if(GP->infty > -1) SENDMSGW(CC_CMD_INFTY, "=%d", GP->infty);
    // common information
    if(GP->listdevices) SENDCMDW(CC_CMD_CAMLIST);
    if(GP->camdevno > -1) SENDMSGW(CC_CMD_CAMDEVNO, "=%d", GP->camdevno);
    if(GP->info){
        SENDCMDW(CC_CMD_HBIN);
        SENDCMDW(CC_CMD_VBIN);
        SENDCMDW(CC_CMD_CAMTEMPER);
        SENDCMDW(CC_CMD_EXPOSITION);
        SENDCMDW(CC_CMD_EXPSTATE);
    }
    // focuser
    if(GP->listdevices) SENDMSG(CC_CMD_FOCLIST);
    if(GP->focdevno > -1) SENDMSG(CC_CMD_FDEVNO "=%d", GP->focdevno);
    if(GP->info){
        SENDCMDW(CC_CMD_FGOTO);
        SENDCMDW(CC_CMD_FMINPOS);
        SENDCMDW(CC_CMD_FMAXPOS);
        SENDCMDW(CC_CMD_FTEMP);
    }
    if(!isnan(GP->gotopos)){
        SENDMSGW(CC_CMD_FGOTO, "=%g", GP->gotopos);
    }
    // wheel
    if(GP->listdevices) SENDCMDW(CC_CMD_WLIST);
    if(GP->whldevno > -1) SENDMSGW(CC_CMD_WDEVNO, "=%d", GP->whldevno);
    if(GP->info){
        SENDCMDW(CC_CMD_WPOS);
        SENDCMDW(CC_CMD_WMAXPOS);
        SENDCMDW(CC_CMD_WTEMP);
    }
    if(GP->setwheel > -1) SENDMSGW(CC_CMD_WPOS, "=%d", GP->setwheel);
    DBG("nxt");
    // CCD/CMOS
    if(GP->X0 > INT_MIN || GP->Y0 > INT_MIN || GP->X1 > INT_MIN || GP->Y1 > INT_MIN){ // set format
        SENDCMDW(CC_CMD_FRAMEMAX);
        SENDCMDW(CC_CMD_FRAMEFORMAT);
        // default values
        if(GP->X0 == INT_MIN) GP->X0 = xc0;
        if(GP->X1 == INT_MIN) GP->X1 = xc1;
        if(GP->Y0 == INT_MIN) GP->Y0 = yc0;
        if(GP->Y1 == INT_MIN) GP->Y1 = yc1;
        // limiting values
        if(GP->X0 < 0) GP->X0 = xm0;
        else if(GP->X0 > xm1-1) GP->X0 = xm1-1;
        if(GP->Y0 < 0) GP->Y0 = ym0;
        else if(GP->Y0 > ym1-1) GP->Y0 = ym1-1;
        if(GP->X1 < GP->X0+1 || GP->X1 > xm1) GP->X1 = xm1;
        if(GP->Y1 < GP->Y0+1 || GP->Y1 > ym1) GP->Y1 = ym1;
        DBG("set format: (%d,%d)x(%d,%d)", GP->X0,GP->X1,GP->Y0,GP->Y1);
        SENDMSGW(CC_CMD_FRAMEFORMAT, "=%d,%d,%d,%d", GP->X0, GP->Y0, GP->X1, GP->Y1);
    }
    if(GP->cancelexpose) SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_IDLE);
    if(GP->hbin) SENDMSGW(CC_CMD_HBIN, "=%d", GP->hbin);
    if(GP->vbin) SENDMSGW(CC_CMD_VBIN, "=%d", GP->vbin);
    if(!isnan(GP->temperature)) SENDMSGW(CC_CMD_CAMTEMPER, "=%g", GP->temperature);
    if(GP->shtr_cmd > -1) SENDMSGW(CC_CMD_SHUTTER, "=%d", GP->shtr_cmd);
    if(GP->confio > -1) SENDMSGW(CC_CMD_CONFIO, "=%d", GP->confio);
    if(GP->setio > -1) SENDMSGW(CC_CMD_IO, "=%d", GP->setio);\
    if(!isnan(GP->gain)) SENDMSGW(CC_CMD_GAIN, "=%g", GP->gain);
    if(!isnan(GP->brightness)) SENDMSGW(CC_CMD_BRIGHTNESS, "=%g", GP->brightness);
    if(GP->nflushes > 0) SENDMSGW(CC_CMD_NFLUSHES, "=%d", GP->nflushes);
    if(GP->exptime > -DBL_EPSILON){ // exposition and reading control: only if start of exposition
        if(GP->_8bit) SENDMSGW(CC_CMD_8BIT, "=1");
        else SENDMSGW(CC_CMD_8BIT, "=0");
        if(GP->fast) SENDMSGW(CC_CMD_FASTSPD, "=1");
        else SENDMSGW(CC_CMD_FASTSPD, "=0");
        if(GP->dark) SENDMSGW(CC_CMD_DARK, "=1");
        else SENDMSGW(CC_CMD_DARK, "=0");
    }
#if 0
    // FITS header keywords:
#define CHKHDR(x, cmd)   do{if(x) SENDMSG(cmd "=%s", x);}while(0)
    CHKHDR(GP->author, CC_CMD_AUTHOR);
    CHKHDR(GP->instrument, CC_CMD_INSTRUMENT);
    CHKHDR(GP->observers, CC_CMD_OBSERVER);
    CHKHDR(GP->objname, CC_CMD_OBJECT);
    CHKHDR(GP->prog_id, CC_CMD_PROGRAM);
    CHKHDR(GP->objtype, CC_CMD_OBJTYPE);
#undef CHKHDR
#endif
}

/**
 * @brief readNbytes - read `N` bytes from descriptor `fd` into buffer *buf
 * @return false if failed
 */
static int readNbytes(int fd, size_t N, uint8_t *buf){
    size_t got = 0, need = N;
    double t0 = sl_dtime();
    while(sl_dtime() - t0 < CC_CLIENT_TIMEOUT /*&& sl_canread(fd)*/ && need){
        ssize_t rd = read(fd, buf + got, need);
        if(rd <= 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK) continue;
            WARNX(_("Server disconnected"));
            signals(1);
        }
        got += rd; need -= rd;
        t0 = sl_dtime();
    }
    if(need) return FALSE; // didn't got whole packet
    return TRUE;
}

static int refresh_shm(){
    if(!shmima){
        shmima = cc_getshm(GP->shmkey, 0); // try to init client shm
        if(shmima){
            cc_init_sem(FALSE);
            DBG("Got access to shared memory");
            return TRUE;
        }
        return FALSE;
    }
    int shmid = shmget(GP->shmkey, 0, 0);
    if(shmid < 0){
        // Segment was deleted
        shmdt(shmima);
        shmima = NULL;
        // refresh connection
        return refresh_shm();
    }
    struct shmid_ds buf;
    if(shmctl(shmid, IPC_STAT, &buf) == 0){
        if(buf.shm_perm.mode & SHM_DEST){ // marked for deletion
            shmdt(shmima);
            shmima = NULL;
            return refresh_shm();
        }
        // valid segment
        return TRUE;
    }
    return FALSE;
}

static int lockshm(){
    int locked = FALSE;
    double t0 = sl_dtime();
    while(sl_dtime() - t0 < MUTEX_LOCK_TMOUT){
        if(cc_lock_shm(FALSE)){
            DBG("Locked");
            locked = TRUE;
            break;
        }
        usleep(100);
    }
    if(!locked) WARNX(_("Can't lock shared memory"));
    return locked;
}

static int getshmimage(int *shmlocked){
    if(!shmlocked || !shmima) return FALSE;
    if(!refresh_shm()) return FALSE;
    if(!*shmlocked && !(*shmlocked = lockshm())) return FALSE;
    DBG("Server's imno: %zd, bytelen: %zd", shmima->imnumber, shmima->bytelen);
    int ret = cc_copyimage(locima, shmima, TRUE);
    TIMESTAMP("Got by shared memory");
    return ret;
}

static int getsocksizes(int *imsock){
    if(!imsock || *imsock > -1) return FALSE;
    DBG("Open socket @ %s", GP->imageport);
    *imsock = cc_open_socket(FALSE, GP->imageport, TRUE);
    if(*imsock < 0){
        WARNX(_("Can't open image transport socket"));
        return FALSE;
    }
    // get image size
    cc_IMG ima;
    if(!readNbytes(*imsock, sizeof(cc_IMG), (uint8_t*)&ima)){
        WARNX(_("Can't read image header over socket"));
        return FALSE;
    }
    if(ima.MAGICK != CC_SHM_MAGIC || ima.bytelen < 1) return FALSE;
    // now copy fields
#define COPY(field) locima->field = ima.field;
    COPY(timestamp);
    COPY(bitpix);
    COPY(w);
    COPY(h);
    COPY(bytelen);
    COPY(imnumber);
#undef COPY
    return TRUE;
}

static int getsockimage(int *imsock){
    if(!imsock || *imsock < 0) return FALSE;
    pthread_mutex_lock(&locima->mutex);
    if(locima->datasize < locima->bytelen){
        size_t newsz = 1024 * (1 + locima->bytelen / 1024);
        void *nxt = realloc(locima->data, newsz);
        if(!nxt){
            LOGERR("realloc() failed");
            WARN("realloc()");
            pthread_mutex_unlock(&locima->mutex);
            return FALSE;
        }
        locima->data = nxt;
    }
    int ok = readNbytes(*imsock, locima->bytelen, locima->data);
    pthread_mutex_unlock(&locima->mutex);
    if(!ok){
        WARNX(_("Can't read image data"));
        return FALSE;
    }
    TIMESTAMP("Got by socket");
    return TRUE;
}

/**
 * @brief getimage - read image from shared memory or socket
 * @param askheader == TRUE for storing FITS-header
 * @return FALSE if failed to catch image
 */
static int getimage(/*int askheader*/){
    FNAME();
    int imsock = -1, shmlocked = FALSE, ret = FALSE;
    static double oldtimestamp = -1.;
    TIMESTAMP("Get image sizes (or full image over SHM)");
    if(!locima){
        locima = cc_newimage(16, 1024, 1024);
        if(!locima){
            WARN("calloc()"); return FALSE;
        }
    }
    ret = getshmimage(&shmlocked);
    if(!ret){ // can't get by shm -> try over NET
        DBG("Try to get image over network");
        if(!getsocksizes(&imsock)) goto eofg;
        TIMESTAMP("Start of data read");
        ret = getsockimage(&imsock);
        if(!ret){
            WARNX(_("Can't read image data"));
            goto eofg;
        }
    }
    DBG("timestamps new-old=%g; imno: %zd", locima->timestamp - oldtimestamp, locima->imnumber);
    if(locima->timestamp != oldtimestamp){ // test if image is really new
        oldtimestamp = locima->timestamp;
        atomic_store(&grabno, locima->imnumber);
        TIMESTAMP("Got image #%zd", locima->imnumber);
#if 0
        if(askheader){ // read FITS-header for later saving
            if(shmima){
                if(locima->headerstrings > FITS_HEADER_STRINGS_MAX){
                    WARNX(_("Too many FITS headers, truncating"));
                    locima->headerstrings = FITS_HEADER_STRINGS_MAX;
                }
                size_t rsz = FLEN_CARD * locima->headerstrings;
                memcpy(locima->fitsheader, shmima->fitsheader, rsz);
                cc_unlock_shm();
                shmlocked = FALSE;
            }else{
                uint8_t card[FLEN_CARD];
                for(size_t i = 0; i < locima->headerstrings; ++i){
                    if(!readNbytes(imsock, FLEN_CARD, card)){
                        WARNX(_("Can't read full header, got %zd records from %zd"), i, locima->headerstrings);
                        break;
                    }
                    memcpy(&locima->fitsheader[i], card, FLEN_CARD);
                }
                close(imsock);
                imsock = -1;
            }
            if(GP->addhdr){ // add records from client-side files
                char **nxtfile = GP->addhdr;
                while(*nxtfile){
                    cc_kwfromfile(locima, *(nxtfile++));
                }
            }
        }else locima->headerstrings = 0;
#endif
    }else WARNX(_("Still got old image"));
eofg:
    if(imsock > -1) close(imsock); // reopen in next time in case of error
    if(shmlocked) cc_unlock_shm();
    return ret;
}

// get number of current image
static int curImNo(int sock){
    int N = -1;
    if(shmima){
        if(lockshm()){
            N = shmima->imnumber;
            cc_unlock_shm();
            atomic_store(&current_image_number, N);
        }
    }
    if(N < 0){ // no shared memory: try to get number over TCP
        SENDCMDW(CC_CMD_IMNUMBER);
        N = atomic_load(&current_image_number);
    }
    return N;
}

void client(int sock){
    if(sock < 0) ERRX(_("Can't run without command socket"));
    if(!GP->forceimsock) refresh_shm(); // init shm buffer if user don't ask to force workign through image socket
    if(GP->restart){
        SENDCMDW(CC_CMD_RESTART);
        return;
    }
    TIMEINIT();
    send_headers(sock);
    TIMESTAMP("Got headers");
    double t0 = sl_dtime(), tw, tstart;
    int Nremain = 0, nframe = 1;
    atomic_store(&expstate, CAMERA_IDLE); // could be changed earlier
    // if client gives filename/prefix or Nframes, make exposition
    if((GP->outfile && *GP->outfile) || (GP->outfileprefix && *GP->outfileprefix) || GP->nframes > 0){
        Nremain = GP->nframes;
        if(Nremain < 1) Nremain = 1;
        SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE); // call to start capture
    } else return; // just send headers and exit
    double timeout = CC_CLIENT_TIMEOUT;
    verbose(VERBOSE_PRIMARY, _("Exposing frame 1..."));
    DBG("Current state: %d", atomic_load(&expstate));
    verbose(VERBOSE_PRIMARY, _("Wait for exposition end"));
    t0 = sl_dtime();
    tw = tstart = t0;
    int lastImNo = curImNo(sock);
    while(sl_dtime() - t0 < timeout){
        if(sl_dtime() - tw > CC_WAIT_TIMEOUT){
            SENDCMDW(CC_CMD_TREMAIN); // get remained time
            SENDCMDW(CC_CMD_EXPSTATE);
            tw = sl_dtime();
            usleep(100000);
        }
        if(sl_dtime() - tstart < GP->exptime){
            t0 = sl_dtime(); // refresh timeout until exp not ends
            usleep(1000);
            continue;
        }
        int curst = atomic_load(&expstate);
        DBG("Current state: %d", curst);
        if(curst == CAMERA_ERROR){
            WARNX(_("Can't make exposition"));
            if(Nremain > 1){
                verbose(VERBOSE_PRIMARY, _("Exposing frame %d..."), nframe);
                SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE);
                tstart = sl_dtime();
            }
            continue;
        }
        int cur = curImNo(sock);
        if(curst == CAMERA_FRAMERDY || cur != lastImNo){
            atomic_store(&expstate, CAMERA_IDLE);
            DBG("Current imno: %d", cur);
            if(Nremain > 1){ // start next capture
                verbose(VERBOSE_PRIMARY, _("Exposing frame %d..."), nframe);
                SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE);
                tstart = sl_dtime();
            }
            int failed = TRUE;
            if(lastImNo != cur){
                lastImNo = cur;
                verbose(VERBOSE_SECONDARY, _("Frame ready, try to grab"));
                if(!getimage(/*TRUE*/)){
                    WARNX(_("Can't get next image"));
                }else{
                    if(saveFITS(locima, &lastfilename)){
                        --Nremain;
                        ++nframe;
                        failed = FALSE;
                    }
                }
            }else verbose(VERBOSE_SECONDARY, _("Got already saved image, wait next"));
            if(failed && Nremain == 1){ // last image -> should re-expose
                verbose(VERBOSE_PRIMARY, _("Exposing frame %d..."), nframe);
                SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE);
                tstart = sl_dtime();
            }
            if(Nremain > 0){
                if(GP->pause_len > 0){
                    double delta, time1 = sl_dtime() + GP->pause_len;
                    while(1){
                        SENDCMDW(CC_CMD_CAMTEMPER);
                        if((delta = time1 - sl_dtime()) < __FLT_EPSILON__) break;
                        if(delta > 1.) verbose(VERBOSE_PRIMARY, _("%d seconds till pause ends\n"), (int)delta);
                        if(delta > 6.) sleep(5);
                        else if(delta > 1.) sleep((int)delta);
                        else usleep((int)(delta*1e6 + 1));
                    }
                }
            }else{
                verbose(VERBOSE_SECONDARY, "Got all images, closing...");
                break;
            }
        }
    }
    if(Nremain > 0) WARNX(_("Server timeout"));
}

#ifdef IMAGEVIEW
static int controlfd = -1; // control socket FD
void init_grab_sock(int sock){
    if(sock < 0) ERRX(_("Can't run without command socket"));
    controlfd = sock;
    send_headers(sock);
    if(!GP->forceimsock && !shmima){ // init shm buffer if user don't ask to work through image socket
        shmima = cc_getshm(GP->shmkey, 0); // try to init client shm
        cc_init_sem(FALSE);
    }
}

// grab image for active viewer
static void *grabnext(void _U_ *arg){
    FNAME();
    if(controlfd < 0) return NULL;
    int sock = controlfd;
    int lastImNo = curImNo(sock);
    while(1){
        if(!getWin()) exit(1);
        TIMESTAMP("Start of capturing cycle");
        atomic_store(&expstate, CAMERA_IDLE);
        TIMEINIT();
        SENDMSGW(CC_CMD_EXPSTATE, "=%d", CAMERA_CAPTURE); // start capture
        double timeout = GP->exptime + CC_CLIENT_TIMEOUT, t0 = sl_dtime();
        useconds_t sleept = 500000; // 0.5s
        if(GP->exptime < 0.5){
            sleept = (useconds_t)(GP->exptime * 250000.); // a quater of exposition time
            if(sleept < CC_IMWAIT_SLEEP) sleept = CC_IMWAIT_SLEEP;
        }
        TIMESTAMP("Wait for exposition ends (%g s), sleep for %dus", timeout, sleept);
        int curst = CAMERA_CAPTURE;
        int cur = lastImNo;
        SENDCMDW(CC_CMD_EXPSTATE);
        while(sl_dtime() - t0 < timeout){
            DBG("start sleep for %dus", sleept);
            usleep(sleept);
            cur = curImNo(sock);
            curst = atomic_load(&expstate);
            if(curst != CAMERA_CAPTURE || cur != lastImNo) break;
            if(sl_dtime() - t0 > GP->exptime && sleept != CC_IMWAIT_SLEEP){
                sleept = CC_IMWAIT_SLEEP;
                DBG("Set sleeping time to %dus", sleept);
                SENDCMDW(CC_CMD_EXPSTATE);
            }
        }
        SENDCMDW(CC_CMD_EXPSTATE);
        curst = atomic_load(&expstate);
        if(curst == CAMERA_ERROR) ERRX(_("Camera in error state"));
        if(sl_dtime() - t0 >= timeout && cur != lastImNo){
            WARNX(_("Image wasn't received, state: %d, waiting: %g, lastNo: %d, curNo: %d (timeout: %g)"), curst, sl_dtime() - t0, lastImNo, cur, timeout);
            continue;
        }
        lastImNo = cur;
        TIMESTAMP("Frame ready (%d from start)", cur);
        getimage(/*FALSE*/);
    }
    return NULL;
}

// passive waiting for next image
static void *waitimage(void _U_ *arg){
    FNAME();
    if(controlfd < 0) return NULL;
    int sock = controlfd;
    int lastImNo = curImNo(sock);
    double t0 = sl_dtime();
    while(1){
        if(!getWin()) exit(1);
        double tcur = sl_dtime();
        if(tcur - t0 >= CC_WAIT_TIMEOUT){
            SENDCMDW(CC_CMD_EXPSTATE); // `ping` server to know if it disconnected
            t0 = tcur;
        }
        int cur = curImNo(sock);
        if(cur == lastImNo){
            usleep(CC_IMWAIT_SLEEP);
            continue;
        }
        lastImNo = cur;
        TIMESTAMP("End of cycle #%d, start new", cur);
        TIMEINIT();
        getimage(/*FALSE*/);
        atomic_store(&expstate, CAMERA_IDLE);
        DBG("Current state: %d", atomic_load(&expstate));
    }
    return NULL;
}

// try to capture images for viewer
int sockcaptured(cc_IMG *imgptr){
    //TIMESTAMP("sockcaptured() start");
    if(!imgptr) return FALSE;
    static pthread_t grabthread = 0;
    if(controlfd < 0) return FALSE;
    if(imgptr == (void*)-1){ // kill `grabnext`
        DBG("Wait for grabbing thread");
        if(grabthread){
            pthread_cancel(grabthread);
            pthread_join(grabthread, NULL);
            grabthread = 0;
        }
        DBG("OK");
        return FALSE;
    }
    if(!grabthread || pthread_kill(grabthread, 0)){ // start new grab
        if(GP->viewer){
            TIMEINIT();
            DBG("\n\n\nStart new capturing");
            if(pthread_create(&grabthread, NULL, &waitimage, NULL)){
                WARN(_("Can't create capturing thread"));
                grabthread = 0;
           }
        }else{
            TIMEINIT();
            DBG("\n\n\nStart new grab");
            if(pthread_create(&grabthread, NULL, &grabnext, NULL)){
                WARN(_("Can't create capturing thread"));
                grabthread = 0;
           }
        }
    }else{ // grab in process
        int curno = atomic_load(&grabno);
        if(curno != oldgrabno){ // image is ready
            TIMESTAMP("Image #%d ready (old was %d)", curno, oldgrabno);
            oldgrabno = curno;
            framerate();
            //TIMESTAMP("sockcaptured() end, return TRUE");
            int ret = cc_copyimage(imgptr, locima, FALSE);
            return ret;
        }
    }
    //TIMESTAMP("sockcaptured() end, return FALSE");
    return FALSE;
}
// IMAGEVIEW
#endif
