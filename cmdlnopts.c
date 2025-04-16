

#include <assert.h>
#include <math.h> // NAN
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <usefull_macros.h>

#include "cmdlnopts.h"
#include "ccdcapture.h"

static int help;
glob_pars *GP = NULL;
//            DEFAULTS
// default global parameters
static glob_pars  G = {
    .instrument = NULL,
    .exptime = -1.,
    .nframes = 0,
    .X0 = INT_MIN, .Y0 = INT_MIN,
    .X1 = INT_MIN, .Y1 = INT_MIN,
    .focdevno = -1,
    .camdevno = -1,
    .whldevno = -1,
    .temperature = NAN,
    .shtr_cmd = -1,
    .confio = -1, .setio = -1,
    .gotopos = NAN, .addsteps = NAN,
    .brightness = NAN, .gain = NAN,
    .setwheel = -1,
    .fanspeed = -1,
    .shmkey = 7777777,
    .anstmout = -1,
    .infty = -1
};

// need this to proper work with only-long args
//#define NA  (-__COUNTER__)
// TODO: fix the bug in usefull_macros first!!!
#define NA (0)

/*
 * Define command line options by filling structure:
 *  name    has_arg flag    val     type        argptr          help
*/
sl_option_t cmdlnopts[] = {
    {"plugin"   ,NEED_ARG,  NULL,   NA,     arg_string, APTR(&G.commondev), N_("common device plugin (e.g devfli.so)")},
    {"plugincmd",MULT_PAR,  NULL,   '_',    arg_string, APTR(&G.plugincmd), N_("custom camera device plugin command")},
    {"cameradev", NEED_ARG, NULL,   'C',    arg_string, APTR(&G.cameradev), N_("camera device plugin (e.g. devfli.so)")},
    {"focuserdev", NEED_ARG,NULL,   'F',    arg_string, APTR(&G.focuserdev),N_("focuser device plugin (e.g. devzwo.so)")},
    {"wheeldev", NEED_ARG,  NULL,   'W',    arg_string, APTR(&G.wheeldev),  N_("wheel device plugin (e.g. devdummy.so)")},
    {"list",    NO_ARGS,    NULL,   'L',    arg_int,    APTR(&G.listdevices),N_("list connected devices")},
    {"camdevno",NEED_ARG,   NULL,    NA,    arg_int,    APTR(&G.camdevno),  N_("camera device number (if many: 0, 1, 2 etc)")},
    {"wheeldevno",NEED_ARG, NULL,    NA,    arg_int,    APTR(&G.whldevno),  N_("filter wheel device number (if many: 0, 1, 2 etc)")},
    {"focdevno",NEED_ARG,   NULL,    NA,    arg_int,    APTR(&G.focdevno),  N_("focuser device number (if many: 0, 1, 2 etc)")},
    {"help",    NO_ARGS,    &help,   1,     arg_none,   NULL,               N_("show this help")},
    {"rewrite", NO_ARGS,    &G.rewrite,1,   arg_none,   NULL,               N_("rewrite output file if exists")},
    {"verbose", NO_ARGS,    NULL,   'V',    arg_none,   APTR(&G.verbose),   N_("verbose level (-V - messages, -VV - debug, -VVV - all shit)")},
    {"dark",    NO_ARGS,    NULL,   'd',    arg_int,    APTR(&G.dark),      N_("not open shutter, when exposing (\"dark frames\")")},
    {"8bit",    NO_ARGS,    NULL,   '8',    arg_int,    APTR(&G._8bit),     N_("run in 8-bit mode")},
    {"fast",    NO_ARGS,    NULL,   'f',    arg_none,   APTR(&G.fast),      N_("fast readout mode")},
    {"set-temp",NEED_ARG,   NULL,   't',    arg_double, APTR(&G.temperature),N_("set CCD temperature to given value (degr C)")},
    {"set-fan", NEED_ARG,   NULL,   NA,     arg_int,    APTR(&G.fanspeed),  N_("set fan speed (0 - off, 1 - low, 2 - high)")},

    {"author",  NEED_ARG,   NULL,   'A',    arg_string, APTR(&G.author),    N_("program author")},
    {"objtype", NEED_ARG,   NULL,   'Y',    arg_string, APTR(&G.objtype),   N_("object type (neon, object, flat etc)")},
    {"instrument",NEED_ARG, NULL,   'I',    arg_string, APTR(&G.instrument),N_("instrument name")},
    {"object",  NEED_ARG,   NULL,   'O',    arg_string, APTR(&G.objname),   N_("object name")},
    {"obsname", NEED_ARG,   NULL,   'N',    arg_string, APTR(&G.observers), N_("observers' names")},
    {"prog-id", NEED_ARG,   NULL,   'P',    arg_string, APTR(&G.prog_id),   N_("observing program name")},
    {"addrec",  MULT_PAR,   NULL,   'r',    arg_string, APTR(&G.addhdr),    N_("add records to header from given file[s]")},
    {"outfile", NEED_ARG,   NULL,   'o',    arg_string, APTR(&G.outfile),   N_("output file name")},
    {"wait",    NO_ARGS,    &G.waitexpend,1,arg_none,   NULL,               N_("wait while exposition ends")},

    {"nflushes",NEED_ARG,   NULL,   'l',    arg_int,    APTR(&G.nflushes),  N_("N flushes before exposing (default: 1)")},
    {"hbin",    NEED_ARG,   NULL,   'h',    arg_int,    APTR(&G.hbin),      N_("horizontal binning to N pixels")},
    {"vbin",    NEED_ARG,   NULL,   'v',    arg_int,    APTR(&G.vbin),      N_("vertical binning to N pixels")},
    {"nframes", NEED_ARG,   NULL,   'n',    arg_int,    APTR(&G.nframes),   N_("make series of N frames")},
    {"pause",   NEED_ARG,   NULL,   'p',    arg_int,    APTR(&G.pause_len), N_("make pause for N seconds between expositions")},
    {"exptime", NEED_ARG,   NULL,   'x',    arg_double, APTR(&G.exptime),   N_("set exposure time to given value (seconds!)")},
    {"cancel",  NO_ARGS, &G.cancelexpose, 1,arg_none,   NULL,               N_("cancel current exposition")},
    {"X0",      NEED_ARG,   NULL,   NA,     arg_int,    APTR(&G.X0),        N_("absolute (not divided by binning!) frame X0 coordinate (-1 - all with overscan)")},
    {"Y0",      NEED_ARG,   NULL,   NA,     arg_int,    APTR(&G.Y0),        N_("absolute frame Y0 coordinate (-1 - all with overscan)")},
    {"X1",      NEED_ARG,   NULL,   NA,     arg_int,    APTR(&G.X1),        N_("absolute frame X1 coordinate (-1 - all with overscan)")},
    {"Y1",      NEED_ARG,   NULL,   NA,     arg_int,    APTR(&G.Y1),        N_("absolute frame Y1 coordinate (-1 - all with overscan)")},

    {"open-shutter",NO_ARGS,&G.shtr_cmd, SHUTTER_OPEN,arg_none,NULL,        N_("open shutter")},
    {"close-shutter",NO_ARGS,&G.shtr_cmd, SHUTTER_CLOSE,arg_none,NULL,      N_("close shutter")},
    {"shutter-on-low",NO_ARGS,&G.shtr_cmd, SHUTTER_OPENATLOW,arg_none,NULL, N_("run exposition on LOW @ pin5 I/O port")},
    {"shutter-on-high",NO_ARGS,&G.shtr_cmd,SHUTTER_OPENATHIGH,arg_none,NULL,N_("run exposition on HIGH @ pin5 I/O port")},
    {"get-ioport",NO_ARGS,  NULL,   'i',    arg_int,    APTR(&G.getio),     N_("get value of I/O port pins")},
    {"async",   NO_ARGS,    &G.async,1,     arg_none,   NULL,               N_("move stepper motor asynchronous")},

    {"set-ioport",NEED_ARG, NULL,   's',    arg_int,    APTR(&G.setio),     N_("set I/O port pins to given value (decimal number, pin1 is LSB)")},
    {"conf-ioport",NEED_ARG,NULL,   'c',    arg_int,    APTR(&G.confio),    N_("configure I/O port pins to given value (decimal number, pin1 is LSB, 1 == output, 0 == input)")},

    {"goto",    NEED_ARG,   NULL,   'g',    arg_double, APTR(&G.gotopos),   N_("move focuser to absolute position, mm")},
    {"addsteps",NEED_ARG,   NULL,   'a',    arg_double, APTR(&G.addsteps),  N_("move focuser to relative position, mm (only for standalone)")},

    {"wheel-set",NEED_ARG,  NULL,   'w',    arg_int,    APTR(&G.setwheel),  N_("set wheel position")},

    {"gain",    NEED_ARG,   NULL,   NA,     arg_float,  APTR(&G.gain),      N_("CMOS gain level")},
    {"brightness",NEED_ARG, NULL,   NA,     arg_float,  APTR(&G.brightness),N_("CMOS brightness level")},

    {"logfile", NEED_ARG,   NULL,   NA,     arg_string, APTR(&G.logfile),   N_("logging file name (if run as server)")},
    {"path",    NEED_ARG,   NULL,   NA,     arg_string, APTR(&G.path),      N_("UNIX socket name (command socket)")},
    {"port",    NEED_ARG,   NULL,   NA,     arg_string, APTR(&G.port),      N_("local INET command socket port")},
    {"imageport",NEED_ARG,  NULL,   NA,     arg_string, APTR(&G.imageport), N_("INET image socket port")},
    {"client",  NO_ARGS,    &G.client,1,    arg_none,   NULL,               N_("run as client")},
    {"viewer",  NO_ARGS,    &G.viewer,1,    arg_none,   NULL,               N_("passive viewer (only get last images)")},
    {"restart", NO_ARGS,    &G.restart,1,   arg_none,   NULL,               N_("restart image server")},
    {"timeout", NEED_ARG,   NULL,   '0',    arg_double, APTR(&G.anstmout),  N_("network answer timeout (default: 0.1s)")},

    {"shmkey", NEED_ARG,    NULL,   'k',    arg_int,    APTR(&G.shmkey),    N_("shared memory (with image data) key (default: 7777777)")},
    {"forceimsock",NO_ARGS, &G.forceimsock,1, arg_none, NULL,               N_("force using image through socket transition even if can use SHM")},
    {"infty", NEED_ARG,     NULL,   NA,     arg_int,    APTR(&G.infty),     N_("start (!=0) or stop(==0) infinity capturing loop")},

#ifdef IMAGEVIEW
    {"display", NO_ARGS,    NULL,   'D',    arg_int,   APTR(&G.showimage),  N_("Display image in OpenGL window")},
#endif
    //{"",  NEED_ARG,   NULL,   '',    arg_double,   APTR(&G.),    N_("")},

    end_option
};

/**
 * Parse command line options and return dynamically allocated structure
 *      to global parameters
 * @param argc - copy of argc from main
 * @param argv - copy of argv from main
 * @return allocated structure with global parameters
 */
glob_pars *parse_args(int argc, char **argv){
    // format of help: "Usage: progname [args]\n"
    sl_helpstring("Version: " PACKAGE_VERSION "\nUsage: %s [args] [output file prefix]\nTo restart server kill it with SIGUSR1\n\tArgs are:\n");
    // parse arguments
    sl_parseargs(&argc, &argv, cmdlnopts);
    if(help) sl_showhelp(-1, cmdlnopts);
    if(argc > 0){
        G.outfileprefix = strdup(argv[0]);
        if(argc > 1){
            WARNX("%d unused parameters:\n", argc - 1);
            for(int i = 1; i < argc; ++i)
                printf("\t%4d: %s\n", i, argv[i]);
        }
    }
    GP = &G;
    return GP;
}

/**
 * @brief verbose - print additional messages depending of G.verbose
 * @param levl - message level
 * @param fmt  - message
 */
void verbose(int levl, const char *fmt, ...){
    va_list ar;
    if(levl > G.verbose) return;
    //printf("%s: ", __progname);
    va_start(ar, fmt);
    vprintf(fmt, ar);
    va_end(ar);
    printf("\n");
}
