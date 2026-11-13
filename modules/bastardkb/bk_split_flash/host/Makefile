# Copyright 2026 bastardkb
# SPDX-License-Identifier: GPL-2.0-or-later
# Native host harness for the pure safety-core logic (CRC-32, length/magic).
# Not part of the QMK build - run with `make test`.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2

.PHONY: all test clean check-asserts

all: test
	./test

test: test_bk_split_flash.c ../bk_split_flash.h
	$(CC) $(CFLAGS) -o $@ test_bk_split_flash.c

# The config-level ports/transport guards must fire with clear messages.
check-asserts:
	@printf '#define SPLIT_FLASH_MAX_IMAGE_SIZE (256u * 1024)\n#include "../bk_split_flash.h"\n' > .assert_probe.c
	@if $(CC) $(CFLAGS) -DEE_HANDS=1 -c .assert_probe.c -o /dev/null 2>.assert_err.txt; then echo "FAIL: EE_HANDS guard did not fire"; exit 1; fi
	@if ! grep -q "EE_HANDS" .assert_err.txt; then echo "FAIL: EE_HANDS message missing"; cat .assert_err.txt; exit 1; fi
	@echo "EE_HANDS guard ok"
	@if $(CC) $(CFLAGS) -DUSE_I2C=1 -c .assert_probe.c -o /dev/null 2>.assert_err.txt; then echo "FAIL: USE_I2C guard did not fire"; exit 1; fi
	@if ! grep -q "only the serial split transport" .assert_err.txt; then echo "FAIL: USE_I2C message missing"; cat .assert_err.txt; exit 1; fi
	@echo "USE_I2C guard ok"
	@rm -f .assert_probe.c .assert_err.txt

clean:
	rm -f test .assert_probe.c .assert_err.txt