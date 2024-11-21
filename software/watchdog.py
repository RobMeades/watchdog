#!/usr/bin/env python

'''The watchdog application.'''

print("Importing Python modules (may take a while).")
import sys, signal
import threading
from time import sleep
from picamera2 import Picamera2
import cv2 as cv
import gradio as gr
import numpy as np
import matplotlib.pyplot as plt

# Horizontal size of frame passed to OpenCV in pixels 
OPENCV_FRAME_HORIZONTAL_PIXELS = 1920

# Vertical size of frame passed to OpenCV in pixels 
OPENCV_FRAME_VERTICAL_PIXELS = 1080

# Horizontal size of low-res video stream in pixels
VIDEO_FRAME_HORIZONTAL_PIXELS = 640

# Vertical size of low-res video stream in pixels
VIDEO_FRAME_VERTICAL_PIXELS = 480

# Frames per second
FRAME_RATE=30

# Global lock variable and output frame
lock = threading.Lock()
output_frame = None

def generate_image():
        ''' Generate an image for gradio. '''
        global output_frame, lock
        image_object = None

    #while (1):
        print("### Waiting for lock")
        with lock:
            print("### Have lock")
            if output_frame is not None:
                print("### Have an output frame")
                # Return the image as a gradio image object containing a NumPy array
                image_object = gr.Image(value=output_frame, label="Camera")
                print("### Returning an image " + str(type(image_object)))
        return image_object

def video_processing():
    ''' The main video processing function. '''
    global output_frame, lock

    print("Opening the camera.")
    # Open the camera
    camera = Picamera2()
    print("Configuring PiCamera2 for capture..")
    # Configure the camera: a HD main stream in RGB888 for OpenCV processing and a lower res video stream
    camera_config = camera.create_video_configuration(main = {"format": "RGB888", "size": (OPENCV_FRAME_HORIZONTAL_PIXELS, OPENCV_FRAME_VERTICAL_PIXELS)}, lores = {"size": (VIDEO_FRAME_HORIZONTAL_PIXELS, VIDEO_FRAME_VERTICAL_PIXELS)}, controls={"FrameDurationLimits": (int(1000000/FRAME_RATE), int(1000000/FRAME_RATE))})
    # Optimise configuration for Python processing
    camera.align_configuration(camera_config)
    camera.configure(camera_config)
    # Start the camera
    camera.start()

    # Background remover
    background_subtract = cv.createBackgroundSubtractorMOG2()
    print("Capture started.")
    try:
        while (1):
            # Capture frame-by-frame
            frame = camera.capture_array()
            # Apply background subtraction
            mask = background_subtract.apply(frame)

            # TODO

            with lock:
                # Create the output frame
                output_frame = frame.copy()
    except KeyboardInterrupt as ex:
        # Tidy up
        camera.stop()
        print("Capture stopped.")
        raise KeyboardInterrupt from ex

if __name__ == "__main__":
    # Start a thread that will perform all of the video processing
    video_thread = threading.Thread(target=video_processing, daemon=True)
    video_thread.start()
    # Wait for video processing to start, keeps the debug prints nice and neat
    sleep(1)

    if video_thread.is_alive():
        # The gradio interface
        print("Starting web API (may take a while).")
        app = gr.Interface(fn=generate_image, inputs=None, outputs=["image"], flagging_mode="never", clear_btn=None)
        app.queue().launch(server_name="0.0.0.0")

    else:
        print("Unable to do video processing!");
