project(
  'watchdog',
  'cpp',
  version: '0.0.1',
  default_options : ['c_std=c11', 'cpp_std=c++11']
)

add_global_arguments(['-DW_CAMERA_ROTATED_180', '-Wno-unused-function'], language : 'cpp')

watchdog = executable('watchdog',
                      'w_util.cpp', 'w_gpio.cpp', 'w_motor.cpp', 'w_msg.cpp', 'w_led.cpp', 'w_camera.cpp', 'w_image_processing.cpp', 'w_video_encode.cpp', 'w_control.cpp', 'w_command_line.cpp', 'w_cfg.cpp', 'w_main.cpp',
                      dependencies: [dependency('libcamera', required: true),
                                     # All of the libav* things are FFMPEG
                                     dependency('libavformat', required: true),
                                     dependency('libavcodec', required: true),
                                     dependency('libavdevice', required: true),
                                     dependency('libavutil', required: true),
                                     dependency('opencv4', required: true),
                                     dependency('libgpiod', required: true),
                                     dependency('libcjson', required: true)])
