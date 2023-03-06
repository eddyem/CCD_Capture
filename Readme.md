CCD/CMOS imaging server
=======================

Supports FLI cameras/focusers/wheels and cameras: ZWO, Basler, HikRobot, PointGrey, Apogee.
Allows to run as standalone application or imaging server/client.

To restart server (e.g. if hardware was off) kill it with SIGUSR1

## Compile

cmake options:

- `-DAPOGEE=ON` - compile Apogee plugin
- `-DDEBUG=ON` - make with a lot debugging info
- `-DIMAGEVIEW=ON` - compile with image viewer support (only for standalone) (OpenGL!!!)
- `-DBASLER=ON` - compile Basler support plugin
- `-DFLI=ON` - compile FLI support plugin
- `-DFLYCAPT=ON` - compile GrassHopper PointGrey plugin
- `-DHIKROBOT=ON` - compile HikRobot support plugin
- `-DZWO=ON` - compile ZWO support plugin


```
Usage: ccd_capture [args] [output file prefix]
        Args are:

  -8, --8bit                  run in 8-bit mode
  -A, --author=arg            program author
  -C, --cameradev=arg         camera device plugin (e.g. devfli.so)
  -D, --display               Display image in OpenGL window
  -F, --focuserdev=arg        focuser device plugin (e.g. devzwo.so)
  -I, --instrument=arg        instrument name
  -L, --list                  list connected devices
  -N, --obsname=arg           observers' names
  -O, --object=arg            object name
  -P, --prog-id=arg           observing program name
  -V, --verbose               verbose level (-V - messages, -VV - debug, -VVV - all shit)
  -W, --wheeldev=arg          wheel device plugin (e.g. devdummy.so)
  -Y, --objtype=arg           object type (neon, object, flat etc)
  -a, --addsteps=arg          move focuser to relative position, mm (only for standalone)
  -c, --conf-ioport=arg       configure I/O port pins to given value (decimal number, pin1 is LSB, 1 == output, 0 == input)
  -d, --dark                  not open shutter, when exposing ("dark frames")
  -f, --fast                  fast readout mode
  -g, --goto=arg              move focuser to absolute position, mm
  -h, --hbin=arg              horizontal binning to N pixels
  -i, --get-ioport            get value of I/O port pins
  -l, --nflushes=arg          N flushes before exposing (default: 1)
  -n, --nframes=arg           make series of N frames
  -o, --outfile=arg           output file name
  -p, --pause=arg             make pause for N seconds between expositions
  -r, --addrec                add records to header from given file[s]
  -s, --set-ioport=arg        set I/O port pins to given value (decimal number, pin1 is LSB)
  -t, --set-temp=arg          set CCD temperature to given value (degr C)
  -v, --vbin=arg              vertical binning to N pixels
  -w, --wheel-set=arg         set wheel position
  -x, --exptime=arg           set exposure time to given value (seconds!)
  --X0=arg                    absolute (not divided by binning!) frame X0 coordinate (-1 - all with overscan)
  --X1=arg                    absolute frame X1 coordinate (-1 - all with overscan)
  --Y0=arg                    absolute frame Y0 coordinate (-1 - all with overscan)
  --Y1=arg                    absolute frame Y1 coordinate (-1 - all with overscan)
  --async                     move stepper motor asynchronous
  --brightness=arg            CMOS brightness level
  --camdevno=arg              camera device number (if many: 0, 1, 2 etc)
  --cancel                    cancel current exposition
  --client                    run as client
  --close-shutter             close shutter
  --focdevno=arg              focuser device number (if many: 0, 1, 2 etc)
  --gain=arg                  CMOS gain level
  --help                      show this help
  --logfile=arg               logging file name (if run as server)
  --open-shutter              open shutter
  --path=arg                  UNIX socket name
  --pidfile=arg               PID file (default: /tmp/CCD_Capture.pid)
  --plugin=arg                common device plugin (e.g devfli.so)
  --port=arg                  local INET socket port
  --restart                   restart image server
  --rewrite                   rewrite output file if exists
  --set-fan=arg               set fan speed (0 - off, 1 - low, 2 - high)
  --shutter-on-high           run exposition on HIGH @ pin5 I/O port
  --shutter-on-low            run exposition on LOW @ pin5 I/O port
  --wait                      wait while exposition ends
  --wheeldevno=arg            filter wheel device number (if many: 0, 1, 2 etc)
```

## Image viewer
In image view mode you can display menu by clicking of right mouse key or use shortcuts:

- '0' - restore zoom;
- 'c' - capture new image in pause mode (works only with `-n` flag);
- 'e' - switch on/off histogram equalization;
- 'l' - flip image right-left;
- 'p' - pause or resume capturing (works only with `-n` flag);
- 'u' - flip image up-down;
- 'Z' - zoom+;
- 'z' - zoom-;
- 'Ctrl+r' - roll histogram conversion function (LOG, SQRT, POW, LINEAR);
- 'Ctrl+s' - save displayed image (works only if you pointed output file name or prefix);
- 'Ctrl+q' - exit.

Mouse functions:

- Left button - center selected point.
- Middle button - move image.
- Right button - show menu.
- Wheel up - scroll up, or scroll left (with Shift), or zoom+ (with Ctrl).
- Wheel down - scroll down, or scroll right (with Shift), or zoom- (with Ctrl).

