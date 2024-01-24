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
    FNAME();
    if(fd < 1 || !msg || l < 1) return TRUE; // empty message
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // thread safe
    pthread_mutex_lock(&mutex);
    static char *tmpbuf = NULL;
    static int buflen = 0;
    if(l + 1 > buflen){
        buflen = 1024 * (1 + l/1024);
        tmpbuf = realloc(tmpbuf, buflen);
    }
    DBG("send to fd %d: %s [%d]", fd, msg, l);
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
    FNAME();
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
//    [RESULT_SILENCE] = NULL, // nothing to send
//    [RESULT_DISCONNECTED] = NULL, // not to send
};

const char *cc_hresult2str(cc_hresult r){
    /*red("ALL results:\n");
    for(cc_hresult res = 0; res < RESULT_NUM; ++res){
        printf("%d: %s\n", res, resmessages[res]);
    }*/
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
char *cc_get_keyval(char *keyval){
    //DBG("Got string %s", keyval);
    // remove starting spaces in key
    while(isspace(*keyval)) ++keyval;
    char *val = strchr(keyval, '=');
    if(val){ // got value: remove starting spaces in val
        *val++ = 0;
        while(isspace(*val)) ++val;
    }
    //DBG("val = %s (%zd bytes)", val, (val)?strlen(val):0);
    // remove trailing spaces in key
    char *e = keyval + strlen(keyval) - 1; // last key symbol
    while(isspace(*e) && e > keyval) --e;
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

static cc_hresult sendstrN(int fd, const char *str){
    for(int i = 0; i < ntries; ++i){
        if(!cc_sendstrmessage(fd, str)) continue;
        double t0 = dtime();
        while(dtime() - t0 < CC_ANSWER_TIMEOUT){
            // TODO: continue the code
        }
    }
    return FALSE;
}

/**
 * @brief cc_sendint - send integer value over socket (make 2 tries)
 * @param fd - socket fd
 * @param cmd - setter
 * @param val - value
 * @return answer received
 */
cc_hresult cc_sendint(int fd, const char *cmd, int val){
#define BBUFS   (63)
    char buf[BBUFS+1];
    snprintf(buf, BBUFS, "%s=%d\n", cmd, val);
    return sendstrN(fd, buf);
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

cc_charbuff *cc_bufnew(size_t size){
    DBG("Allocate new buffer with size %zd", size);
    cc_charbuff *b = MALLOC(cc_charbuff, 1);
    b->bufsize = size;
    b->buf = MALLOC(char, size);
    return b;
}

void cc_bufdel(cc_charbuff **buf){
    FREE((*buf)->buf);
    FREE(*buf);
}

/**
 * @brief cc_read2buf - try to read next data portion from POLLED socket
 * @param fd - socket fd to read from
 * @param buf - buffer to read
 * @return FALSE in case of buffer overflow or client disconnect, TRUE if got 0..n bytes of data
 */
int cc_read2buf(int fd, cc_charbuff *buf){
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
 * @brief cc_getline - read '\n'-terminated string from `b` and substitute '\n' by 0
 * @param b - input charbuf
 * @param str (allocated outside) - string-receiver
 * @param len - length of `str` (including terminating zero)
 * @return amount of bytes read
 */
size_t cc_getline(cc_charbuff *b, char *str, size_t len){
    if(!b) return 0;
    size_t idx = 0;
    pthread_mutex_lock(&b->mutex);
    if(!b->buf) goto ret;
    --len; // for terminating zero
    char *ptr = b->buf;
    for(; idx < b->buflen; ++idx) if(*ptr++ == '\n') break;
    if(idx == b->buflen) goto ret;
    size_t minlen = (len > idx) ? idx : len; // prevent `str` overflow
    memcpy(str, b->buf, minlen);
    str[minlen] = 0;
    if(++idx < b->buflen){ // move rest of data in buffer to beginning
        memmove(b->buf, b->buf+idx, b->buflen-idx);
        b->buflen -= idx;
    }else b->buflen = 0;
ret:
    pthread_mutex_unlock(&b->mutex);
    return idx;
}
