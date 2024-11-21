# Introduction
Here you will find all of the software aspects, running in Python, using [PiCamera2](https://github.com/raspberrypi/picamera2), [OpenCV](https://opencv.org/), [matplotlib](https://matplotlib.org/) and [gradio](https://www.gradio.app/).  Note that these instructions are for the V3 Pi camera; this camera can ONLY be accessed through `libcamera` (which [PiCamera2](https://github.com/raspberrypi/picamera2) wraps) and which [OpenCV](https://opencv.org/) does NOT understand natively (different from the V2 camera, which [OpenCV](https://opencv.org/) could use directly).

# Installation
For the PiZero2W, use the [Raspberry PI Imager](https://www.raspberrypi.com/news/raspberry-pi-imager-imaging-utility/) to write the headless Raspbian distribution for PiZero2W to SD card (with your Wifi details pre-entered for ease of first use and SSH access enabled); insert the SD card into the Pi and power it up.

Throughout this section, `ssh` (or [PuTTY](https://www.putty.org/) if you prefer) is used to access the Pi and `sftp` (or [FileZilla](https://filezilla-project.org/) if you prefer) to download (`get`) files from the Pi, so make sure that both of those work to access the Pi from another computer on your LAN, a computer that has a display attached.

Power the Pi down again and plug in the V3 camera.  Power-up the Pi once more, log in over `ssh` and check that an image can be captured from the camera with:

```
rpicam-jpeg --output test.jpg
```

`sftp`->`get` the `test.jpg` file and view it to check that it contains a good image.

Install [PiCamera2](https://github.com/raspberrypi/picamera2), [OpenCV](https://opencv.org/) and [matplotlib](https://matplotlib.org/) with:

```
sudo apt install python3-picamera2
sudo apt install python3-opencv
sudo apt install python3-matplotlib
```

Start a Python3 interpreter and check that you can capture an image from the camera in Python with:

```
from picamera2 import Picamera2
picam2 = Picamera2()
camera_config = picam2.create_still_configuration(main={"size": (1920, 1080)}, lores={"size": (640, 480)}, display="lores")
picam2.configure(camera_config)
picam2.start()
picam2.capture_file("test_from_python.jpg")
```

`sftp`->`get` the `test_from_python.jpg` file and check that it is a good image.

To check that [OpenCV](https://opencv.org/) can handle image data (as a `NumPy` array), add the following lines of Python to the above in the same Python interpreter session:

```
import cv2
image_array=picam2.capture_array()
cv2.imwrite("test_through_opencv.jpg", image_array)
```

`sftp`->`get` the `test_through_opencv.jpg` file and check that it is a good image.

The rest of the application is based on LearnOpenCV's article "[moving object detection with OpenCV](https://learnopencv.com/moving-object-detection-with-opencv)".  In that article they use [gradio](https://www.gradio.app/) to provide a web app interface, which seemed very useful when operating headless.  Install [gradio](https://www.gradio.app/) with:

```
sudo apt install python3-dev python3-setuptools libtiff5-dev libjpeg-dev libopenjp2-7-dev zlib1g-dev libfreetype6-dev liblcms2-dev libwebp-dev tcl8.6-dev tk8.6-dev python3-tk libharfbuzz-dev libfribidi-dev libxcb1-dev libopenblas-dev
python -m venv gradio-env --system-site-packages
source gradio-env/bin/activate
pip install gradio
```

Note: the first line installs a load of pre-requisites for `pillow`, which is compiled as part of the [gradio](https://www.gradio.app/) installation.
Note: `--system-site-packages` allows the `gradio-env` virtual environment to see the system-wide Python packages you installed further up; if you later want to install a package locally in the virtual environment, overriding a system one, use `pip install --ignore-installed blah`.

Open a new Python interpreter and run the following, which is largely the [gradio](https://www.gradio.app/) [standard test application](https://github.com/gradio-app/gradio?tab=readme-ov-file#building-your-first-demo):

```python
from picamera2 import Picamera2
import cv2 as cv
import matplotlib.pyplot as plt
import gradio as gr

def greet(name, intensity):
    return "Hello, " + name + "!" * int(intensity)

demo = gr.Interface(
    fn=greet,
    inputs=["text", "slider"],
    outputs=["text"],
)

demo.launch(server_name="0.0.0.0")
```

[gradio](https://www.gradio.app/) makes its applications available on port 7860, so check that it has worked by accessing the PiZero2W from a browser on another machine of your LAN with:

```
http://ip-address-of-PiZero2W:7860/
```