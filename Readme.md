CCD/CMOS imaging server
=======================

Supports FLI cameras/focusers/wheels and cameras: ZWO, Basler, HikRobot, PointGrey, Apogee.
Allows to run as standalone application or imaging server/client.

To restart server (e.g. if hardware was off) kill it with SIGUSR1 or send command `restartTheServer`.

## Compile
The tool itself depends on [usefull_macros](https://github.com/eddyem/snippets_library) librariy.
"Artifical star" plugin depends on my [improclib](https://github.com/eddyem/improclib).
Image viewer depends on OpenGL and GLUT libraries (also depending on X11).
All other plugins (excluding "dummy camera") depends on third-party libraries. Apogee plugin also
depends on [apogee C wrapper](https://github.com/eddyem/apogee_control/tree/master/apogee_C_wrapper).

I will try later to combine all third-party libraries in this repository.


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

## Server
When you run the server you need at least to point:

- camera plugin (`-C libdev….so`);
- command socket: it could be local INET socket (then by default image socket will have next number),
just point `--port=port`, or UNIX socket, then point `--path=path` (for unnamed sockets use "\0" as first
path letter);
- image socket (optionally): `--imageport=port` - to have ability to transmit image to other PCs by
network INET socket (default value: 12345 if no command socket port used, or cmdport+1);
- shared memory key for fast local image transmission, `-k=key` (default value: 7777777).

To send commands to server you can use client, open `netcat` session, use my [tty_term](https://github.com/eddyem/tty_term)
or any other tools. Server have text protocol (send `help\n` to see full list):

- 8bit - run in 8 bit mode instead of 16 bit
- author - FITS 'AUTHOR' field
- brightness - camera brightness
- camdevno - camera device number
- camlist - list all connected cameras
- ccdfanspeed - fan speed of camera
- confio - camera IO configuration
- dark - don't open shutter @ exposure
- expstate - get exposition state
- exptime - exposition time
- fastspeed - fast readout speed
- filename - save file with this name, like file.fits
- filenameprefix - prefix of files, like ex (will be saved as exXXXX.fits)
- focdevno - focuser device number
- foclist - list all connected focusers
- focpos - focuser position
- format - camera frame format (X0,Y0,X1,Y1)
- gain - camera gain
- getheaders - get last file FITS headers
- hbin - horizontal binning
- headerfiles - add FITS records from these files (comma-separated list)
- help - show this help
- imheight - last image height
- imwidth - last image width
- info - connected devices state
- infty - an infinity loop taking images until there's connected clients
- instrument - FITS 'INSTRUME' field
- io - get/set camera IO
- lastfilename - path to last saved file
- maxformat - camera maximal available format
- nflushes - camera number of preflushes
- object - FITS 'OBJECT' field
- objtype - FITS 'IMAGETYP' field
- observer - FITS 'OBSERVER' field
- plugincmd - custom camera plugin command
- program - FITS 'PROG-ID' field
- restartTheServer - restart server
- rewrite - rewrite file (if give `filename`, not `filenameprefix`)
- shmemkey - get shared memory key
- shutter - camera shutter's operations
- tcold - camera chip temperature
- tremain - time (in seconds) of exposition remained
- vbin - vertical binning
- wdevno - wheel device number
- wlist - list all connected wheels
- wpos - wheel position

Send `expstate=1` to start capture. As only the frame is ready server will send `expstate=4` to all
clients connected.

When you send a command to server you will receive:

- one of answers - "OK" (all OK), "BUSY" (can't run setter because camera is busy), "FAIL" (some error
occured), "BADKEY" (wrong key name) or "BADVAL" (wrong key's value) - for procedures and setters
(instead of "OK" you will give "parameter=value" for setter if all OK);
- "parameter=value" for getters.

Command `info` equivalent to sequential commands `camlist`, `hbin`, `vbin`, `tcold`, `tbody`, `thot`,
`exptime`, `lastfilename`, `expstate` and `camflags`.

Command `getheaders` returns base FITS-header of last file.

## Client
To run client you should point `--client` and same ports' options like for server (command socket and
image SHM key or socket).
Use `--forceimsock` to forse getting image through INET socket even if run on the same PC as server.
Client have no options to connect to server on other host, so you need to make proxy with ssh or other
tool. Maybe later I will add `--host` option for these purposes (but for security you will still have
to use ssh-proxy for command socket).

Using client as pure viewer (using `--viewer` option) you can't run expositions - just can view last
files done by command from other clients. But if you will send to server `infty=1`, it will run in
infinity mode, making frame by frame while at least one client connected. To turn this off send
`infty=0`.

Without image view options (`--viewer` or `-D`) client just send given options to server and shuts down,
but you can wait until image done with option `--wait` (be careful: when reading process takes too long
time taking server in busy state, client can decide to shut down by timeout).

## Plugins custom commands
Since version 1.2.0 introduced custom camera plugin commands system. Commonly to read help just
type `-_help`. You can point as much custom commands in one commandline as you need. They can be a
procedures/flags (like `-_cmd`) or a setters/getters (like `-_key` and `-_key=value`).

To transmit these custom commands to server use `plugincmd=key[=value]`.

### Dummy camera plugin custom commands
This plugin simply emulates image aqcuisition process where images are 2-D sinusoide with given periods.
Each next frame will be shifted by one pixel.

Commands:

- px = (double) [1, inf] - set/get sin period over X axis (pix)
- py = (double) [1, inf] - set/get sin period over Y axis (pix)


### Artifical star plugin custom commands
This plugin lets you to emulate star field with up to 32 stars. You can shift center of field emulating
telescope correction, also you can rotate field emulating derotation.
All stars (Moffat) have the same FWHM and scale parameters. Their coordinates (arcsec) are given by arrays `x`
and `y` with a hardware magnitude `mag`. You can emulate image drift and rotation. Also you can add
a little image position (full frame position) fluctuations. To emulate poisson noice just point its `lambda`
value (`lambda==1` means no noise).

Two additional commands - `mask` and `bkg` allows you to load binary mask and background.
The background image (8-bit monochrome) will be added to generated star field. After that if you
pointed an mask image it will be AND-ed with result: all non-zero pixels on mask remains the same
pixels of resulting image, but zeros will clean them emulating holes or screens.
And at last Poisson noice will be added to full frame.

Commands:

-   beta = (double) [0.5, inf] - Moffat `beta` parameter
-   bkg = (string) - load background image
-   fluct = (double) [0, 3] - stars position fluctuations (arcsec per sec)
-   fwhm = (double) [0.1, 10] - stars min FWHM, arcsec
-   lambda = (double) [1, inf] - Poisson noice lambda value (>1)
-   mag = (double) [-30, 30] - Next star magnitude: 0m is 0xffff/0xff (16/8 bit) ADUs per second
-   mask = (string) - load mask image (binary, ANDed)
-   rotangle = (double) [0, 1.296e+06] - Starting rotation angle (arcsec)
-   scale = (double) [0.001, 3600] - CCD scale: arcsec/pix
-   vr = (double) [-36000, 36000] - rotation speed (arcsec/s)
-   vx = (double) [-20, 20] - X axe drift speed (arcsec/s)
-   vy = (double) [-20, 20] - Y axe drift speed (arcsec/s)
-   x = (int) - X coordinate of next star (arcsec, in field coordinate system)
-   y = (int) - Y coordinate of next star (arcsec, in field coordinate system)
-   xc = (int) - x center of field in array coordinates
-   yc = (int) - y center of field in array coordinates


## Examples

### ccd_client

Connect to server, send `infty` and simply get up to N images (or quit if no images in 5 seconds).

Usage:
```
  -U, --isun          use UNIX socket
  -h, --help          show this help
  -i, --infty         run in infinity capturing loop (else - request each frame)
  -k, --shmkey=arg    shared memory (with image data) key (default: 7777777)
  -n, --nframes=arg   make series of N frames (2 default)
  -s, --sock=arg      command socket name or port
  -x, --exptime=arg   set exposure time to given value (seconds!)
```
