CCD/CMOS imaging server
=======================

Supports FLI cameras/focusers/wheels and cameras: ZWO, Basler, HikRobot, PointGrey, Apogee.
Allows to run as standalone application or imaging server/client.

To restart server (e.g. if hardware was off) kill it with SIGUSR1

## Compile

cmake options:

- `-DAPOGEE=ON` - compile Apogee plugin
- `-DASTAR=ON` - compile "artifical star" plugin
- `-DBASLER=ON` - compile Basler support plugin
- `-DDEBUG=ON` - make with a lot debugging info
- `-DDUMMY=OFF` - compile without dummy camera plugin
- `-DEXAMPLES=ON` - compile also some exaples of libccdcapture use
- `-DFLI=ON` - compile FLI support plugin
- `-DFLYCAP=ON` - compile GrassHopper PointGrey plugin
- `-DHIKROBOT=ON` - compile HikRobot support plugin
- `-DIMAGEVIEW=OFF` - compile without image viewer support (OpenGL!!!)
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
  -_, --plugincmd             custom camera device plugin command
  -a, --addsteps=arg          move focuser to relative position, mm (only for standalone)
  -c, --conf-ioport=arg       configure I/O port pins to given value (decimal number, pin1 is LSB, 1 == output, 0 == input)
  -d, --dark                  not open shutter, when exposing ("dark frames")
  -f, --fast                  fast readout mode
  -g, --goto=arg              move focuser to absolute position, mm
  -h, --hbin=arg              horizontal binning to N pixels
  -i, --get-ioport            get value of I/O port pins
  -k, --shmkey=arg            shared memory (with image data) key (default: 7777777)
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
  --forceimsock               force using image through socket transition even if can use SHM
  --gain=arg                  CMOS gain level
  --help                      show this help
  --imageport=arg             INET image socket port
  --infty=arg                 start (!=0) or stop(==0) infinity capturing loop
  --logfile=arg               logging file name (if run as server)
  --open-shutter              open shutter
  --path=arg                  UNIX socket name (command socket)
  --plugin=arg                common device plugin (e.g devfli.so)
  --port=arg                  local INET command socket port
  --restart                   restart image server
  --rewrite                   rewrite output file if exists
  --set-fan=arg               set fan speed (0 - off, 1 - low, 2 - high)
  --shutter-on-high           run exposition on HIGH @ pin5 I/O port
  --shutter-on-low            run exposition on LOW @ pin5 I/O port
  --viewer                    passive viewer (only get last images)
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

## Plugins custom commands
Since version 1.2.0 introduced custom camera plugin commands system. Commonly to read help just
type `-_help`. You can point as much custom commands in one commandline as you need. They can be a
procedures/flags (like `-_cmd`) or a setters/getters (like `-_key` and `-_key=value`).

### Dummy camera plugin custom commands
This plugin simply emulates image aqcuisition process where images are 2-D sinusoide with given periods.
Each next frame will be shifted by one pixel.

Commands:

- px = (double) [1, inf] - set/get sin period over X axis (pix)
- py = (double) [1, inf] - set/get sin period over Y axis (pix)


### Artifical star plugin custom commands
This plugin lets you to emulate star field with up to 32 stars. You can shift center of field emulating
telescope correction, also you can rotate field emulating derotation.
All stars (Moffat) have the same FWHM and scale parameters. Their coordinates are given by arrays `x`
and `y` with a hardware magnitude `mag`. You can emulate image drift and rotation. Also you can add
a little image position (full frame position) fluctuations. To emulate poisson noice just point its `lambda`
value (`lambda==1` means no noise).

Commands:

-    xc = (int) - x center of field in array coordinates
-    yc = (int) - y center of field in array coordinates
-    x - X coordinate of next star
-    y - Y coordinate of next star
-    fwhm = (double) [0.1, 10] - stars min FWHM, arcsec
-    scale = (double) [0.001, 3600] - CCD scale: arcsec/pix
-    mag - Next star magnitude: 0m is 0xffff/0xff (16/8 bit) ADUs per second
-    mask - load mask image (binary, ANDed)
-    vx = (double) [-20, 20] - X axe drift speed (arcsec/s)
-    vy = (double) [-20, 20] - Y axe drift speed (arcsec/s)
-    vr = (double) [-36000, 36000] - rotation speed (arcsec/s)
-    fluct = (double) [0, 3] - stars position fluctuations (arcsec per sec)
-    beta = (double) [0.5, inf] - Moffat `beta` parameter
-    lambda = (double) [1, inf] - Poisson noice lambda value (>1)
-    rotangle = (double) [0, 1.296e+06] - Starting rotation angle (arcsec)

