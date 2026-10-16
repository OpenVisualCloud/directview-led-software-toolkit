/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include "config_reader.h"
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define RX_FUZZ_MAX_INPUT (1u << 20)

static inline void rx_fuzz_one_input(const uint8_t* data, size_t size) {
  if ((!data && size != 0) || size > RX_FUZZ_MAX_INPUT)
    return;

  char path[] = "/tmp/dvledrx_fuzz_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return;

  size_t written = 0;
  while (written < size) {
    ssize_t rc = write(fd, data + written, size - written);
    if (rc < 0 && errno == EINTR)
      continue;
    if (rc <= 0) {
      close(fd);
      unlink(path);
      return;
    }
    written += (size_t)rc;
  }

  if (close(fd) != 0) {
    unlink(path);
    return;
  }

  struct rx_config config;
  logger_set_level(LOG_LEVEL_ERROR);
  if (parse_rx_config(path, &config) == 0)
    (void)validate_rx_config(&config);

  unlink(path);
}
