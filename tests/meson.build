# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

project('dvledtx-tests', 'c')

cc = meson.get_compiler('c')

m_dep       = cc.find_library('m',       required: true)
pthread_dep = cc.find_library('pthread', required: true)

libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec',  required: true)
libavutil_dep   = dependency('libavutil',   required: true)
libswscale_dep  = dependency('libswscale',  required: true)
avdevice_dep    = dependency('libavdevice', required: true)
mtl_dep         = dependency('mtl',         required: false)

cmocka_dep = dependency('cmocka', required: false, method: 'pkg-config')

if not cmocka_dep.found()
  subdir_done()
endif

all_deps    = [m_dep, pthread_dep,
               libavformat_dep, libavcodec_dep, libavutil_dep, libswscale_dep,
               avdevice_dep]
fixture_dir = meson.project_source_root() / '../config'
test_inc    = include_directories('../include', '/usr/local/include/mtl')
test_deps   = all_deps + [cmocka_dep]
test_c_args = ['-DALLOW_EXPERIMENTAL_API']

test_config_reader = executable('test_config_reader',
  sources: [
    'test_config_reader.c',
    '../src/util/config_reader.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args + ['-DFIXTURE_DIR="' + fixture_dir + '"'],
  install: false
)
test('config_reader', test_config_reader)

test_logger = executable('test_logger',
  sources: [
    'test_logger.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: [m_dep, pthread_dep, cmocka_dep],
  install: false
)
test('logger', test_logger)

test_ffmpeg_decoder = executable('test_ffmpeg_decoder',
  sources: [
    'test_ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  install: false
)
test('ffmpeg_decoder', test_ffmpeg_decoder)

test_ffmpeg_frame_handler = executable('test_ffmpeg_frame_handler',
  sources: [
    'test_ffmpeg_frame_handler.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  install: false
)
test('ffmpeg_frame_handler', test_ffmpeg_frame_handler)

test_ffmpeg_tx = executable('test_ffmpeg_tx',
  sources: [
    'test_ffmpeg_tx.c',
    '../src/ffmpeg/ffmpeg_tx.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false
)
test('ffmpeg_tx', test_ffmpeg_tx)

# session_manager.c compiled in; ffmpeg_decoder.c excluded — mocks provided in the test file.
test_session_manager = executable('test_session_manager',
  sources: [
    'test_session_manager.c',
    '../src/core/session_manager.c',
    '../src/mtl/mtl_tx.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps + (mtl_dep.found() ? [mtl_dep] : []),
  c_args: test_c_args,
  install: false
)
test('session_manager', test_session_manager)

# main.c is included directly in the test file to expose static functions.
# session_manager.c and config_reader.c excluded — stubs provided in the test file.
test_main = executable('test_main',
  sources: [
    'test_main.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  install: false
)
test('main', test_main)

# av_guess_format wrapped to return the 'null' muxer instead of mtl_st20p.
test_ffmpeg_decoder_mock = executable('test_ffmpeg_decoder_mock',
  sources: [
    'test_ffmpeg_decoder_mock.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false
)
test('ffmpeg_decoder_mock', test_ffmpeg_decoder_mock)

test_ffmpeg_tx_mock = executable('test_ffmpeg_tx_mock',
  sources: [
    'test_ffmpeg_tx_mock.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/ffmpeg/ffmpeg_tx.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false
)
test('ffmpeg_tx_mock', test_ffmpeg_tx_mock)

# Watchdog tests: --wrap=av_read_frame to inject perpetual decode failure.
test_ffmpeg_watchdog = executable('test_ffmpeg_watchdog',
  sources: [
    'test_ffmpeg_watchdog.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_frame_handler.c',
    '../src/util/logger.c',
  ],
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  link_args: ['-Wl,--wrap=av_read_frame',
              '-Wl,--wrap=av_guess_format'],
  install: false
)
test('ffmpeg_watchdog', test_ffmpeg_watchdog)

if mtl_dep.found()
  test_mtl_tx = executable('test_mtl_tx',
    sources: [
      'test_mtl_tx.c',
      '../src/mtl/mtl_tx.c',
      '../src/util/logger.c',
    ],
    include_directories: [test_inc, include_directories('/usr/local/include/mtl')],
    dependencies: test_deps + [mtl_dep],
    c_args: test_c_args + ['-DENABLE_MTL_TX'],
    install: false
  )
  test('mtl_tx', test_mtl_tx)
endif
