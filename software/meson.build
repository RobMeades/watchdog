project(
  'watchdog',
  'cpp',
  version: '0.0.1'
)

add_global_arguments('-Wno-unused-function', language : 'cpp')

watchdog = executable('watchdog',
                      'watchdog.cpp',
                      dependencies: [dependency('libcamera', required: true),
                                     # All of the libav* things are FFMPEG
                                     dependency('libavformat', required: true),
                                     dependency('libavcodec', required: true),
                                     dependency('libavdevice', required: true),
                                     dependency('libavutil', required: true),
                                     dependency('opencv4', required: true)])
