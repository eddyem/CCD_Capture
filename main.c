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
#include "server.h"
#include "socket.h"

static pid_t childpid = 0;

// get struct size
//char (*__dummy)[sizeof(cc_IMG)] = 1;

void signals(int signo){
    //if(signo) signal(signo, SIG_IGN);
    DBG("signo=%d", signo);
    if(childpid){ // master process
        if(signo == SIGUSR1){ // kill child
            kill(childpid, signo);
            signal(signo, signals);
            return;
        }
        WARNX(_("Master killed with sig=%d"), signo);
        LOGERR("Master killed with sig=%d", signo);
        exit(signo);
    }
    // slave: cancel exposition
    if(signo){
        WARNX(_("Get signal %d - exit"), signo);
#ifdef IMAGEVIEW
        if(!GP->server){
            windowData *win = getWin();
            if(win){
                win->killthread = 1;
                return;
            }
        }
#endif
    }
    if(GP->server){
        DBG("Cancel capturing and close all");
        stop_server(); // wait until 1s for server to stopped
    }else{
#ifdef IMAGEVIEW
        DBG("KILL GL");
        closeGL();
#endif
        closewheel();
        focclose();
        closecam();
    }
    DBG("exit(%d)", signo);
    exit(signo);
}

typedef enum{
    SERVER_MODE,
    CLIENT_MODE,
    STANDALONE_MODE
} working_mode_t;

int main(int argc, char **argv){
    sl_init();
#if defined GETTEXT_PACKAGE && defined LOCALEDIR
    //printf("GETTEXT_PACKAGE=" GETTEXT_PACKAGE ", LOCALEDIR=" LOCALEDIR "\n");
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
    if(GP->outfile && GP->outfileprefix){
        WARNX(_("Can't use outfile name and prefix together"));
        return 1;
    }
    if(GP->outfile && !GP->rewrite){
        struct stat filestat;
        if(0 == stat(GP->outfile, &filestat)){
            WARNX(_("File %s exists!"), GP->outfile);
            return 1;
        }
    }
    if(GP->anstmout > 0.){
        if(!cc_setAnsTmout(GP->anstmout)){
            WARNX(_("Can't set answer timeout to %g"), GP->anstmout);
            return 1;
        }
    }
    if(GP->client && GP->server){
        WARNX(_("Can't run in client and server modes simultaneously"));
        return 1;
    }
    working_mode_t mode = STANDALONE_MODE;
    if(GP->client) mode = CLIENT_MODE;
    else if(GP->server) mode = SERVER_MODE;
    if(GP->imageport){
        int port = atoi(GP->imageport);
        if(port < CC_PORTN_MIN || port > CC_PORTN_MAX){
            WARNX(_("Wrong image port value: %d"), port);
            return 1;
        }
    }
    if(GP->port){ // open TCP socket besides (or instead - for client) UNIX-socket
        int port = atoi(GP->port);
        if(port < CC_PORTN_MIN || port > CC_PORTN_MAX){
            WARNX(_("Wrong port value: %d"), port);
            return 1;
        }
        if(!GP->imageport){ // make default: port + 1
            GP->imageport = MALLOC(char, 32);
            snprintf(GP->imageport, 31, "%d", 1 + port);
            verbose(VERBOSE_PRIMARY, _("Set image port to %s\n"), GP->imageport);
        }
    }
    if(mode == CLIENT_MODE && (GP->commondev || GP->focuserdev || GP->cameradev || GP->wheeldev)){
        WARNX(_("Can't be client and standalone in same time!"));
        return 1;
    }
    if(GP->logfile){
        int lvl = LOGLEVEL_ERR + GP->verbose;
        DBG("level = %d", lvl);
        if(lvl > LOGLEVEL_ANY) lvl = LOGLEVEL_ANY;
        verbose(VERBOSE_PRIMARY, _("Log file %s @ level %d\n"), GP->logfile, lvl);
        OPENLOG(GP->logfile, lvl, 1);
        if(!sl_globlog) WARN(_("Can't create log file %s"), GP->logfile);
        else LOGMSG("Started");
    }
    signal(SIGINT, signals);
    signal(SIGQUIT, signals);
    signal(SIGABRT, signals);
    signal(SIGTERM, signals);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, signals); // restart server
    if(mode == STANDALONE_MODE){
        focusers();
        wheels();
        int camerainit = prepare_ccds();
#ifdef IMAGEVIEW
        if(GP->showimage){ // activate image window in capture or simple viewer mode
            imagefunc imfn = NULL;
            if(camerainit) imfn = ccdcaptured; // capture mode
            if(!imfn){
                // TODO - simple file viewer
                WARNX(_("Point camera device plugin to run viewer"));
                closewheel();
                focclose();
                return 1;
            }
            return viewer(imfn);
        }
#endif
        if(camerainit) ccds();
        closewheel();
        focclose();
        return 0;
    } else if(mode == CLIENT_MODE){
        return start_socket(FALSE);
    }
    // server mode
    LOGMSG("Started image server");
#ifndef EBUG
    unsigned int pause = 5;
    while(1){
        childpid = fork();
        if(childpid < 0){
            WARN("fork() error");
            LOGERR("fork() error");
            sleep(1);
            continue;
        }
        if(childpid){ // master
            double t0 = sl_dtime();
            LOGMSG("Created child with pid %d", childpid);
            wait(NULL);
            LOGERR("Child %d died", childpid);
            if(sl_dtime() - t0 < 1.) pause += 5;
            else pause = 1;
            if(pause > 900) pause = 900;
            sleep(pause); // wait a little before respawn
        }else{ // slave
            prctl(PR_SET_PDEATHSIG, SIGTERM); // send SIGTERM to child when parent dies
            break;
        }
    }
#endif

    return start_socket(TRUE);
}

