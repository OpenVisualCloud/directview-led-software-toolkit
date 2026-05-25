/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * libFuzzer harness for the JSON config parser.
 * Used by ClusterFuzzLite for continuous fuzzing (detected by OpenSSF Scorecard).
 *
 * Build: clang -fsanitize=fuzzer,address -g -O1 -I../include \
 *        fuzz_config_reader_libfuzzer.c ../src/util/config_reader.c \
 *        ../src/util/logger.c -lavutil -lm -o fuzz_config_reader_libfuzzer
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/config_reader.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > (1 << 20)) {
        return 0;
    }

    /* Write fuzz input to a temporary file for parse_tx_config */
    char tmpfile[] = "/tmp/fuzz_cfg_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        return 0;
    }

    if (write(fd, data, size) != (ssize_t)size) {
        close(fd);
        unlink(tmpfile);
        return 0;
    }
    close(fd);

    struct dvledtx_config config;
    memset(&config, 0, sizeof(config));
    parse_tx_config(tmpfile, &config);
    validate_tx_config(&config);

    unlink(tmpfile);
    return 0;
}
