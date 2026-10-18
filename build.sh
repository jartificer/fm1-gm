#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the firmware package (build/fm1gm.fwsc). Needs JIELI_TOOLCHAIN and AC79_SDK: see BUILDING.md.
#   ./build.sh [--release X.Y[.Z]]
cd "$(dirname "$0")" && exec python3 tools/build.py "$@"
