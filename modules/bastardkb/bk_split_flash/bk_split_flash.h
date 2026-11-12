// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bk_split_flash: split secondary flash copy (Phase 0 walking skeleton).
// This header is the boundary between the protocol/transport logic (bk_split_flash.c)
// and the action/flash writer (bk_split_flash_flash.c, Phase 1).
#pragma once

#include <stdint.h>
#include <stddef.h>

// Protocol version for the capability probe/echo exchanged in Phase 0.
#define BK_SPLIT_FLASH_PROTOCOL_VERSION 1
// Master -> slave capability probe.
typedef struct {
    uint8_t magic;   // BK_SPLIT_FLASH_MAGIC, sanity check
    uint8_t version; // BK_SPLIT_FLASH_PROTOCOL_VERSION
    uint8_t seq;     // echoed back so the round-trip is observable
} bk_split_flash_probe_t;

// Slave -> master echo reply.
typedef struct {
    uint8_t magic;   // BK_SPLIT_FLASH_MAGIC
    uint8_t version; // BK_SPLIT_FLASH_PROTOCOL_VERSION
    uint8_t seq;     // mirrored from the probe
    uint8_t status;  // BK_SPLIT_FLASH_STATUS_* - capability/ack
} bk_split_flash_echo_t;

#define BK_SPLIT_FLASH_MAGIC 0x5F
#define BK_SPLIT_FLASH_STATUS_OK 0x00
