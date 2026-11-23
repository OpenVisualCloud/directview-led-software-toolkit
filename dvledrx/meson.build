# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2022 Intel Corporation

# This subdirectory defines the receiver application and its nested tests.
rx_cc = meson.get_compiler('c')

rx_x11_dep = dependency('x11', required: true)
rx_xext_dep = dependency('xext', required: true)
rx_xrandr_dep = dependency('xrandr', required: true)
rx_json_c_dep = dependency('json-c', required: true)
rx_zlib_dep = dependency('zlib', required: true)

rx_c_args = [
  '-DALLOW_EXPERIMENTAL_API',
  '-DDVLEDRX_VERSION="' + meson.project_version() + '"',
  '-D_FORTIFY_SOURCE=2',
  '-O3',
]
foreach arg : ['-march=native', '-funroll-loops']
  if rx_cc.has_argument(arg)
    rx_c_args += [arg]
  endif
endforeach
rx_mtl_include_args = ['-I/usr/local/include/mtl']
rx_deps = [mtl_dep, m_dep, pthread_dep, rx_x11_dep, rx_xext_dep,
           rx_xrandr_dep, rx_json_c_dep, rx_zlib_dep]

executable('dvledrx',
  sources: [
    'src/main.c',
    'src/frame_converter.c',
    'src/x11_handler.c',
    'src/config_reader.c',
    'src/session_manager.c',
    'src/logger.c',
  ],
  include_directories: include_directories('include'),
  dependencies: rx_deps,
  c_args: rx_c_args + rx_mtl_include_args,
  link_args: ['-s'],
  install: false
)

subdir('tests')
