CCD/CMOS imaging server
=======================

Supports FLI cameras/focusers/wheels and cameras: ZWO, Basler, HikRobot, PointGrey, Apogee, ToupCam.
Allows to run as standalone application or imaging server/client.

To restart server (e.g. if hardware was off) kill it with SIGUSR1 or send command `restartTheServer`.

## Compile
The tool itself depends on [usefull_macros](https://github.com/eddyem/snippets_library) librariy.
"Artifical star" plugin depends on my [improclib](https://github.com/eddyem/improclib).
The image viewer depends on OpenGL and GLUT libraries (also depending on X11).
All other plugins (excluding "dummy camera") depends on third-party libraries. The Apogee plugin also
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
- `-DTOUPCAM=ON`  - compile ToupCam support plugin
- `-DZWO=ON` - compile ZWO support plugin

## Usage

```
Usage: ccd_capture [args] [output file prefix]
  Args are:
  -0, --timeout=arg           network answer timeout (default: 0.1s)
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
  -V, --verbose               verbose level (-V - main messages, -VV - secondary messages, -VVV - debug)
  -W, --wheeldev=arg          wheel device plugin (e.g. devdummy.so)
  -Y, --objtype=arg           object type (neon, object, flat etc)
  -_, --plugincmd=arg         custom camera device plugin command (can occur multiple times)
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
  -r, --addrec=arg            add records to header from given file[s] (can occur multiple times)
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
  --host=arg                  server's host name
  --imageport=arg             INET image socket port (default: port+1)
  --infty=arg                 start (!=0) or stop(==0) infinity capturing loop
  --logfile=arg               logging file name (if run as server)
  --open-shutter              open shutter
  --plugin=arg                common device plugin (e.g devfli.so)
  --port=arg                  INET command socket port (omit if use on same host with server)
  --restart                   restart image server
  --rewrite                   rewrite output file if exists
  --server                    run as server
  --set-fan=arg               set fan speed (0 - off, 1 - low, 2 - high)
  --shutter-on-high           run exposition on HIGH @ pin5 I/O port
  --shutter-on-low            run exposition on LOW @ pin5 I/O port
  --viewer                    passive viewer (only get last images)
  --wheeldevno=arg            filter wheel device number (if many: 0, 1, 2 etc)
```

Notes:

- A UNIX command socket named `@ccd_capture` is created/used automatically
  (abstract namespace, no file on disk). There is no `--path` option; the
  name is hardcoded in `socket.h` as `UNIX_SOCK_NAME`.
- If neither `--port` nor `--imageport` is given, the client receives images
  through shared memory (see `-k/--shmkey`). To force image transmission over
  INET, point `--host`, `--port` and/or `--imageport` (this replaces the removed `--forceimsock`).
  If server run without `--port`, there's only shared memory mechanism to grab images for client.
- If `--port` is given without `--imageport`, the latter is set to `port+1` automatically.


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
- a way to accept commands:
  - UNIX socket `@ccd_capture` - always opened by the server; the local client
    connects to it by default when no `--port` is given;
  - optionally an INET command socket, via `--port=port`;
- optionally an INET image socket, via `--imageport=port`; if you point `--port`
  but not `--imageport`, the latter defaults to `port+1`;
- shared memory key for fast local image transmission, `-k=key` (default: `7777777`).

To send commands to server you can use the client, `netcat` session, my
[tty_term](https://github.com/eddyem/tty_term) or any other tool. The server speaks a simple
line-based text protocol (send `help` to see the full list).

### Protocol
Each request is a single line, terminated by `\n`:

- `key`            - getter or command without arguments
- `key=value`      - setter

The server answers with one line (or several, for `help`, `camlist`, `foclist`,
`wlist`):

- `OK`      - the command completed successfully
- `BUSY`    - cannot run the command now (camera is busy)
- `FAIL`    - some error occurred
- `BADKEY`  - unknown key
- `BADVAL`  - invalid value for the key
- `key=value` - for getters (and setters, as echo of the new value)

### Supported commands

| Command             | Description |
|---------------------|-------------|
| `brightness[=V]`    | get/set camera brightness |
| `camdevno[=N]`      | get/set active camera number |
| `camfanspeed[=N]`   | get/set camera fan speed (0..2) |
| `camflags`          | get flags: bit0 - start capture, bit1 - cancel, bit2 - restart server |
| `camlist`           | list all connected cameras (one `camlist='model'` line per camera) |
| `confio[=N]`        | configure I/O port |
| `dark[=0/1]`        | don't open shutter on exposure (dark frames) |
| `expstate[=N]`      | see below |
| `exptime[=V]`       | get/set exposure time (s) |
| `fastspeed[=0/1]`   | get/set fast readout mode |
| `focdevno[=N]`      | get/set active focuser |
| `foclist`           | list all connected focusers |
| `focmaxpos`         | get maximal focuser position |
| `focminpos`         | get minimal focuser position |
| `focpos[=V]`        | get/set focuser position (mm) |
| `foctemp`           | get focuser body temperature |
| `format[=X0,Y0,X1,Y1]` | get/set camera frame format |
| `gain[=V]`          | get/set camera gain |
| `hbin[=N]`          | get/set horizontal binning |
| `help`              | show this help |
| `imnumber`          | number of grabbed images since the server start |
| `imwidth` / `imheight` | width/height of the last image |
| `infty[=0/1]`       | run/stop the infinity capturing loop |
| `io[=N]`            | get/set I/O port state |
| `lowres[=0/1]`      | run in 8-bit mode instead of 16-bit |
| `maxformat`         | get maximal available frame format |
| `nflushes[=N]`      | get/set number of pre-flushes |
| `plugincmd=KEY[=VAL]` | custom camera plugin command |
| `restartTheServer`  | restart the server |
| `shmemkey`          | get shared memory key |
| `shutter=N`         | run shutter command (0=open, 1=close, 2=expose @HIGH, 3=expose @LOW) |
| `tcold[=V]`         | get/set cold-side temperature; also returns `tbody=` and `thot=` if supported |
| `tremain`           | remaining exposure time (s) |
| `vbin[=N]`          | get/set vertical binning |
| `wdevno[=N]`        | get/set active wheel |
| `wlist`             | list all connected wheels |
| `wmaxpos`           | get maximal wheel position |
| `wpos[=N]`          | get/set wheel position |
| `wtemp`             | get wheel body temperature |


`expstate` values:

- `0` - idle (sending `expstate=0` cancels the current exposition);
- `1` - capturing (sending `expstate=1` starts an exposition);
- `2` - frame is ready (invalid for setter);
- `3` - error (invalid for setter).

Server doesn't sends to client any asynchronous data.

### Custom plugin commands
Since version 1.2.0 a custom plugin command system is available. To list the available commands,
run the client/server with `--plugincmd=help`, or send `plugincmd=help` to a running server.
Commands can be procedures/flags (`plugincmd=cmd`) or setters/getters (`plugincmd=key` and
`plugincmd=key=value`).

To transmit these custom commands to the server use `plugincmd=key[=value]`.

## Client
To run the client you should point `--client` and the same port options as for
the server (command socket and image SHM key or image socket):

- `--host=HOST` (optional) - server host name / IP for INET command socket;
- `--port=PORT` (optional) - INET command socket port; if omitted, the client
  connects to the UNIX socket `@ccd_capture` on the local host;
- `--imageport=PORT` (optional) - INET image socket port; if omitted, the client
  receives images through the shared memory segment `-k/--shmkey`;
- `-k/--shmkey=KEY` (optional) - shared memory key for image data.

Client modes:

- with `--viewer` - opens the OpenGL viewer and passively receives
  images from the server (in this mode the client cannot start expositions);
  to produce images continuously the server must be set to `infty=1`, or other
  clients must send `expstate=1`;
- with `--display` (or `-D`) - opens the OpenGL window and continuously requests server for 
  new images (you should point `--exptime=T` (or `-x T`) for specific exposition time;
- without viewer options - sends the given options to the server and exits;
  if you specify `--outfile`, `--outfileprefix` or `--nframes>0`, the client
  starts an exposition, waits for the image, and optionally saves it as FITS.

`--forceimsock` was removed; use `--port` and optionally `--imageport` to force image transfer over
INET socket instead of shared memory


## Dummy camera plugin
The plugin simply emulates the acquisition process: images are a 2-D sinusoid
with given periods, shifted by one pixel each frame.

Commands:

- px = (double) [1, inf] - set/get sin period over X axis (pix)
- py = (double) [1, inf] - set/get sin period over Y axis (pix)


## Artifical star plugin
The plugin emulates a star field with up to `MAX_STARS` (32) stars. You can shift the center of the
field to emulate telescope correction and rotate the field to emulate derotation. All stars are
Moffat profiles with the same FWHM and scale parameters. Their coordinates (arcsec) are stored in
arrays `x` and `y`, with magnitudes in `mag`. You can emulate image drift, rotation, short-exposure
position fluctuations, and Poisson noise (`lambda==1` means "no noise").

Two additional commands - `mask` and `bkg` - allow you to load a binary mask and a background
image. The background (8-bit monochrome) is added to the generated star field. The mask is then
AND-ed with the result: zero mask pixels force the output to zero, non-zero pixels are preserved.
After that, the bias level is added and (optionally) Poisson noise is applied to the full frame.

Commands:

-   beta = (double) [0.5, inf] - Moffat `beta` parameter
-   bias = (int) [0, 30000] - bias value
-   bkg = (string) - load background image
-   curstar = (int) [0, MAX_STARS-1] - set number of current star to change parameters
-   fluct = (double) [0, 3] - stars position fluctuations (arcsec per sec)
-   fwhm = (double) [0.1, 10] - stars min FWHM, arcsec
-   lambda = (double) [1, inf] - Poisson noice lambda value (>1)
-   lambda0 = (double) [1, inf] - Poisson noice lambda value (>1) for dark noise
-   mag = (double) [-30, 30] - Next star magnitude: 0m is 0xffff/0xff (16/8 bit) ADUs per second
-   mask = (string) - load mask image (binary, ANDed)
-   nstars = (int) [1, MAX_STARS] - set amount of stars (not more than MAX_STARS)
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
Connects to the server, optionally enables `infty`, and takes up to `N` images (or quits if no
image is available for a few seconds).

Usage:
```
-N, --node=arg      server's host (if working over net) or UNIX-socket path
-P, --import=arg    image socket port (if not 'port+1'
-h, --help          show this help
-i, --infty         run in infinity capturing loop (else - request each frame)
-k, --shmkey=arg    shared memory (with image data) key (default: 7777777)
-n, --nframes=arg   make series of N frames
-p, --port=arg      command socket port
-x, --exptime=arg   set exposure time to given value (seconds!)
```

### centroids / centroids16
Get images from the server (only over shared memory) and calculate the gravity center and moments
of the first-order/second-order brightness. `centroids` works only in 8-bit mode and `centroids16`
only in 16-bit mode.

Usage:
```
-N, --node=arg         INET command socket host or UNIX socket name
-b, --background=arg   fixed background level
-h, --help             show this help
-i, --infty            run in infinity capturing loop (else - request each frame)
-k, --shmkey=arg       shared memory (with image data) key (default: 7777777)
-n, --nframes=arg      make series of N frames (default: 10)
-o, --output=arg       output file with T/x/y/w
-p, --port=arg         port of INET command socket
-s, --sigmas=arg       amount of sigmas [-5..15] added to image median for background calculation (default: 1)
-x, --exptime=arg      set exposure time to given value (seconds!)
```

Option `--sigmas` works only for 16-bit version, as in 8-bit background calculation use 
`il_Image_background` (calculates background by histogram curve).

## Known issues

- The server requires at least one command socket (`@ccd_capture` UNIX socket
  is always created; INET command socket is optional).
- If both `--outfile` and an output-file prefix (positional argument) are given,
  the program exits.
- The image viewer uses OpenGL and freeglut; it needs an X11 display.

## License

This repository is provided under the GNU General Public License v3 or later.
Full text of the GPLv3 is available at <https://www.gnu.org/licenses/gpl-3.0.html>.
