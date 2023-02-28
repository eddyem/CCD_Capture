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
#include <pthread.h>
#include <stdint.h>

// max & min TCP socket port number
#define PORTN_MAX   (65535)
#define PORTN_MIN   (1024)

#define BUFLEN      (1024)
// Max amount of connections
#define MAXCLIENTS  (30)

// wait for mutex locking
#define BUSY_TIMEOUT    (0.3)
// waiting for answer timeout
#define ANSWER_TIMEOUT  (1.0)
// wait for exposition ends (between subsequent check calls)
#define WAIT_TIMEOUT    (2.0)
// client will disconnect after this time from last server message
#define CLIENT_TIMEOUT  (10.0)

extern pthread_mutex_t locmutex;

typedef enum{
    RESULT_OK,      // all OK
    RESULT_BUSY,    // camera busy and no setters can be done
    RESULT_FAIL,    // failed running command
    RESULT_BADVAL,  // bad key's value
    RESULT_BADKEY,  // bad key
    RESULT_SILENCE, // send nothing to client
    RESULT_DISCONNECTED,// client disconnected
    RESULT_NUM
} hresult;

const char *hresult2str(hresult r);

// fd - socket fd to send private messages, key, val - key and its value
typedef hresult (*mesghandler)(int fd, const char *key, const char *val);

typedef struct{
    hresult (*chkfunction)(char *val);  // function to check device is ready
    mesghandler handler;                // handler function
    const char *key;                    // keyword
} handleritem;

int start_socket(int server, char *path, int isnet);
int sendimage(int fd, uint16_t *data, int l);
int sendmessage(int fd, const char *msg, int l);
int sendstrmessage(int fd, const char *msg);
char *get_keyval(char *keyval);

int processData(int fd, handleritem *handlers, char *buf, int buflen);
