# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

cmocka_dep = dependency('cmocka', required: false, method: 'pkg-config')
if not cmocka_dep.found()
  subdir_done()
endif

tx_test_deps = [m_dep, pthread_dep, libavformat_dep, libavcodec_dep,
                libavutil_dep, libswscale_dep, avdevice_dep, cmocka_dep]
tx_fixture_dir = meson.current_source_dir() / '../config'
tx_test_inc = include_directories('../include', '/usr/local/include/mtl')
tx_test_c_args = ['-DALLOW_EXPERIMENTAL_API']

tx_test_config_reader = executable('test_config_reader',
  sources: ['test_config_reader.c', '../src/util/config_reader.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args + ['-DFIXTURE_DIR="' + tx_fixture_dir + '"'],
  install: false,
  build_by_default: false
)
test('config_reader', tx_test_config_reader)

tx_test_logger = executable('test_logger',
  sources: ['test_logger.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: [m_dep, pthread_dep, cmocka_dep],
  install: false,
  build_by_default: false
)
test('logger', tx_test_logger)

tx_test_ffmpeg_decoder = executable('test_ffmpeg_decoder',
  sources: ['test_ffmpeg_decoder.c', '../src/ffmpeg/ffmpeg_decoder.c',
            '../src/ffmpeg/ffmpeg_frame_handler.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  install: false,
  build_by_default: false
)
test('ffmpeg_decoder', tx_test_ffmpeg_decoder)

tx_test_ffmpeg_frame_handler = executable('test_ffmpeg_frame_handler',
  sources: ['test_ffmpeg_frame_handler.c', '../src/ffmpeg/ffmpeg_frame_handler.c',
            '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  install: false,
  build_by_default: false
)
test('ffmpeg_frame_handler', tx_test_ffmpeg_frame_handler)

tx_test_ffmpeg_tx = executable('test_ffmpeg_tx',
  sources: ['test_ffmpeg_tx.c', '../src/ffmpeg/ffmpeg_tx.c',
            '../src/ffmpeg/ffmpeg_frame_handler.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false,
  build_by_default: false
)
test('ffmpeg_tx', tx_test_ffmpeg_tx)

# session_manager.c compiled in; ffmpeg_decoder.c excluded — mocks provided in the test file.
tx_test_session_manager = executable('test_session_manager',
  sources: ['test_session_manager.c', '../src/core/session_manager.c',
            '../src/mtl/mtl_tx.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps + (mtl_dep.found() ? [mtl_dep] : []),
  c_args: tx_test_c_args,
  install: false,
  build_by_default: false
)
test('session_manager', tx_test_session_manager)

# main.c is included directly in the test file to expose static functions.
tx_test_main = executable('test_main',
  sources: ['test_main.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  install: false,
  build_by_default: false
)
test('main', tx_test_main)

tx_test_ffmpeg_decoder_mock = executable('test_ffmpeg_decoder_mock',
  sources: ['test_ffmpeg_decoder_mock.c', '../src/ffmpeg/ffmpeg_decoder.c',
            '../src/ffmpeg/ffmpeg_frame_handler.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false,
  build_by_default: false
)
test('ffmpeg_decoder_mock', tx_test_ffmpeg_decoder_mock)

tx_test_ffmpeg_tx_mock = executable('test_ffmpeg_tx_mock',
  sources: ['test_ffmpeg_tx_mock.c', '../src/ffmpeg/ffmpeg_decoder.c',
            '../src/ffmpeg/ffmpeg_frame_handler.c', '../src/ffmpeg/ffmpeg_tx.c',
            '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false,
  build_by_default: false
)
test('ffmpeg_tx_mock', tx_test_ffmpeg_tx_mock)

tx_test_ffmpeg_watchdog = executable('test_ffmpeg_watchdog',
  sources: ['test_ffmpeg_watchdog.c', '../src/ffmpeg/ffmpeg_decoder.c',
            '../src/ffmpeg/ffmpeg_frame_handler.c', '../src/util/logger.c'],
  include_directories: tx_test_inc,
  dependencies: tx_test_deps,
  c_args: tx_test_c_args,
  link_args: ['-Wl,--wrap=av_read_frame', '-Wl,--wrap=av_guess_format'],
  install: false,
  build_by_default: false
)
test('ffmpeg_watchdog', tx_test_ffmpeg_watchdog)

if mtl_dep.found()
  tx_test_mtl = executable('test_mtl_tx',
    sources: ['test_mtl_tx.c', '../src/mtl/mtl_tx.c', '../src/util/logger.c'],
    include_directories: [tx_test_inc, include_directories('/usr/local/include/mtl')],
    dependencies: tx_test_deps + [mtl_dep],
    c_args: tx_test_c_args + ['-DENABLE_MTL_TX'],
    install: false,
    build_by_default: false
  )
  test('mtl_tx', tx_test_mtl)

  tx_test_mtl_stub = executable('test_mtl_tx_stub',
    sources: ['test_mtl_tx_stub.c', '../src/mtl/mtl_tx.c', '../src/util/logger.c'],
    include_directories: [tx_test_inc, include_directories('/usr/local/include/mtl')],
    dependencies: tx_test_deps + [mtl_dep],
    c_args: tx_test_c_args + ['-DENABLE_MTL_TX'],
    link_args: ['-Wl,--wrap=mtl_init', '-Wl,--wrap=mtl_uninit',
                '-Wl,--wrap=mtl_pmd_by_port_name', '-Wl,--wrap=st20p_tx_create',
                '-Wl,--wrap=st20p_tx_free', '-Wl,--wrap=st20p_tx_frame_size',
                '-Wl,--wrap=st20p_tx_get_frame', '-Wl,--wrap=st20p_tx_put_frame'],
    install: false,
    build_by_default: false
  )
  test('mtl_tx_stub', tx_test_mtl_stub)
endif
