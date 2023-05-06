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

#include <libintl.h>
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
#include "socket.h"

static int isserver = FALSE;
static pid_t childpid = 0;

void signals(int signo){
    //if(signo) signal(signo, SIG_IGN);
    DBG("signo=%d", signo);
    if(childpid){ // master process
        if(signo == SIGUSR1){ // kill child
            kill(childpid, signo);
            signal(signo, signals);
            return;
        }
        WARNX("Master killed with sig=%d", signo);
        LOGERR("Master killed with sig=%d", signo);
        exit(signo);
    }
    // slave: cancel exposition
    if(signo) WARNX("Get signal %d - exit", signo);
    if(!GP->client){
        DBG("Cancel capturing and close all");
        camstop();
        closewheel();
        focclose();
    }
#ifdef IMAGEVIEW
    DBG("KILL GL");
    closeGL();
#endif
    DBG("exit(%d)", signo);
    exit(signo);
}

int main(int argc, char **argv){
    initial_setup();
#if defined GETTEXT_PACKAGE && defined LOCALEDIR
    printf("GETTEXT_PACKAGE=" GETTEXT_PACKAGE ", LOCALEDIR=" LOCALEDIR "\n");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    textdomain(GETTEXT_PACKAGE);
#endif

/*
    int cpunumber = sysconf(_SC_NPROCESSORS_ONLN);
    if(omp_get_max_threads() != cpunumber)
        omp_set_num_threads(cpunumber);
*/
    parse_args(argc, argv);
    if(GP->viewer){
        GP->client = 1;
        GP->showimage = 1;
    }
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
    if((isserver || GP->client) && !GP->imageport){
        GP->imageport = MALLOC(char, 32);
        if(!GP->port) sprintf(GP->imageport, "12345");
        else snprintf(GP->imageport, 31, "%d", 1+atoi(GP->port));
        verbose(1, "Set image port to %s", GP->imageport);
    }
    if(GP->client && (GP->commondev || GP->focuserdev || GP->cameradev || GP->wheeldev))
       ERRX("Can't be client and standalone in same time!");
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
    if(!isserver){ // run in standalone or client mode
        int camerainit = FALSE;
        if(!GP->client){ // standalone mode
            focusers(); // run focusers and wheels before showimage
            wheels();
            camerainit = prepare_ccds();
        }else{ // client mode
            return start_socket(isserver);
        }
#ifdef IMAGEVIEW
        if(GP->showimage){ // activate image vindow in capture or simple viewer mode
            imagefunc imfn = NULL;
            if(GP->cameradev || GP->commondev){if(camerainit) imfn = ccdcaptured;} // capture mode
            else imfn = NULL; // TODO - simple file viewer
            return viewer(imfn);
        }
#endif
        if(camerainit) ccds();
        signals(0);
    }
    LOGMSG("Started");
#ifndef EBUG
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
#endif

    return start_socket(isserver);
}

