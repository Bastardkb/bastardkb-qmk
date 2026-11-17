# SPDX-License-Identifier: GPL-2.0-or-later
# bk_split_flash.c is auto-included by the community module build (same basename
# as the module folder); no extra sources are needed.

# Gate both QMK's normal master sync and RPCs issued directly by community
# modules while split flash owns the wire. transport_execute_transaction() is
# deliberately not wrapped, so the clone's own transactions remain available.
LDFLAGS += -Wl,--wrap=transport_master
LDFLAGS += -Wl,--wrap=transaction_rpc_exec
