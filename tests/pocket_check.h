// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Minimal check macro for the pocket host tests; active regardless of NDEBUG.
#pragma once

#include <stdio.h>
#include <stdlib.h>

static int pocket_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        pocket_checks++;                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

#define CHECK_DONE(name) printf("%s: %d checks passed\n", name, pocket_checks)
