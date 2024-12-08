# Introduction
Here you will find all of the software aspects, running in C++, using [libcamera](https://libcamera.org/), [OpenCV](https://opencv.org/) and [FFmpeg](https://www.ffmpeg.org/).  I did try doing this stuff from Python on a PiZero2W but it was taking soooo lonnnngggggg: up to 60&nbsp;seconds just to import the Python modules, hence I switched to C instead.  Then I found that the PiZero2W wasn't quite up to running an H.264 video encoder as well as capturing the video: at 854x480-YUV420 resolution it was getting something like 22&nbsp;FPS through the encoder with some encode durations being several seconds, at 1920x1080-YUV420 it would often stall entirely.  So I made room in the watchdog for a Pi&nbsp;5 with 8&nbsp;gigabytes of RAM (a steady 25&nbsp;fps encoded video at 1920x1080-YUV420, worst-case encode time 60&nbsp;ms) but I continued with C++ 'cos I'd already passed through that pain barrier.

Note that these instructions are for the V3 Pi camera; this camera can ONLY be accessed through `libcamera`, which the Python module [PiCamera2](https://github.com/raspberrypi/picamera2) wraps, and which [OpenCV](https://opencv.org/) does NOT understand natively (different from the V2 camera, which [OpenCV](https://opencv.org/) could use directly).

In writing the software here I was guided by:

- [this tutorial](https://learnopencv.com/moving-object-detection-with-opencv) from Learn OpenCV (in Python),
- [this article](https://pyimagesearch.com/2019/09/02/opencv-stream-video-to-web-browser-html-page/) about using a V3 Pi camera for video monitoring (from Python),
- [this article](https://medium.com/@vladakuc/hls-video-streaming-from-opencv-and-ffmpeg-828ca80b4124) about streaming using [FFmpeg](https://www.ffmpeg.org/),
- OpenCV's [excellent tutorials](https://docs.opencv.org/4.x/d9/df8/tutorial_root.html), particularly the ones on [background subtraction](https://docs.opencv.org/4.x/d1/dc5/tutorial_background_subtraction.html) and [contour detection](https://docs.opencv.org/4.x/df/d0d/tutorial_find_contours.html),
- the equally excellent [documentation for libcamera](https://libcamera.org/guides/application-developer.html),
- the source code for [libcamera's cam application](https://git.libcamera.org/libcamera/libcamera.git/tree/src/apps/cam).

# Installation
For the Pi, use the [Raspberry PI Imager](https://www.raspberrypi.com/news/raspberry-pi-imager-imaging-utility/) to write the headless Raspbian distribution to SD card (with your Wifi details pre-entered for ease of first use and SSH access enabled); insert the SD card into the Pi and power it up.

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

Install [OpenCV](https://opencv.org/) and its development libraries with:

```
sudo apt install python3-opencv libopencv-dev
```

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

As a browser interface, being a traditionalist, I would suggest installing Apache with:

```
sudo apt install apache2
```

Enable Apache to run at boot with:

```
sudo systemctl enable apache2
```

...and edit the file `/etc/apache2/sites-available/000-default.conf` to set `DocumentRoot` to wherever you plan to run the `watchdog` executable; best not to put this in your own home directory as permissions get awkward, put it somehere like `/home/http/`.  You probably also need to add in the same Apache configuration file:

```
        <Directory your_document_root>
            AllowOverride none
            Require all granted
        </Directory>
```

Whatever directory you choose, to make it accessible to the default Apache user, `www-data`:

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

# Build/Run
Clone this repo to the Pi, `cd` to the directory where you cloned it, then `cd` to this sub-directory and build/run the application with:

```
meson setup build
cd build
ninja
./watchdog
```

To run with maximum debug from [libcamera](https://libcamera.org/), use:

```
LIBCAMERA_LOG_LEVELS=0 ./watchdog
```

Otherwise, the default (log level 1) is to run with information, warning and error messages from [libcamera](https://libcamera.org/) but not debug messages.

# Serve
To serve video, copy `*.png` and `*.html` from this directory, plus the built `watchdog` executable, to the directory you have told Apache to serve pages from and run `./watchdog` from there.

# A Note On Developing
Since this is a simple application, a single source file, I just used `nano` as an editor on the Pi itself and had an `sftp` session running from a PC so that I could `get` the single source file I was working on from there and do all of the archive pushing stuff on the PC.  The only thing to remember was to open the source file on the PC in something like [Notepad++](https://notepad-plus-plus.org/) and do an `Edit`->`EOL Conversion` to switch the file to Windows line-endings on the PC before doing any pushing.  For larger edits I did it the other way around: edit the `.cpp` file on the PC and `sftp`->`put` the file to the Pi before compiling.  You can open the file in `nano` on the Pi and `CTRL-O` to write the file but press `ALT-D` before you press `<enter>` to commit the write to change to Linux line endings; that said, the `meson` build system and GCC worked fine with Windows line endings on Linux.