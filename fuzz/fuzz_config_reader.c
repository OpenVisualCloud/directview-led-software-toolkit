/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * AFL fuzzing harness for the JSON config parser.
 * Build:  afl-gcc -o fuzz_config_reader fuzz_config_reader.c ../src/util/config_reader.c
 *         ../src/util/logger.c -I../include $(pkg-config --cflags --libs libavutil) -lm
 * Run:    afl-fuzz -i corpus/ -o findings/ -- ./fuzz_config_reader @@
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/config_reader.h"

#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

int main(int argc, char *argv[]) {
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

    while (__AFL_LOOP(10000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        if (len < 2) continue;

        /* Write fuzz input to a temporary file for parse_tx_config */
        char tmpfile[] = "/tmp/fuzz_cfg_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd < 0) continue;
        write(fd, buf, len);
        close(fd);

        struct dvledtx_config config;
        memset(&config, 0, sizeof(config));
        parse_tx_config(tmpfile, &config);
        validate_tx_config(&config);

        unlink(tmpfile);
    }
#else
    /* Standard mode: read from file argument */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <json_config_file>\n", argv[0]);
        return 1;
    }

    struct dvledtx_config config;
    memset(&config, 0, sizeof(config));
    parse_tx_config(argv[1], &config);
    validate_tx_config(&config);
#endif

    return 0;
}
