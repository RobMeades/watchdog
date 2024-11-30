# Introduction
Here you will find all of the software aspects, running in C++, using [libcamera](https://libcamera.org/) and [OpenCV](https://opencv.org/).  I did try doing this stuff from Python but it was taking soooo lonnnngggggg: up to 60 seconds just to import the Python modules, hence I switched to C instead.  Note that these instructions are for the V3 Pi camera; this camera can ONLY be accessed through `libcamera`, which the Python module [PiCamera2](https://github.com/raspberrypi/picamera2) wraps, and which [OpenCV](https://opencv.org/) does NOT understand natively (different from the V2 camera, which [OpenCV](https://opencv.org/) could use directly).

In writing the software here I was guided by:

- [this tutorial](https://learnopencv.com/moving-object-detection-with-opencv) from Learn OpenCV (in Python),
- [this article](https://pyimagesearch.com/2019/09/02/opencv-stream-video-to-web-browser-html-page/) about using a V3 Pi camera for video monitoring (from Python),
- OpenCV's [excellent tutorials](https://docs.opencv.org/4.x/d9/df8/tutorial_root.html), particularly the ones on [background subtraction](https://docs.opencv.org/4.x/d1/dc5/tutorial_background_subtraction.html) and [contour detection](https://docs.opencv.org/4.x/df/d0d/tutorial_find_contours.html),
- the equally excellent [documentation for libcamera](https://libcamera.org/guides/application-developer.html),
- the source code for [libcamera's cam application](https://git.libcamera.org/libcamera/libcamera.git/tree/src/apps/cam).

# Installation
For the PiZero2W, use the [Raspberry PI Imager](https://www.raspberrypi.com/news/raspberry-pi-imager-imaging-utility/) to write the headless Raspbian distribution for PiZero2W to SD card (with your Wifi details pre-entered for ease of first use and SSH access enabled); insert the SD card into the Pi and power it up.  It is worth waiting a good 10 minutes for the PiZero2W to sort itself out and become available to `ssh` on your Wifi network at first boot; I've no idea why.

Throughout this section, `ssh` (or [PuTTY](https://www.putty.org/) if you prefer) is used to access the Pi and `sftp` (or [FileZilla](https://filezilla-project.org/) if you prefer) to download (`get`) files from the Pi, so make sure that both of those work to access the Pi from another computer on your LAN, a computer that has a display attached.

Log into the Pi over `ssh`.  Make sure the Pi is up to date with:

```
sudo apt update
sudo apt upgrade
```

Power the Pi down again and plug in the V3 camera.  Power-up the Pi once more, log in over `ssh` and check that an image can be captured from the camera with:

```
rpicam-jpeg --output test.jpg
```

`sftp`->`get` the `test.jpg` file and view it to check that it contains a good image.

Install the dependencies required to develop with [libcamera](https://libcamera.org/):

```
sudo apt install git cmake meson ninja-build pkg-config libyaml-dev python3-yaml python3-ply python3-jinja2 libssl-dev openssl libdw-dev libunwind-dev libudev-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libpython3-dev pybind11-dev libevent-dev libdrm-dev libjpeg-dev libsdl2-dev pybind11-dev
```

Fetch and build [libcamera](https://libcamera.org/) (this will take a few hours) with:

```
git clone https://git.libcamera.org/libcamera/libcamera.git
cd libcamera
meson setup build
sudo ninja -C build install
```

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

`cam -c 1 -p`

...displays the properties of camera 1 while:

`cam -c 1 -I`

...displays the pixel formats supported, etc.

Finally, install [OpenCV](https://opencv.org/) and its development libraries with:

```
sudo apt install python3-opencv libopencv-dev
```

# Build/Run
Clone this repo to the PiZero2W, `cd` to the directory where you cloned it, then `cd` to this sub-directory and build/run the application with:

```
meson setup build
cd build
ninja
./watchdog
```

To run with maximum debug, use:

```
LIBCAMERA_LOG_LEVELS=0 ./watchdog
```

Otherwise, the default (log level 1) is to run with information, warning and error messages but not debug messages.

# A Note On Developing
I couldn't find a way to set up Github authentication (required to push code back to Github) on a 32-bit ARM Linux Raspberry Pi, which is what the PiZero2W is: most of the applications required to store authentication keys don't seem to be available for that platform combination and I didn't fancy compiling them myself.  Since this is a simple application, a single source file, I just used `nano` as an editor on the Pi itself and had an `sftp` session running from a PC so that I could `get` the single source file I was working on from there and do all of the archive pushing stuff on the PC.  The only thing to remember was to open the source file on the PC in something like [Notepad++](https://notepad-plus-plus.org/) and do an `Edit`->`EOL Conversion` to switch the file to Windows line-endings on the PC before doing any pushing.