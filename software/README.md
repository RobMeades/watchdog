# Introduction
Here you will find all of the software aspects, running in C++, using [libcamera](https://libcamera.org/), [OpenCV](https://opencv.org/) and [FFmpeg](https://www.ffmpeg.org/).  I did try doing this stuff from Python on a PiZero2W but it was taking soooo lonnnngggggg: up to 60&nbsp;seconds just to import the Python modules, hence I switched to C instead.  Then I found that the PiZero2W wasn't quite up to running an H.264 video encoder as well as capturing the video: at 854x480-YUV420 resolution it was getting something like 22&nbsp;FPS through the encoder with some encode durations being several seconds, at 1920x1080-YUV420 it would often stall entirely.  So I made room in the watchdog for a Pi&nbsp;5 with 8&nbsp;gigabytes of RAM (a steady 25&nbsp;fps encoded video at 1920x1080-YUV420, worst-case encode time 60&nbsp;ms, though by the time everything else was running I had scaled that to 950x540-YUV420 and 15&nbsp;fps) but I continued with C++ 'cos I'd already passed through that pain barrier.

Note that these instructions are for the V3 Pi camera; this camera can ONLY be accessed through `libcamera`, which the Python module [PiCamera2](https://github.com/raspberrypi/picamera2) wraps, and which [OpenCV](https://opencv.org/) does NOT understand natively (different from the V2 camera, which [OpenCV](https://opencv.org/) could use directly).

In writing the software here I was guided by:

- [this tutorial](https://learnopencv.com/moving-object-detection-with-opencv) from Learn OpenCV (in Python),
- [this article](https://pyimagesearch.com/2019/09/02/opencv-stream-video-to-web-browser-html-page/) about using a V3 Pi camera for video monitoring (from Python),
- [this article](https://medium.com/@vladakuc/hls-video-streaming-from-opencv-and-ffmpeg-828ca80b4124) about streaming using [FFmpeg](https://www.ffmpeg.org/),
- OpenCV's [excellent tutorials](https://docs.opencv.org/4.x/d9/df8/tutorial_root.html), particularly the ones on [background subtraction](https://docs.opencv.org/4.x/d1/dc5/tutorial_background_subtraction.html) and [contour detection](https://docs.opencv.org/4.x/df/d0d/tutorial_find_contours.html),
- the equally excellent [documentation for libcamera](https://libcamera.org/guides/application-developer.html),
- the source code for [libcamera's cam application](https://git.libcamera.org/libcamera/libcamera.git/tree/src/apps/cam).

# Implementation Notes
All of my stuff here is really in C rather than C++, despite the `.cpp` extension.  I did try moving to C++, 'cos I like the name-spacing, but you can't initialise flexible arrays (of which I use quite a few) in a C++ class and C++ contructors can't return errors, making them unsuitable for HW-oriented things that can fail, so you end up having to have `init()` methods anyway, `#define`s (which are nice 'cos you can modify them by passing values to the compiler command-line) aren't name-spaced in C++, etc.  Hence I've name-spaced the code in the usual C way (by prefixing function and variable names sensibly); that will have to do.

The files, and structure, of the embedded stuff are as follows:

- each API is formed by a pair of `.h`/.`cpp` files: so for instance the `wCamera` API is contained in the [w_camera.h](w_camera.h)/[w_camera.cpp](w_camera.cpp) file pair,
- an API may include a pair of `wXxxInit()`/`wXxxDeinit()` functions that should be called at start/end of day by `main()`,
- the important APIs are [wCamera](w_camera.h), [wImageProcessing](w_image_processing.h) and [wVideoEncode](w_video_encode.h): [wVideoEncode](w_video_encode.h) is the start, so calling `wVideoEncodeStart()` will in turn call `wImageProcessingStart()`, which will in turn call `wCameraStart()` and image frames will be taken from the camera, processed and written to HLS format video files (see also [w_hls.h](w_hls.h)) in a directory of your choice,
- the [wMsg](w_msg.h) API forms a key piece of infrastructure, allowing data and commands to be queued \[by the APIs themselves under a function-calling shim\], providing asynchronous behaviour.
- the [wMotor](w_motor.h) API controls the stepper motors and the [wLed](w_led.h) API controls the LEDs that form the watchdog's eyes,
- the [wGpio](w_gpio.h) API provides access to the Raspberry Pi's GPIO pins for [wMotor](w_motor.h) and [wLed](w_led.h),
- the [wCfg](w_cfg.h) API manages a JSON configuration file (`watchdog.cfg`) which allows control of whether the motors or the lights can be operated, on the basis of a weekly schedule and/or manual overrides.
- miscellaneous utils can be found in [wUtil](w_util.h) (in particular a function that starts a real-time task that is driven by an accurate periodic tick, a pattern used throughout the code), debug logging in [wLog](w_log.h) and a small number of common definitions in [wCommon](w_common.h),
- to make the program more usable, [wCommandLine](w_command_line.h) provides command-line parsing and help,
- [wControl](w_control.h) coordinates it all and [w_main.cpp](w_main.cpp) brings it all together as an executable thing.

Along with the above, [index.html](index.html) provides a web interface, including a load of JavaScript in [index.js](index.js) and CSS styles in [styles.css](styles.css) to (a) display the streamed HLS video and (b) give read/write access to the `watchdog.cfg` configuration file from wherever, as prettily and accessibly and mobile-compatibilitily as I could manage in a week of sweating over mushy JavaScript.

# Installation
For the Pi, use the [Raspberry PI Imager](https://www.raspberrypi.com/news/raspberry-pi-imager-imaging-utility/) to write the headless Raspbian distribution to SD card (with your Wifi details pre-entered for ease of first use and SSH access enabled); insert the SD card into the Pi and power it up.

Throughout this section, `ssh` (or [PuTTY](https://www.putty.org/) if you prefer) is used to access the Pi and `sftp` (or [FileZilla](https://filezilla-project.org/) if you prefer) to download (`get`) files from the Pi, so make sure that both of those work to access the Pi from another computer on your LAN, a computer that has a display attached.

Log into the Pi over `ssh`.  Make sure the Pi is up to date with:

```
sudo apt update
sudo apt upgrade
```

## Camera
Power the Pi down again and plug in the V3 camera.  Power-up the Pi once more, log in over `ssh` and check that an image can be captured from the camera with:

```
rpicam-jpeg --output test.jpg
```

`sftp`->`get` the `test.jpg` file and view it to check that it contains a good image.

Install the dependencies required to develop with [libcamera](https://libcamera.org/):

```
sudo apt install git cmake meson ninja-build pkg-config libyaml-dev python3-yaml python3-ply python3-jinja2 libssl-dev openssl libdw-dev libunwind-dev libudev-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libpython3-dev pybind11-dev libevent-dev libdrm-dev libjpeg-dev libsdl2-dev pybind11-dev
```

Fetch and build [libcamera](https://libcamera.org/) with:

```
git clone https://github.com/raspberrypi/libcamera.git
cd libcamera
meson setup build
sudo ninja -C build install
```

Note: normally you would clone from https://git.libcamera.org/libcamera/libcamera.git but the Pi 5 camera support is not there, hence the use of https://github.com/raspberrypi/libcamera.git.

Once that has cooked, run:

```
cam -l
```

...and you should see something like:

```
INFO Camera camera_manager.cpp:325 libcamera v0.3.2+27-7330f29b
WARN RPiSdn sdn.cpp:40 Using legacy SDN tuning - please consider moving SDN inside rpi.denoise
INFO RPI vc4.cpp:447 Registered camera /base/soc/i2c0mux/i2c@1/imx708@1a to Unicam device /dev/media3 and ISP device /dev/media0
Available cameras:
1: 'imx708_wide_noir' (/base/soc/i2c0mux/i2c@1/imx708@1a)
```

The little `cam` application does a few useful things, `cam -h` for help.  For example:

```
cam -c 1 -p
```

...displays the properties of camera 1 while:

```
cam -c 1 -I
```

...displays the pixel formats supported, etc.

## OpenCV
Install [OpenCV](https://opencv.org/) and its development libraries with:

```
sudo apt install python3-opencv libopencv-dev
```

## FFmpeg
Install the dependencies for [FFmpeg](https://www.ffmpeg.org/) as follows; you probably don't actually need half of these but it does no harm to have them:

```
sudo apt install imagemagick libasound2-dev libass-dev libavcodec-dev libavdevice-dev libavfilter-dev libavformat-dev libavutil-dev libfreetype6-dev libgmp-dev libmp3lame-dev libopencore-amrnb-dev libopencore-amrwb-dev libopus-dev librtmp-dev libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-net-dev libsdl2-ttf-dev libsnappy-dev libsoxr-dev libssh-dev libtool libv4l-dev libva-dev libvdpau-dev libvo-amrwbenc-dev libvorbis-dev libwebp-dev libx264-dev libx265-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb1-dev libxml2-dev lzma-dev nasm python3-dev python3-pip texinfo yasm zlib1g-dev libdrm-dev
```

Then fetch and compile what we need from [FFmpeg](https://www.ffmpeg.org/) with:

```
cd ~
git clone git://source.ffmpeg.org/ffmpeg --depth=1
cd ffmpeg
./configure --extra-ldflags="-lpthread -lm -latomic" --extra-cflags="-Wno-format-truncation" --arch=arm64 --target-os=linux --enable-gpl  --enable-pthreads --enable-libx264
make -j4
sudo make install
```

Note: for the PiZero2W replace `--arch=arm64` with `--arch=armel`.

## GPIO
To read and write GPIOs we need `libgpiod` (v1.6); install the development libraries with:

```
sudo apt install libgpiod-dev
```

## cJSON
The web interface configures the operation of the executable by writing a JSON file, for which [cJSON](https://github.com/DaveGamble/cJSON) is used.

Fetch, compile and install it with:

```
cd~
git clone https://github.com/DaveGamble/cJSON.git
cd cJSON
mkdir build
cd build
cmake ..
make
sudo make install
```

## Apache
To provide a browser interface, being ancient, I would suggest installing Apache with:

```
sudo apt install apache2
```

Enable Apache to run at boot with:

```
sudo systemctl enable apache2
```

`hls.js` says that CORS headers have to be added for it to work correctly; I'm not _sure_ this matters in such a local set up but, in case it does, run:

```
sudo a2enmod headers
```

...to enable the headers module of Apache; see also the additional `Header set` lines in the Apache configuration file below.

You will also need `mod_wsgi` to allow POST requests to be handled from the web client (to update the operating schedule); install and enable it with

```
sudo apt install libapache2-mod-wsgi-py3
```

Edit the file `/etc/apache2/sites-available/000-default.conf` to set `DocumentRoot` to wherever you plan to run the `watchdog` executable; best not to put this in your own home directory as permissions get awkward, put it somehere like `/home/http/`.

To handle POSTing changes to the schedule JSON file from the web client, you need to configure `mod_wsgi` to send any requests directed at the URL `/watchdog.cfg` to our script, `cfg.wsgi`.  Do this by adding the following to the top of `/etc/apache2/sites-available/000-default.conf`, just below the `DocumentRoot` stuff; note that the full path is needed to the `.wsgi` file and that the `.wsgi` file must be in the same folder as the file `watchdog.cfg`, which should be your `DocumentRoot`:

```
# Call the script cfg.wsgi when a POST/GET/whatever request is made to the file /watchdog.cfg
WSGIScriptAlias /watchdog.cfg <your_folder>/cfg.wsgi
```

You also need to add, after the above, in the same Apache configuration file:

```
<Directory your_document_root>
    AllowOverride none
    Require all granted
    # CORS headers that may or may not be needed for correct HLS operation
    Header set Access-Control-Allow-Origin "*"
    Header set Access-Control-Allow-Headers "*"
    Header set Access-Control-Allow-Methods "PUT, GET, POST, DELETE, OPTIONS"
</Directory>
```

Whatever directory you choose, to make it accessible to the default Apache user, "`www-data`":

```
sudo usermod -a -G www-data <your_user>
```

Log out and back in again, then:

```
sudo chown -R <your_user>:www-data <your_folder>
sudo chmod 2750 <your_folder>
```

Restart Apache for the changes to take effect:

```
sudo systemctl restart apache2
```

If Apache fails to restart, run:

```
journalctl -xe
```

...and stare long and hard at the output to find the buried error cause.  You might need to set `LogLevel` in the Apache configuration file to `debug` for the more subtle errors.

# Increasing SD Card Life
In order to increase the life of the SD card in the Pi, what with all of this video streaming activity, it is a good idea to create a RAM disk.  Edit `/etc/fstab` to append the following line, replacing `your_document_root` with wherever you put your Apache document root above; this will put the `video` sub-directory into RAM:

```
tmpfs your_document_root/video  tmpfs defaults,noatime,size=100m 0 0
```

If you are nervous about editing what is a vital file, run the following command to check that it is good before rebooting:

```
sudo findmnt --verify --verbose
```

If you haven't already created the directory `your_document_root/video` this will show up as an error; create it with `mkdir your_document_root/video`.

To activate the changes:

```
sudo systemctl daemon-reload
sudo mount -a
```

`df` should show a 100&nbsp;Mbyte `tmpfs` file system at `your_document_root/video`, e.g.:

```
tmpfs             102400       0    102400   0% /home/http/video
```

Since the watchdog will write logging output, and that will end up in the system log when watchdog is run as a service (see below), you should also move system logs to RAM, following the advice [here](https://pimylifeup.com/raspberry-pi-log2ram/#:~:text=When%20you%20reboot%20your%20Raspberry,at%20%E2%80%9C%20%2Fvar%2Fhdd.) i.e.:

```
sudo apt install rsync
wget https://github.com/azlux/log2ram/archive/master.tar.gz -O log2ram.tar.gz
tar xf log2ram.tar.gz
cd log2ram-master
sudo ./install.sh
```

Reboot for the changes to take effect and follow the instructions [here](https://github.com/azlux/log2ram?tab=readme-ov-file#is-it-working) to check that `log2ram` is working, primarily:

```
systemctl status log2ram
```

...and if it isn't working, take a look at the [troubleshooting section](https://github.com/azlux/log2ram?tab=readme-ov-file#troubleshooting).

You might also want to edit the Apache configuration file and re-direct its logging to the system log (following [this advice](https://www.loggly.com/ultimate-guide/centralizing-apache-logs/)) by changing the `ErrorLog` and `CustomLog` lines to be:

```
ErrorLog  "| /usr/bin/logger -thttpd -plocal6.err"
CustomLog "| /usr/bin/logger -thttpd -plocal6.notice"
```

Obviously restart Apache afterwards for the change to take effect.

# Build/Run
Clone this repo to the Pi, `cd` to the directory where you cloned it, then `cd` to this sub-directory and build/run the application with:

```
meson setup build
cd build
ninja
sudo ./watchdog
```

To run with maximum debug from [libcamera](https://libcamera.org/), use:

```
LIBCAMERA_LOG_LEVELS=0 sudo ./watchdog
```

Otherwise, the default (log level 1) is to run with information, warning and error messages from [libcamera](https://libcamera.org/) but not debug messages.

# Serve
To serve video, copy `*.png`, `*.html`, `*.js`, `*.css` and `*.wsgi` from this directory, plus the built `watchdog` executable, to the directory you have told Apache to serve pages from and run `sudo ./watchdog -d video` from there to put your video output files in the `video` sub-directory.  You may need to modify the permissions of the `watchdog.cfg` file that is created when `watchdog` is first run so that the file is writeable by the group that Apache belongs to (in order that the schedule can be written-to by the web API): do this with:

```
chmod -R g+rw watchdog.cfg
```

# Watchdog Service
To start the watchdog at boot, copy the file [watchdog.service](watchdog.service) from this directory, replacing `/home/http` with whatever you have chosen as `your_document_root`, into `/etc/systemd/system/`, then do:

```
sudo systemctl start watchdog
sudo systemctl enable watchdog
```

Note: if you get the `.service` file onto the Pi with `sftp` or the like then you will need to convert the line-endings to Linux or `systemd` won't like it, see "A Note On Developing" below for how to do that.

Check that the watchdog has started with:

```
sudo systemctl status watchdog
 ```

If it has not, take a look at what it said with:

```
sudo journalctl -u watchdog
```

...or, if it has, take a look at what it is saying with:

```
 sudo journalctl -u watchdog -f
```

# A Note On Developing
I edited the source files on a PC (in [Notepad++](https://notepad-plus-plus.org/)) and `sftp`->`put *` the files to the Pi before compiling in an `ssh` terminal to the Pi.  You can also open files in `nano` on the Pi and `CTRL-O` to write the file, but press `ALT-D` before you press `<enter>` to commit the write to change to Linux line-endings; that said, the `meson` build system and GCC worked fine with Windows line-endings on Linux.