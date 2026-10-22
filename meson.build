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

# MTL is required by both transmit and receive applications.
mtl_dep      = dependency('mtl', required: true)

# libavdevice is needed regardless of the TX path: the mtl_st20p muxer TX
# path uses it directly, and the MTL-native TX path still needs it for the
# x11grab screen-capture input decoder (avdevice_register_all()).
avdevice_dep = dependency('libavdevice', required: true)

# Each application owns its Meson definition and its tests subdirectory.
subdir('dvledtx')
subdir('dvledrx')
