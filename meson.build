# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

project('dvledtx', 'c', version: '1.0.0')

cc = meson.get_compiler('c')

m_dep       = cc.find_library('m',       required: true)
pthread_dep = cc.find_library('pthread', required: true)

libavformat_dep = dependency('libavformat', required: true)
libavcodec_dep  = dependency('libavcodec',  required: true)
libavutil_dep   = dependency('libavutil',   required: true)
libswscale_dep  = dependency('libswscale',  required: true)

# TX path selection — see meson_options.txt for details.
enable_mtl_tx = get_option('enable_mtl_tx')

# mtl_tx.c always calls mtl_init()/mtl_uninit() (unconditionally, not just
# under ENABLE_MTL_TX) so that the FFmpeg avdevice path can pre-register all
# NIC ports with DPDK EAL before the mtl_st20p muxer opens its own session —
# see the comment above mtl_tx_init() in src/mtl/mtl_tx.c. MTL is therefore a
# required dependency for both TX paths, not just the direct-MTL one.
#
# Its include path comes entirely from `dependency('mtl')`'s own pkg-config
# Cflags (which already resolves to wherever MTL headers are installed, e.g.
# `-I/usr/local/include -I/usr/local/include/mtl`) — no hardcoded include
# path is added here, so this works regardless of MTL's install location.
mtl_dep      = dependency('mtl', required: true)
avdevice_dep = []
c_args       = ['-DALLOW_EXPERIMENTAL_API', '-D_FORTIFY_SOURCE=2', '-O2']

if enable_mtl_tx
  c_args      += ['-DENABLE_MTL_TX']
else
  avdevice_dep = dependency('libavdevice', required: true)
endif

all_deps = [m_dep, pthread_dep,
            libavformat_dep, libavcodec_dep, libavutil_dep, libswscale_dep,
            avdevice_dep, mtl_dep]

executable('dvledtx',
  sources: [
    'src/main.c',
    'src/util/config_reader.c',
    'src/core/session_manager.c',
    'src/ffmpeg/ffmpeg_decoder.c',
    'src/ffmpeg/ffmpeg_frame_handler.c',
    'src/ffmpeg/ffmpeg_tx.c',
    'src/mtl/mtl_tx.c',
    'src/util/logger.c'
  ],
  include_directories: include_directories('include'),
  dependencies: all_deps,
  c_args: c_args,
  link_args: ['-s'],
  build_rpath: '',
  install: false
)
