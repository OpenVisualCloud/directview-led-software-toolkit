/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * libFuzzer harness for the receiver JSON config parser and validator.
 */

#include "fuzz_config_reader_common.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rx_fuzz_one_input(data, size);
  return 0;
}
