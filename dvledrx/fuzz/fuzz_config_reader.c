/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * AFL++ harness for the receiver JSON config parser.
 * Build: afl-clang-fast -g -O1 -I../include fuzz_config_reader.c \
 *        ../src/config_reader.c ../src/logger.c -pthread \
 *        $(pkg-config --cflags --libs json-c zlib) -o fuzz_config_reader
 * Run:   afl-fuzz -i corpus/ -o findings/ -- ./fuzz_config_reader @@
 */

#include "fuzz_config_reader_common.h"

#include <stdio.h>
#include <string.h>

#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

static void fuzz_file(const char* filename) {
  FILE* input = fopen(filename, "rb");
  if (!input)
    return;

  uint8_t* data = malloc(RX_FUZZ_MAX_INPUT + 1u);
  if (!data) {
    fclose(input);
    return;
  }

  size_t size = fread(data, 1, RX_FUZZ_MAX_INPUT + 1u, input);
  if (!ferror(input) && size <= RX_FUZZ_MAX_INPUT)
    rx_fuzz_one_input(data, size);

  free(data);
  fclose(input);
}

int main(int argc, char** argv) {
#ifdef __AFL_HAVE_MANUAL_CONTROL
  __AFL_INIT();
  unsigned char* data = __AFL_FUZZ_TESTCASE_BUF;
  while (__AFL_LOOP(1000)) {
    size_t size = __AFL_FUZZ_TESTCASE_LEN;
    rx_fuzz_one_input(data, size);
  }
#else
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <json_config_file>\n", argv[0]);
    return 1;
  }
  fuzz_file(argv[1]);
#endif
  return 0;
}
