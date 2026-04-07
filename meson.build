project('TxApp', 'c', version: '1.0.0')

# Compiler
cc = meson.get_compiler('c')

# Dependencies
m_dep = cc.find_library('m', required: true)
pthread_dep = cc.find_library('pthread', required: true)

# FFmpeg dependencies
libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec',  required: true)
libavutil_dep   = dependency('libavutil',   required: true)
libswscale_dep  = dependency('libswscale',  required: true)
libavdevice_dep = dependency('libavdevice', required: true)  # MTL Kahawai muxer lives here

# Include directories
inc_dirs = include_directories('.')

# Compiler arguments
c_args = ['-DALLOW_EXPERIMENTAL_API']

all_deps = [m_dep, pthread_dep,
            libavformat_dep, libavcodec_dep, libavutil_dep, libswscale_dep, libavdevice_dep]

# Build executable
executable('TxApp',
  sources: [
    'src/tx_app_main.c',
    'src/config_reader.c',
    'src/session_manager.c',
    'src/ffmpeg/ffmpeg_decoder.c'
  ],
  include_directories: include_directories('include'),
  dependencies: all_deps,
  c_args: ['-DALLOW_EXPERIMENTAL_API'],
  install: false

)
