#!/bin/sh
# Riptide - compare src/amiga's OS/SDK definitions with the reference NDK
# and SDK headers and FD files (see check_layouts.py).
#   NDK=/path/NDK3.2 MIAMI_SDK=/path/MiamiSDK AMITCP_SDK=/path/AmiTCP-SDK-4.3 \
#     tests/amiga/check_layouts.sh
# SPDX-License-Identifier: GPL-2.0-only
exec python3 "$(dirname "$0")/check_layouts.py" "$@"
