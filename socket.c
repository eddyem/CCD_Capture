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

pthread_mutex_t locmutex = PTHREAD_MUTEX_INITIALIZER; // mutex for wheel/camera/focuser functions

/**
 * @brief open_socket - create socket and open it
 * @param isserver  - TRUE for server, FALSE for client
 * @param path      - UNIX-socket path or local INET socket port
 * @param isnet     - TRUE for INET socket, FALSE for UNIX
 * @return socket FD or -1 if failed
 */
int open_socket(int isserver, char *path, int isnet){
    //DBG("isserver=%d, path=%s, isnet=%d", isserver, path, isnet);
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
        if(getaddrinfo("127.0.0.1", path, &hints, &res) != 0){
            ERR("getaddrinfo");
        }
    }else{
        //DBG("UNIX socket");
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

/**
 * @brief start_socket - create socket and run client or server
 * @param isserver  - TRUE for server, FALSE for client
 * @return 0 if OK
 */
int start_socket(int isserver){
    char *path = NULL;
    int isnet = FALSE;
    if(GP->path) path = GP->path;
    else if(GP->port){ path = GP->port; isnet = TRUE; }
    else ERRX("Point network port or UNIX-socket path");
    int sock = open_socket(isserver, path, isnet), imsock = -1;
    if(sock < 0){
        LOGERR("Can't open socket");
        ERRX("start_socket(): can't open socket");
    }
    if(isserver){
        imsock = open_socket(TRUE, GP->imageport, TRUE);
        server(sock, imsock);
    }else{
#ifdef IMAGEVIEW
        if(GP->showimage){
            if(!GP->viewer && GP->exptime < 0.00001) ERRX("Need exposition time!");
            init_grab_sock(sock);
            viewer(sockcaptured); // start viewer with socket client parser
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

// send image data to client
int sendimage(int fd, uint16_t *data, int l){
    DBG("fd=%d, l=%d", fd, l);
    if(fd < 1 || !data || l < 1) return TRUE; // empty message
    DBG("send new image (size=%d) to fd %d", l, fd);
//strncpy((char*)data, "TEST image data\n", 17);
//l = 16;
    if(l != send(fd, data, l, MSG_NOSIGNAL)){
        WARN("write()");
        LOGWARN("write()");
        return FALSE;
    }
    DBG("success");
    if(globlog)  LOGDBG("SEND image (size=%d) to fd %d", l, fd);
    return TRUE;
}

// simple wrapper over write: add missed newline and log data
int sendmessage(int fd, const char *msg, int l){
    if(fd < 1 || !msg || l < 1) return TRUE; // empty message
    static char *tmpbuf = NULL;
    static int buflen = 0;
    if(l + 1 > buflen){
        buflen += 1024;
        tmpbuf = realloc(tmpbuf, buflen);
    }
    //DBG("send to fd %d: %s [%d]", fd, msg, l);
    memcpy(tmpbuf, msg, l);
    if(msg[l-1] != '\n') tmpbuf[l++] = '\n';
    if(l != send(fd, tmpbuf, l, MSG_NOSIGNAL)){
        WARN("write()");
        LOGWARN("write()");
        return FALSE;
    }else{
        //DBG("success");
        if(globlog){ // logging turned ON
            tmpbuf[l-1] = 0; // remove trailing '\n' for logging
            LOGDBG("SEND '%s'", tmpbuf);
        }
    }
    return TRUE;
}
int sendstrmessage(int fd, const char *msg){
    if(fd < 1 || !msg) return FALSE;
    int l = strlen(msg);
    return sendmessage(fd, msg, l);
}

// text messages for `hresult`
static const char *resmessages[] = {
    [RESULT_OK] = "OK",
    [RESULT_BUSY] = "BUSY",
    [RESULT_FAIL] = "FAIL",
    [RESULT_BADKEY] = "BADKEY",
    [RESULT_BADVAL] = "BADVAL",
    [RESULT_SILENCE] = "",
};

const char *hresult2str(hresult r){
    if(r < 0 || r >= RESULT_NUM) return "BADRESULT";
    return resmessages[r];
}

/**
 * @brief get_keyval - get value of `key = val`
 * @param keyval (io) - pair `key = val`, return `key`
 * @return `val`
 */
char *get_keyval(char *keyval){
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

// parse string of data (command or key=val)
// the CONTENT of buffer `str` WILL BE BROKEN!
// @return FALSE if client closed (nothing to read)
static int parsestring(int fd, handleritem *handlers, char *str){
    if(fd < 1 || !handlers || !handlers->key || !str || !*str) return FALSE;
    char *val = get_keyval(str);
    if(val){
        DBG("RECEIVE '%s=%s'", str, val);
        LOGDBG("RECEIVE '%s=%s'", str, val);
    }else{
        DBG("RECEIVE '%s'", str);
        LOGDBG("RECEIVE '%s'", str);
    }
    for(handleritem *h = handlers; h->key; ++h){
        if(strcmp(str, h->key) == 0){ // found command
            hresult r = RESULT_OK;
            int l = 1;
            if(h->chkfunction){
                double t0 = dtime();
                do{ l = pthread_mutex_trylock(&locmutex); }while(l && dtime() - t0 > BUSY_TIMEOUT);
                if(l){
                    DBG("Can't lock mutex");
                    return RESULT_BUSY; // long blocking work
                }
                r = h->chkfunction(val);
            } // else NULL instead of chkfuntion -> don't check and don't lock mutex
            if(r == RESULT_OK){ // no test function or it returns TRUE
                if(h->handler) r = h->handler(fd, str, val);
                else r = RESULT_FAIL;
            }
            if(!l) pthread_mutex_unlock(&locmutex);
            if(r == RESULT_DISCONNECTED){
                DBG("handler return RESULT_DISCONNECTED");
                return FALSE;
            }
            return sendstrmessage(fd, hresult2str(r));
        }
    }
    DBG("Command not found!");
    return sendstrmessage(fd, resmessages[RESULT_BADKEY]);
}

/**
 * @brief processData - read (if available) data from fd and run processing, sending to fd messages for each command
 * @param fd        - socket file descriptor
 * @param handlers  - NULL-terminated array of handlers
 * @param buf (io)   - zero-terminated buffer for storing rest of data (without newline), its content will be changed
 * @param buflen    - its length
 * @return FALSE if client closed (nothing to read)
 */
int processData(int fd, handleritem *handlers, char *buf, int buflen){
    int curlen = strlen(buf);
    if(curlen == buflen-1) curlen = 0; // buffer overflow - clear old content
    ssize_t rd = read(fd, buf + curlen, buflen-1 - curlen);
    if(rd <= 0){
        //DBG("read %zd bytes from client", rd);
        return FALSE;
    }
    //DBG("got %s[%zd] from %d", buf, rd, fd);
    char *restofdata = buf, *eptr = buf + curlen + rd;
    *eptr = 0;
    do{
        char *nl = strchr(restofdata, '\n');
        if(!nl) break;
        *nl++ = 0;
        if(!parsestring(fd, handlers, restofdata)) return FALSE; // client disconnected
        restofdata = nl;
        //DBG("rest of data: %s", restofdata);
    }while(1);
    if(restofdata != buf) memmove(buf, restofdata, eptr - restofdata + 1);
    return TRUE;
}

/**
 * check data from  fd (polling function for client)
 * @param fd - file descriptor
 * @return 0 in case of timeout, 1 in case of fd have data, -1 if error
 */
int canberead(int fd){
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
