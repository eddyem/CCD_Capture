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

#include <ctype.h> // isspace
#include <dlfcn.h>  // dlopen/close
#include <fcntl.h>
#include <float.h> // for float max
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/un.h>  // unix socket
#include <unistd.h>
#include <usefull_macros.h>

#include "client.h"
#include "cmdlnopts.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif
#include "server.h"
#include "socket.h"

#ifdef EBUG
double __t0 = 0.;
#endif

static int ntries = 2;  // amount of tries to send messages controlling the answer
double answer_timeout = 0.1; // timeout of waiting answer from server (not static for client.c)

/**
 * @brief cc_open_socket - create socket and open it
 * @param isserver  - TRUE for server, FALSE for client
 * @param path      - UNIX-socket path or local INET socket port
 * @param isnet     - 1/2 for INET socket (1 - localhost, 2 - network), 0 for UNIX
 * @return socket FD or -1 if failed
 */
int cc_open_socket(int isserver, char *path, int isnet){
    DBG("isserver=%d, path=%s, isnet=%d", isserver, path, isnet);
    if(!path) return 1;
    //DBG("path/port: %s", path);
    int sock = -1;
    struct addrinfo hints = {0}, *res;
    struct sockaddr_un unaddr = {0};
    if(isnet){
        //DBG("Network socket");
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        const char *node = (isnet == 2) ? NULL : "127.0.0.1";
        if(getaddrinfo(node, path, &hints, &res) != 0){
            WARN("getaddrinfo");
            return -1;
        }
    }else{
        DBG("UNIX socket");
        char apath[128];
        if(*path == 0){
            DBG("convert name");
            apath[0] = 0;
            strncpy(apath+1, path+1, 126);
        }else if(strncmp("\\0", path, 2) == 0){
            DBG("convert name");
            apath[0] = 0;
            strncpy(apath+1, path+2, 126);
        }else strcpy(apath, path);
        //unlink(apath);
        unaddr.sun_family = AF_UNIX;
        hints.ai_addr = (struct sockaddr*) &unaddr;
        hints.ai_addrlen = sizeof(unaddr);
        memcpy(unaddr.sun_path, apath, 106); // if sun_path[0] == 0 we don't create a file
        hints.ai_family = AF_UNIX;
        hints.ai_socktype = SOCK_SEQPACKET;
        res = &hints;
    }
    for(struct addrinfo *p = res; p; p = p->ai_next){
        if((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0){ // or SOCK_STREAM?
            LOGWARN("socket()");
            WARN("socket()");
            continue;
        }
        if(isserver){
            int reuseaddr = 1;
            if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) == -1){
                WARN("setsockopt()");
                LOGWARN("setsockopt()");
                close(sock); sock = -1;
                continue;
            }
            //fcntl(sock, F_SETFL, O_NONBLOCK);
            if(bind(sock, p->ai_addr, p->ai_addrlen) == -1){
                WARN("bind()");
                LOGWARN("bind()");
                close(sock); sock = -1;
                continue;
            }
            /*
            int enable = 1;
            if(ioctl(sock, FIONBIO, (void *)&enable) < 0){ // make socket nonblocking
                WARN("ioctl()");
                LOGWARN("Can't make socket nonblocking");
            }
            */
        }else{
            if(connect(sock, p->ai_addr, p->ai_addrlen) == -1){
                WARN("connect()");
                LOGWARN("connect()");
                close(sock); sock = -1;
            }
        }
        break;
    }
    if(isnet) freeaddrinfo(res);
    return sock;
}

// send data through the socket
int cc_senddata(int fd, void *data, size_t l){
    DBG("fd=%d, l=%zd", fd, l);
    if(fd < 1 || !data || l < 1) return TRUE; // empty message
    DBG("send new data (size=%zd) to fd %d", l, fd);
//strncpy((char*)data, "TEST image data\n", 17);
//l = 16;
    if(l != (size_t)send(fd, data, l, MSG_NOSIGNAL)){
        WARN("write()");
        LOGWARN("write()");
        return FALSE;
    }
    DBG("success");
    if(globlog)  LOGDBG("SEND data (size=%d) to fd %d", l, fd);
    return TRUE;
}

// simple wrapper over write: add missed newline and log data
int cc_sendmessage(int fd, const char *msg, int l){
    if(fd < 1 || !msg || l < 1) return TRUE; // empty message
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // thread safe
    pthread_mutex_lock(&mutex);
    static char *tmpbuf = NULL;
    static int buflen = 0;
    if(l + 1 > buflen){
        buflen = 1024 * (1 + l/1024);
        tmpbuf = realloc(tmpbuf, buflen);
    }
    DBG("send to fd %d:\n%s[%d]", fd, msg, l);
    memcpy(tmpbuf, msg, l);
    if(msg[l-1] != '\n') tmpbuf[l++] = '\n';
    if(l != send(fd, tmpbuf, l, MSG_NOSIGNAL)){
        WARN("write()");
        LOGWARN("write()");
        pthread_mutex_unlock(&mutex);
        return FALSE;
    }else{
        //DBG("success");
        if(globlog){ // logging turned ON
            tmpbuf[l-1] = 0; // remove trailing '\n' for logging
            LOGDBG("SEND '%s'", tmpbuf);
        }
    }
    pthread_mutex_unlock(&mutex);
    return TRUE;
}
int cc_sendstrmessage(int fd, const char *msg){
    if(fd < 1 || !msg) return TRUE; // empty message
    int l = strlen(msg);
    return cc_sendmessage(fd, msg, l);
}

// text messages for `cc_hresult`
// WARNING! You should initialize ABSOLUTELY ALL members of `cc_hresult` or some pointers would give segfault
static const char *resmessages[RESULT_NUM] = {
    [RESULT_OK] = "OK",
    [RESULT_BUSY] = "BUSY",
    [RESULT_FAIL] = "FAIL",
    [RESULT_BADVAL] = "BADVAL",
    [RESULT_BADKEY] = "BADKEY",
};

const char *cc_hresult2str(cc_hresult r){
    if(r < 0 || r >= RESULT_NUM) return "BADRESULT";
    return resmessages[r];
}

cc_hresult cc_str2hresult(const char *str){
    for(cc_hresult res = 0; res < RESULT_NUM; ++res){
        if(!resmessages[res]) continue;
        if(0 == strcmp(resmessages[res], str)) return res;
    }
    return RESULT_NUM; // didn't find
}

/**
 * @brief cc_get_keyval - get value of `key = val`
 * @param keyval (io) - pair `key = val`, return `key`
 * @return `val`
 */
char *cc_get_keyval(char **keyval){
    DBG("Got string %s", *keyval);
    // remove starting spaces in key
    while(isspace(**keyval)) ++(*keyval);
    char *val = strchr(*keyval, '=');
    if(val){ // got value: remove starting spaces in val
        *val++ = 0;
        while(isspace(*val)) ++val;
    }
    DBG("val = %s (%zd bytes)", val, (val)?strlen(val):0);
    // remove trailing spaces in key
    char *e = *keyval + strlen(*keyval) - 1; // last key symbol
    while(isspace(*e) && e > *keyval) --e;
    e[1] = 0;
    // now we have key (`str`) and val (or NULL)
    //DBG("key=%s, val=%s", keyval, val);
    return val;
}

/**
 * check data from  fd (polling function for client)
 * @param fd - file descriptor
 * @return 0 in case of timeout, 1 in case of fd have data, -1 if error
 */
int cc_canberead(int fd){
    fd_set fds;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    do{
        int rc = select(fd+1, &fds, NULL, NULL, &timeout);
        if(rc < 0){
            if(errno != EINTR){
                LOGWARN("select()");
                WARN("select()");
                return -1;
            }
            continue;
        }
        break;
    }while(1);
    if(FD_ISSET(fd, &fds)){
        return 1;
    }
    return 0;
}

/**
 * @brief cc_getshm - get shared memory segment for image
 * @param imsize - size of image data (in bytes): if !=0 allocate as server, else - as client (readonly)
 * @return pointer to shared memory region or NULL if failed
 */
cc_IMG *cc_getshm(key_t key, size_t imsize){
    size_t shmsize = sizeof(cc_IMG) + imsize;
    shmsize = 1024 * (1 + shmsize / 1024);
    DBG("Allocate %zd bytes in shared memory", shmsize);
    int shmid = -1;
    int flags = (imsize) ? IPC_CREAT | 0666 : 0;
    shmid = shmget(key, 0, flags);
    if(shmid < 0 && imsize == 0){ // no SHM segment for client
        WARN("Can't get shared memory segment %d", key);
        return NULL;
    }
    if(imsize){ // check if segment exists and its size equal to needs
        struct shmid_ds buf;
        if(shmctl(shmid, IPC_STAT, &buf) > -1 && shmsize != buf.shm_segsz){ // remove already existing segment
            DBG("Need to remove already existing segment");
            shmctl(shmid, IPC_RMID, NULL);
        }
        shmid = shmget(key, shmsize, flags);
        if(shmid < 0){
            WARN("Can't create shared memory segment %d", key);
            return NULL;
        }
    }
    flags = (imsize) ? 0 : SHM_RDONLY; // client opens memory in readonly mode
    cc_IMG *ptr = shmat(shmid, NULL, flags);
    if(ptr == (void*)-1){
        if(imsize) WARN("Can't attach SHM segment %d", key);
        return NULL;
    }
    if(!imsize){
        if(ptr->MAGICK != CC_SHM_MAGIC){
            WARNX("Shared memory %d isn't belongs to image server", key);
            shmdt(ptr);
            return NULL;
        }
        return ptr;
    }
    bzero(ptr, sizeof(cc_IMG));
    ptr->data = (void*)((uint8_t*)ptr + sizeof(cc_IMG));
    ptr->MAGICK = CC_SHM_MAGIC;
    return ptr;
}


// find plugin
static void *open_plugin(const char *name){
    DBG("try to open lib %s", name);
    void* dlh = dlopen(name, RTLD_NOLOAD); // library may be already opened
    if(!dlh){
        DBG("Not loaded - load");
        dlh = dlopen(name, RTLD_NOW);
    }
    if(!dlh){
        WARNX(_("Can't find plugin %s: %s"), name, dlerror());
        return NULL;
    }
    return dlh;
}

cc_Focuser *open_focuser(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    cc_Focuser* f = (cc_Focuser*) dlsym(dlh, "focuser");
    if(!f){
        WARNX(_("Can't find focuser in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return f;
}
cc_Camera *open_camera(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    cc_Camera *c = (cc_Camera*) dlsym(dlh, "camera");
    if(!c){
        WARNX(_("Can't find camera in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return c;
}
cc_Wheel *open_wheel(const char *pluginname){
    FNAME();
    void* dlh = open_plugin(pluginname);
    if(!dlh) return NULL;
    cc_Wheel *w = (cc_Wheel*) dlsym(dlh, "wheel");
    if(!w){
        WARNX(_("Can't find wheel in plugin %s: %s"), pluginname, dlerror());
        return NULL;
    }
    return w;
}

/**
 * @brief cc_getNbytes - calculate amount of bytes to store bitpix (1/2)
 * @param image - image
 * @return 1 for bitpix<8 or 2
 */
int cc_getNbytes(cc_IMG *image){
    int n = (image->bitpix + 7) / 8;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    return n;
}

/**
 * @brief cc_strbufnew - allocate new buffer
 * @param bufsize - size of full socket buffer
 * @param stringsize - max length of string from buffer (excluding \0)
 * @return allocated buffer
 */
cc_strbuff *cc_strbufnew(size_t bufsize, size_t stringsize){
    if(bufsize < 8 || stringsize < 8){
        WARNX("Need to allocate at least 8 bytes in buffers");
        return NULL;
    }
    DBG("Allocate new string buffer with size %zd and string size %zd", bufsize, stringsize);
    cc_strbuff *b = MALLOC(cc_strbuff, 1);
    b->bufsize = bufsize;
    b->buf = MALLOC(char, bufsize);
    b->string = MALLOC(char, stringsize + 1); // for terminated zero
    b->strlen = stringsize;
    return b;
}
void cc_strbufdel(cc_strbuff **buf){
    FREE((*buf)->buf);
    FREE((*buf)->string);
    FREE(*buf);
}

cc_charbuff *cc_charbufnew(){
    DBG("Allocate new char buffer with size %d", BUFSIZ);
    cc_charbuff *b = MALLOC(cc_charbuff, 1);
    b->bufsize = BUFSIZ;
    b->buf = MALLOC(char, BUFSIZ);
    return b;
}
// set buflen to 0
void cc_charbufclr(cc_charbuff *buf){
    if(!buf) return;
    buf->buflen = 0;
}
// put `l` bytes of `s` to b->buf and add terminated zero
void cc_charbufput(cc_charbuff *b, const char *s, size_t l){
    if(!cc_charbuftest(b, l+1)) return;
    DBG("add %zd bytes to buff", l);
    memcpy(b->buf + b->buflen, s, l);
    b->buflen += l;
    b->buf[b->buflen] = 0;
}
void cc_charbufaddline(cc_charbuff *b, const char *s){
    if(!b || !s) return;
    size_t l = strlen(s);
    if(l < 1 || !cc_charbuftest(b, l+2)) return;
    cc_charbufput(b, s, l);
    if(s[l-1] != '\n'){ // add trailing '\n'
        b->buf[b->buflen++] = '\n';
        b->buf[b->buflen] = 0;
    }
}
// realloc buffer if its free size less than maxsize
int cc_charbuftest(cc_charbuff *b, size_t maxsize){
    if(!b) return FALSE;
    if(b->bufsize - b->buflen > maxsize + 1) return TRUE;
    size_t newblks = (maxsize + BUFSIZ) / BUFSIZ;
    b->bufsize += BUFSIZ * newblks;
    DBG("Realloc charbuf to %zd", b->bufsize);
    b->buf = realloc(b->buf, b->bufsize);
    return TRUE;
}
void cc_charbufdel(cc_charbuff **buf){
    FREE((*buf)->buf);
    FREE(*buf);
}


/**
 * @brief cc_read2buf - try to read next data portion from POLLED socket
 * @param fd - socket fd to read from
 * @param buf - buffer to read
 * @return FALSE in case of buffer overflow or client disconnect, TRUE if got 0..n bytes of data
 */
int cc_read2buf(int fd, cc_strbuff *buf){
    int ret = FALSE;
    if(!buf) return FALSE;
    pthread_mutex_lock(&buf->mutex);
    if(!buf->buf || buf->buflen >= buf->bufsize) goto ret;
    size_t maxlen = buf->bufsize - buf->buflen;
    ssize_t rd = read(fd, buf->buf + buf->buflen, maxlen);
    if(rd <= 0) goto ret;
    DBG("got %zd bytes", rd);
    if(rd) buf->buflen += rd;
    ret = TRUE;
ret:
    pthread_mutex_unlock(&buf->mutex);
    return ret;
}

/**
 * @brief cc_refreshbuf - same as cc_read2buf, but with polling
 * @param fd - socket fd
 * @param buf - buffer
 * @return TRUE if got data
 */
int cc_refreshbuf(int fd, cc_strbuff *buf){
    if(!cc_canberead(fd)) return FALSE;
    return cc_read2buf(fd, buf);
}

/**
 * @brief cc_getline - read '\n'-terminated string from `b` and substitute '\n' by 0
 * @param b - input charbuf
 * @param len - length of `str` (including terminating zero)
 * @return amount of bytes read (idx > b->strlen in case of string buffer overflow)
 */
size_t cc_getline(cc_strbuff *b){
    if(!b) return 0;
    size_t idx = 0;
    pthread_mutex_lock(&b->mutex);
    if(!b->buf || !b->string) goto ret;
    char *ptr = b->buf;
    for(; idx < b->buflen; ++idx) if(*ptr++ == '\n') break;
    if(idx == b->buflen){
        idx = 0; // didn't fount '\n'
        goto ret;
    }
    size_t minlen = (b->strlen > idx) ? idx : b->strlen; // prevent `str` overflow
    memcpy(b->string, b->buf, minlen);
    b->string[minlen] = 0;
    if(++idx < b->buflen){ // move rest of data in buffer to beginning
        memmove(b->buf, b->buf+idx, b->buflen-idx);
        b->buflen -= idx;
    }else b->buflen = 0;
    DBG("got string `%s`", b->string);
ret:
    pthread_mutex_unlock(&b->mutex);
    return idx;
}


/**
 * @brief cc_setNtries, cc_getNtries - ntries setter and getter
 * @param n - new amount of tries
 * @return cc_setNtries returns TRUE if succeed, cc_getNtries returns current ntries value
 */
int cc_setNtries(int n){
    if(n > 1000 || n < 1) return FALSE;
    ntries = n;
    return TRUE;
}
int cc_getNtries(){return ntries;}

/**
 * @brief cc_setAnsTmout, cc_getAnsTmout - answer timeout setter/getter
 * @param t - timeout, s (not less than 0.001s)
 * @return true/timeout
 */
int cc_setAnsTmout(double t){
    if(t < 0.001) return FALSE;
    answer_timeout = t;
    return TRUE;
}
double cc_getAnsTmout(){return answer_timeout;}


/**
 * @brief ask4cmd - send string `cmdwargs` like "par=val"
 * @param fd - fd of socket
 * @param buf - buffer to store data read from socket
 * @param cmdwargs (i) - "par=val"
 * @return RESULT_OK if got same string or other error
 */
static cc_hresult ask4cmd(int fd, cc_strbuff *buf, const char *cmdwargs){
    DBG("ask for command %s", cmdwargs);
    char *keyptr = strdup(cmdwargs), *key = keyptr;
    cc_get_keyval(&key); // pick out key from `cmdwargs`
    int l = strlen(key);
    cc_hresult ret = RESULT_FAIL;
    for(int i = 0; i < ntries; ++i){
        DBG("Try %d time", i+1);
        if(!cc_sendstrmessage(fd, cmdwargs)) continue;
        double t0 = dtime();
        while(dtime() - t0 < answer_timeout){
            int r = cc_canberead(fd);
            if(r == 0) continue;
            else if(r < 0){
                LOGERR("Socket disconnected");
                WARNX("Socket disconnected");
                ret = RESULT_DISCONNECTED;
                goto rtn;
            }
            while(cc_refreshbuf(fd, buf));
            DBG("read");
            size_t got = 0;
            while((got = cc_getline(buf))){
                if(got >= BUFSIZ){
                    DBG("Client fd=%d gave buffer overflow", fd);
                    LOGMSG("SERVER client fd=%d buffer overflow", fd);
                }else if(got){
                    if(strncmp(buf->string, key, l) == 0){
                        ret = RESULT_OK;
                        goto rtn;
                    }else{ // answers like 'OK' etc
                        cc_hresult r = cc_str2hresult(buf->string);
                        if(r != RESULT_NUM){ // some other data
                            ret = r;
                            goto rtn;
                        }
                    }
                }
                cc_refreshbuf(fd, buf);
            }
        }
    }
rtn:
    DBG("returned with `%s`", cc_hresult2str(ret));
    FREE(keyptr);
    return ret;
}

#define BBUFS   (63)

/**
 * @brief cc_setint - send integer value over socket
 * @param fd - socket fd
 * @param cmd - setter
 * @param val - new value
 * @return answer received
 */
cc_hresult cc_setint(int fd, cc_strbuff *cbuf, const char *cmd, int val){
    char buf[BBUFS+1];
    snprintf(buf, BBUFS, "%s=%d\n", cmd, val);
    return ask4cmd(fd, cbuf, buf);
}
/**
 * @brief cc_getint - getter for integer value
 */
cc_hresult cc_getint(int fd, cc_strbuff *cbuf, const char *cmd, int *val){
    char buf[BBUFS+1];
    snprintf(buf, BBUFS, "%s\n", cmd);
    cc_hresult r = ask4cmd(fd, cbuf, buf);
    if(r == RESULT_OK){
        char *p = cbuf->string;
        char *sv = cc_get_keyval(&p);
        if(!sv) return RESULT_FAIL;
        char *ep;
        long L = strtol(sv, &ep, 0);
        if(sv == ep || L < INT_MIN || L > INT_MAX) return RESULT_BADVAL;
        if(val) *val = (int) L;
    }
    return r;
}

/**
 * @brief cc_setfloat - send float value over socket
 * @param fd - socket fd
 * @param cmd - setter
 * @param val - new value
 * @return answer received
 */
cc_hresult cc_setfloat(int fd, cc_strbuff *cbuf, const char *cmd, float val){
    char buf[BBUFS+1];
    snprintf(buf, BBUFS, "%s=%g\n", cmd, val);
    return ask4cmd(fd, cbuf, buf);
}
/**
 * @brief cc_getfloat - getter for float value
 */
cc_hresult cc_getfloat(int fd, cc_strbuff *cbuf, const char *cmd, float *val){
    char buf[BBUFS+1];
    snprintf(buf, BBUFS, "%s\n", cmd);
    cc_hresult r = ask4cmd(fd, cbuf, buf);
    if(r == RESULT_OK){
        char *p = cbuf->string;
        char *sv = cc_get_keyval(&p);
        if(!sv) return RESULT_FAIL;
        char *ep;
        double d = strtod(sv, &ep);
        if(sv == ep || d < (-FLT_MAX) || d > FLT_MAX) return RESULT_BADVAL;
        if(val) *val = (float)d;
    }
    return r;
}


// get next record from external buffer, newlines==1 if every record ends with '\n'
char *cc_nextkw(char *buf, char record[FLEN_CARD+1], int newlines){
    char *nextline = NULL;
    int l = FLEN_CARD - 1;
    if(newlines){
        char *e = strchr(buf, '\n');
        if(e){
            if(e - buf < FLEN_CARD) l = e - buf;
            nextline = e + 1;
        }
    }else nextline = buf + (FLEN_CARD - 1);
    strncpy(record, buf, l);
    record[l] = 0;
    return nextline;
}

/**
 * @brief cc_kwfromfile - add records from file
 * @param b - buffer to add
 * @param filename - file name with FITS headers ('\n'-terminated or by 80 chars)
 * @return amount of bytes added
 */
size_t cc_kwfromfile(cc_charbuff *b, char *filename){
    if(!b) return 0;
    mmapbuf *buf = My_mmap(filename);
    if(!buf || buf->len < 1){
        WARNX("Can't add FITS records from file %s", filename);
        LOGWARN("Can't add FITS records from file %s", filename);
        return 0;
    }
    size_t blen0 = b->buflen;
    char rec[FLEN_CARD+1], card[FLEN_CARD+1];
    char *data = buf->data, *x = strchr(data, '\n'), *eodata = buf->data + buf->len;
    int newlines = 0;
    if(x && (x - data) < FLEN_CARD){ // we found newline -> this is a format with newlines
        newlines = 1;
    }
    do{
        data = cc_nextkw(data, rec, newlines);
        if(data > eodata) break;
        int status = 0, kt = 0;
        fits_parse_template(rec, card, &kt, &status);
        if(status) fits_report_error(stderr, status);
        else cc_charbufaddline(b, card);
    }while(data && *data);
    My_munmap(buf);
    return b->buflen - blen0;
}

/**
 * @brief cc_charbuf2kw - write all keywords from `b` to file `f`
 * `b` should be prepared by cc_kwfromfile or similar
 * @param b - buffer with '\n'-separated FITS keys
 * @param f - file to write
 * @return FALSE if failed
 */
int cc_charbuf2kw(cc_charbuff *b, fitsfile *f){
    if(!b || !f || !b->buflen) return FALSE;
    char key[FLEN_KEYWORD + 1], card[FLEN_CARD+1]; // keyword to update
    char *c = b->buf, *eof = b->buf + b->buflen;
    while(c < eof){
        char *e = strchr(c, '\n');
        if(!e || e > eof) break;
        //char *ek = strchr(c, ' ');
        char *eq = strchr(c, '=');
        //if(eq > ek) eq = ek; // if keyword is shorter than 8 letters
        //size_t l = eq-c; if(l > FLEN_KEYWORD) l = FLEN_KEYWORD;
        //memcpy(key, c, l); key[l] = 0;
        memcpy(key, c, 8); key[8] = 0;
        size_t l = e - c; if(l > FLEN_CARD) l = FLEN_CARD;
        memcpy(card, c, l); card[l] = 0;
        int status = 0;
        if(eq - c == 8){ // key = val
            DBG("Update key `%s` with `%s`", key, card);
            fits_update_card(f, key, card, &status);
        }else{ // comment etc
            DBG("Write full record `%s`", card);
            fits_write_record(f, card, &status);
        }
        if(status) fits_report_error(stderr, status);
        c = e + 1;
    }
    return TRUE;
}

static size_t print_val(cc_partype_t t, void *val, char *buf, size_t bufl){
    size_t l = 0;
    switch(t){
        case CC_PAR_INT:
            l = snprintf(buf, bufl, "%d", *(int*)val);
        break;
        case CC_PAR_FLOAT:
            l = snprintf(buf, bufl, "%g", *(float*)val);
        break;
        case CC_PAR_DOUBLE:
            l = snprintf(buf, bufl, "%g", *(double*)val);
        break;
        default:
         l = snprintf(buf, bufl, "hoojnya");
        break;
    }
    return l;
}

/**
 * @brief cc_plugin_customcmd - common handler for custom plugin commands
 * @param str - string like "par" (getter/cmd) or "par=val" (setter)
 * @param handlers - NULL-terminated array of handlers for custom commands
 * @param ans - buffer for output string
 * @return RESULT_OK if all OK or error code
 */
cc_hresult cc_plugin_customcmd(const char *str, cc_parhandler_t *handlers, cc_charbuff *ans){
    if(!str || !handlers) return RESULT_FAIL;
    char key[256], *kptr = key;
    snprintf(key, 255, "%s", str);
    char *val = cc_get_keyval(&kptr);
    cc_parhandler_t *phptr = handlers;
    cc_hresult result = RESULT_BADKEY;
    char buf[512];
#define ADDL(...) do{if(ans){size_t l = snprintf(bptr, L, __VA_ARGS__); bptr += l; L -= l;}}while(0)
#define PRINTVAL(v) do{if(ans){size_t l = print_val(phptr->type, phptr->v, bptr, L); bptr += l; L -= l;}}while(0)
    while(phptr->cmd){
        if(0 == strcmp(kptr, phptr->cmd)){
            char *bptr = buf; size_t L = 511;
            result = RESULT_OK;
            if(phptr->checker) result = phptr->checker(str, ans);
            if(phptr->ptr){ // setter/getter
                if(val){if(result == RESULT_OK){// setter: change value only if [handler] returns OK (`handler` could be value checker)
                    int ival; float fval; double dval;
#define UPDATE_VAL(type, val, pr) do{ \
  if(phptr->max && val > *(type*)phptr->max){ADDL("max=" pr, *(type*)phptr->max); result = RESULT_BADVAL;} \
  if(phptr->min && val < *(type*)phptr->min){ADDL("min=" pr, *(type*)phptr->min); result = RESULT_BADVAL;} \
  if(result == RESULT_OK) *(type*)phptr->ptr = val;  \
}while(0)
                    switch(phptr->type){
                        case CC_PAR_INT:
                            ival = atoi(val);
                            UPDATE_VAL(int, ival, "%d");
                        break;
                        case CC_PAR_FLOAT:
                            fval = (float)atof(val);
                            UPDATE_VAL(float, fval, "%g");
                        break;
                        case CC_PAR_DOUBLE:
                            dval = atof(val);
                            UPDATE_VAL(double, dval, "%g");
                        break;
                        default:
                            result = RESULT_FAIL;
                    }
#undef UPDATE_VAL
                }}else result = RESULT_SILENCE; // getter - don't show "OK"
                DBG("res=%d", result);
                if(result == RESULT_SILENCE || result == RESULT_OK){
                    ADDL("%s=", phptr->cmd);
                    PRINTVAL(ptr);
                }
                if(ans) cc_charbufaddline(ans, buf);
            }
            break;
        }
        ++phptr;
    }
    if(ans && result == RESULT_BADKEY){ // cmd not found - display full help
        cc_charbufaddline(ans, "Custom plugin commands:\n");
        phptr = handlers;
        while(phptr->cmd){
            char *bptr = buf; size_t L = 511;
            ADDL("\t%s", phptr->cmd);
            if(phptr->ptr){
                ADDL(" = (");
                switch(phptr->type){
                    case CC_PAR_INT:
                        ADDL("int");
                    break;
                    case CC_PAR_FLOAT:
                        ADDL("float");
                    break;
                    case CC_PAR_DOUBLE:
                        ADDL("double");
                    break;
                    default:
                        ADDL("undefined");
                }
                ADDL(")");
                if(phptr->min || phptr->max){
                    ADDL(" [");
                    if(phptr->min) PRINTVAL(min);
                    else ADDL("-inf");
                    ADDL(", ");
                    if(phptr->max) PRINTVAL(max);
                    else ADDL("inf");
                    ADDL("]");
                }
            }
            ADDL(" - ");
            ADDL("%s\n", phptr->helpstring);
            cc_charbufaddline(ans, buf);
            ++phptr;
        }
    }
#undef ADDL
    return result;
}
