// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The module's split transaction ID (injected into `enum serial_transaction_id`
// via the generated split_transaction_id_community_modules.inc).
#define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH

// RPC buffer sizing (global split RPC macros). m2s packets carry
// opcode + seq + payload + CRC; s2m returns the capability/ACK.
#ifndef RPC_M2S_BUFFER_SIZE
#    define RPC_M2S_BUFFER_SIZE 246
#endif
#ifndef RPC_S2M_BUFFER_SIZE
#    define RPC_S2M_BUFFER_SIZE 16
#endif
