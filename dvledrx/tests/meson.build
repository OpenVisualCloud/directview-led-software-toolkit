# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

rx_test_cc = meson.get_compiler('c')
rx_test_m_dep = rx_test_cc.find_library('m', required: true)
rx_test_pthread_dep = rx_test_cc.find_library('pthread', required: true)
rx_test_mtl_dep = dependency('mtl', required: true)
rx_test_x11_dep = dependency('x11', required: true)
rx_test_xext_dep = dependency('xext', required: true)
rx_test_xrandr_dep = dependency('xrandr', required: true)
rx_test_json_c_dep = dependency('json-c', required: true)
rx_test_zlib_dep = dependency('zlib', required: true)

rx_test_c_args = [
  '-DALLOW_EXPERIMENTAL_API',
  '-DDVLEDRX_VERSION="' + meson.project_version() + '"',
  '-O3',
  '-I/usr/local/include/mtl',
]
foreach arg : ['-march=native', '-funroll-loops']
  if rx_test_cc.has_argument(arg)
    rx_test_c_args += [arg]
  endif
endforeach

rx_test_source_dir = meson.current_source_dir() / '..'
rx_test_config_dir = rx_test_source_dir / 'config'
rx_test_data_dir = meson.current_source_dir() / 'data'
rx_common_test_deps = [rx_test_m_dep, rx_test_pthread_dep, rx_test_x11_dep,
                       rx_test_xext_dep, rx_test_xrandr_dep, rx_test_json_c_dep,
                       rx_test_zlib_dep]

rx_unit_tests = {
  'config_reader': [
    ['test_config_reader.c', '../src/config_reader.c', '../src/logger.c'],
    [rx_test_json_c_dep, rx_test_zlib_dep, rx_test_pthread_dep], [rx_test_config_dir],
  ],
  'logger': [
    ['test_logger.c', '../src/logger.c'],
    [rx_test_zlib_dep, rx_test_pthread_dep], [],
  ],
  'frame_converter': [
    ['test_frame_converter.c', '../src/frame_converter.c', '../src/logger.c'],
    [rx_test_mtl_dep, rx_test_zlib_dep, rx_test_pthread_dep], [],
  ],
}
foreach name, t : rx_unit_tests
  test('dvledrx_' + name,
    executable('test_dvledrx_' + name, t[0],
      include_directories: include_directories('../include'),
      dependencies: t[1],
      c_args: rx_test_c_args,
      build_by_default: false),
    args: t[2],
    suite: 'dvledrx-unit')
endforeach

# These tests link fake_mtl.c instead of libmtl, so they need no NIC/DPDK hardware.
rx_mtl_headers = rx_test_mtl_dep.partial_dependency(compile_args: true, includes: true)
rx_app_test_deps = [rx_mtl_headers] + rx_common_test_deps
rx_app_test_srcs = [
  '../src/session_manager.c', '../src/x11_handler.c', '../src/frame_converter.c',
  '../src/config_reader.c', '../src/logger.c', 'fake_mtl.c',
]
rx_app_test_inc = include_directories('../include', '.')

rx_fake_app = executable('dvledrx_fake', ['../src/main.c'] + rx_app_test_srcs,
  include_directories: rx_app_test_inc,
  dependencies: rx_app_test_deps,
  c_args: rx_test_c_args,
  build_by_default: false)

rx_cli_cases = {
  'cli_help':           [['--help'], true],
  'cli_version':        [['--version'], false],
  'cli_no_config':      [[], true],
  'cli_config_missing': [['--config', 'does-not-exist.json'], true],
  'cli_config_invalid': [['-C', rx_test_source_dir / 'meson.build'], true],
  'cli_test_time_zero': [['-C', rx_test_data_dir / 'run_single.json', '-t', '0'], true],
  'cli_test_time_text': [['-C', rx_test_data_dir / 'run_single.json', '--test-time', '5s'], true],
  'cli_test_time_long': [['-C', rx_test_data_dir / 'run_single.json', '-t', '86401'], true],
  'cli_unknown_option': [['-C', rx_test_data_dir / 'run_single.json', '--bogus'], true],
  'cli_missing_value':  [['-C'], true],
}
foreach name, c : rx_cli_cases
  test('dvledrx_' + name, rx_fake_app,
    args: c[0], should_fail: c[1], suite: 'dvledrx-unit')
endforeach

# Window tests use a virtual X server; CI installs xvfb and xauth for these.
rx_xvfb_run = find_program('xvfb-run', required: false)
if rx_xvfb_run.found()
  rx_xvfb_args = ['-a', '-s', '-screen 0 1280x720x24 -nolisten tcp']

  foreach name, srcs : {
    'session_manager': ['test_session_manager.c'] + rx_app_test_srcs,
    'x11': ['test_x11.c', '../src/x11_handler.c', '../src/frame_converter.c',
            '../src/config_reader.c', '../src/logger.c'],
  }
    test('dvledrx_' + name, rx_xvfb_run,
      args: rx_xvfb_args + [executable('test_dvledrx_' + name, srcs,
        include_directories: rx_app_test_inc,
        dependencies: rx_app_test_deps,
        c_args: rx_test_c_args,
        build_by_default: false)],
      suite: 'dvledrx-unit', timeout: 120)
  endforeach

  rx_run_cases = {
    'run_single':        ['run_single.json',     ['-t', '1'], {}, false],
    'run_composite':     ['run_composite.json',  ['-t', '1'], {}, false],
    'run_per_output':    ['run_per_output.json', ['-t', '1'], {}, false],
    'run_fps24':         ['run_fps24.json',      ['-t', '1'], {}, false],
    'run_fps29':         ['run_fps29.json',      ['-t', '1'], {}, false],
    'run_fps59':         ['run_fps59.json',      ['-t', '1'], {}, false],
    'run_sigterm':       ['run_fps30.json',      ['-t', '20'], {'FAKE_MTL_RAISE_AFTER': '20'}, false],
    'run_mtl_init_fail': ['run_single.json',     [], {'FAKE_MTL_FAIL_INIT': '1'}, true],
    'run_create_fail':   ['run_per_output.json', ['-t', '20'], {'FAKE_MTL_FAIL_CREATE': '1'}, true],
  }
  foreach name, r : rx_run_cases
    test('dvledrx_' + name, rx_xvfb_run,
      args: rx_xvfb_args + [rx_fake_app, '--config', rx_test_data_dir / r[0]] + r[1],
      env: r[2], should_fail: r[3], suite: 'dvledrx-unit', timeout: 60)
  endforeach
else
  message('xvfb-run not found: receiver X11, session manager and app-run tests are skipped')
endif
