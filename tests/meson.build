# =============================================================================
# Tests — only built when cmocka is available
# =============================================================================
cmocka_dep = dependency('cmocka', required: false)

if not cmocka_dep.found()
  message('cmocka not found; skipping test targets')
  subdir_done()
endif

fixture_dir = meson.project_source_root() / 'config'

test_config_reader = executable('test_config_reader',
  sources: [
    'test_config_reader.c',
    '../src/config_reader.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: all_deps + [cmocka_dep],
  c_args: [
    '-DALLOW_EXPERIMENTAL_API',
    '-DFIXTURE_DIR="' + fixture_dir + '"',
  ],
  install: false
)

test('config_reader', test_config_reader)

test_logger = executable('test_logger',
  sources: [
    'test_logger.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: [m_dep, pthread_dep, cmocka_dep],
  install: false
)

test('logger', test_logger)

test_ffmpeg_decoder = executable('test_ffmpeg_decoder',
  sources: [
    'test_ffmpeg_decoder.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: all_deps + [cmocka_dep],
  c_args: ['-DALLOW_EXPERIMENTAL_API'],
  install: false
)

test('ffmpeg_decoder', test_ffmpeg_decoder)

# test_session_manager: session_manager.c compiled in; ffmpeg_decoder.c
# intentionally excluded — mocks are provided inside the test file itself.
test_session_manager = executable('test_session_manager',
  sources: [
    'test_session_manager.c',
    '../src/session_manager.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: all_deps + [cmocka_dep],
  c_args: ['-DALLOW_EXPERIMENTAL_API'],
  install: false
)

test('session_manager', test_session_manager)

# test_tx_app_main: includes tx_app_main.c directly (via #include in the test
# file) to expose its static functions.  session_manager.c and config_reader.c
# are intentionally excluded — stubs are provided inside the test file.
test_tx_app_main = executable('test_tx_app_main',
  sources: [
    'test_tx_app_main.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: all_deps + [cmocka_dep],
  c_args: ['-DALLOW_EXPERIMENTAL_API'],
  install: false
)

test('tx_app_main', test_tx_app_main)

# test_ffmpeg_decoder_mock: exercises open_ffmpeg_output, open_shared_ffmpeg,
# open_ffmpeg_source, shared_decode_thread, and ffmpeg_decode_and_send by
# wrapping av_guess_format to return the "null" muxer instead of mtl_st20p,
# and using a tiny generated MPEG2 video for the input paths.
test_ffmpeg_decoder_mock = executable('test_ffmpeg_decoder_mock',
  sources: [
    'test_ffmpeg_decoder_mock.c',
    '../src/ffmpeg/ffmpeg_decoder.c',
    '../src/util/logger.c',
  ],
  include_directories: include_directories('../include'),
  dependencies: all_deps + [cmocka_dep],
  c_args: ['-DALLOW_EXPERIMENTAL_API'],
  link_args: ['-Wl,--wrap=av_guess_format'],
  install: false
)

test('ffmpeg_decoder_mock', test_ffmpeg_decoder_mock)
