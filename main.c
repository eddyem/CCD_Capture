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

#include <omp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <usefull_macros.h>

#include "cmdlnopts.h"
#include "ccdfunc.h"
#ifdef IMAGEVIEW
#include "imageview.h"
#endif
#include "omp.h"
#include "socket.h"

static int isserver = FALSE;
static pid_t childpid = 0;

void signals(int signo){
    signal(signo, SIG_IGN);
    if(childpid){ // master process
        if(signo == SIGUSR1){ // kill child
            kill(childpid, signo);
            signal(signo, signals);
            return;
        }
        DBG("Master killed with sig=%d", signo);
        LOGERR("Master killed with sig=%d", signo);
        if(!GP->client){
            DBG("Unlink pid file");
            unlink(GP->pidfile);
        }
        exit(signo);
    }
    // slave: cancel exposition
    WARNX("Get signal %d - exit", signo);
    if(!GP->client){
        DBG("Cancel capturing");
        cancel();
    }
#ifdef IMAGEVIEW
    DBG("KILL GL");
    closeGL();
    usleep(100000);
#endif
    exit(signo);
}

int main(int argc, char **argv){
    char *self = strdup(argv[0]);
    initial_setup();
/*
    int cpunumber = sysconf(_SC_NPROCESSORS_ONLN);
    if(omp_get_max_threads() != cpunumber)
        omp_set_num_threads(cpunumber);
*/
    parse_args(argc, argv);
    if(GP->outfile && GP->outfileprefix) ERRX("Can't use outfile name and prefix together");
    if(GP->outfile && !GP->rewrite){
        struct stat filestat;
        if(0 == stat(GP->outfile, &filestat)) ERRX("File %s exists!", GP->outfile);
    }
    if(GP->port){
        if(GP->path){
            WARNX("Options `port` and `path` can't be used together! Point `port` for TCP socket or `path` for UNIX.");
            return 1;
        }
        int port = atoi(GP->port);
        if(port < PORTN_MIN || port > PORTN_MAX){
            WARNX("Wrong port value: %d", port);
            return 1;
        }
        if(!GP->client) isserver = TRUE;
    }
    if(GP->path && !GP->client) isserver = TRUE;
    if(GP->logfile){
        int lvl = LOGLEVEL_WARN + GP->verbose;
        DBG("level = %d", lvl);
        if(lvl > LOGLEVEL_ANY) lvl = LOGLEVEL_ANY;
        verbose(1, "Log file %s @ level %d\n", GP->logfile, lvl);
        OPENLOG(GP->logfile, lvl, 1);
        if(!globlog) WARNX("Can't create log file");
    }
    signal(SIGINT, signals);
    signal(SIGQUIT, signals);
    signal(SIGABRT, signals);
    signal(SIGTERM, signals);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGUSR1, signals); // restart server
    // check for another running process in server and standalone mode
    if(!GP->client) check4running(self, GP->pidfile);
    if(!isserver && !GP->client){ // standalone mode
        focusers();
        wheels();
        ccds();
        return 0;
    }
    LOGMSG("Started");
#ifndef EBUG
    if(isserver){
        unsigned int pause = 5;
        while(1){
            childpid = fork();
            if(childpid){ // master
                double t0 = dtime();
                LOGMSG("Created child with pid %d", childpid);
                wait(NULL);
                LOGERR("Child %d died", childpid);
                if(dtime() - t0 < 1.) pause += 5;
                else pause = 1;
                if(pause > 900) pause = 900;
                sleep(pause); // wait a little before respawn
            }else{ // slave
                prctl(PR_SET_PDEATHSIG, SIGTERM); // send SIGTERM to child when parent dies
                break;
            }
        }
    }
#endif

    if(GP->path) return start_socket(isserver, GP->path, FALSE);
    if(GP->port) return start_socket(isserver, GP->port, TRUE);
}

