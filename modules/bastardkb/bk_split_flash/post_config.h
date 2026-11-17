// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Argos owns the EEPROM block holding the firmware fingerprint. */
#ifndef COMMUNITY_MODULE_ARGOS_ENABLE
#    error "bk_split_flash module requires the bastardkb/argos module"
#endif
