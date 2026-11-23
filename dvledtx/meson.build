# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# TX path selection — see the root meson_options.txt for details.
enable_mtl_tx = get_option('enable_mtl_tx')
tx_c_args = ['-DALLOW_EXPERIMENTAL_API', '-D_FORTIFY_SOURCE=2', '-O2']
if enable_mtl_tx
  tx_c_args += ['-DENABLE_MTL_TX']
endif

tx_deps = [m_dep, pthread_dep, libavformat_dep, libavcodec_dep, libavutil_dep,
           libswscale_dep, avdevice_dep, mtl_dep]

executable('dvledtx',
  sources: [
    'src/main.c',
    'src/util/config_reader.c',
    'src/core/session_manager.c',
    'src/ffmpeg/ffmpeg_decoder.c',
    'src/ffmpeg/ffmpeg_frame_handler.c',
    'src/ffmpeg/ffmpeg_tx.c',
    'src/mtl/mtl_tx.c',
    'src/util/logger.c',
  ],
  include_directories: include_directories('include'),
  dependencies: tx_deps,
  c_args: tx_c_args,
  link_args: ['-s'],
  build_rpath: '',
  install: false
)

subdir('tests')
