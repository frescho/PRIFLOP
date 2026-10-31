# Source this file:   . tools/env.sh
#
# Points the build and dev tools at the copies in tools/external/ (see the
# README there), so nothing depends on paths in one person's home directory.
# It OVERRIDES an already-set PICO_SDK_PATH on purpose: sourcing it means "use
# the bundled toolchain". To use your own SDK, simply don't source this file.
# shellcheck shell=bash

_priflop_ext="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/external"

export PICO_SDK_PATH="$_priflop_ext/pico-sdk"

# z80asm/zcc for the Primo tools. (Never set $Z80ASM: z80asm reads it as extra arguments.)
export ZCCCFG="$_priflop_ext/z88dk/lib/config"
export PATH="$_priflop_ext/z88dk/bin:$PATH"

# picotool is only used at build time, to turn the ELF into a UF2. The SDK's own
# fallback FETCHES it from GitHub (the moving 'develop' branch), and it ignores
# FETCHCONTENT_FULLY_DISCONNECTED, so a fresh build directory needs the network.
# Pointing it at this folder makes the SDK use the pre-installed, pinned copy in
# picotool/ instead: no download, no rebuild. See tools/external/README.md.
export PICOTOOL_FETCH_FROM_GIT_PATH="$_priflop_ext/picotool"

# C64 ROM images for VICE (x64sc -kernal/-basic/-chargen).
export VICE_ROM_DIR="$_priflop_ext/vice-roms/C64"

unset _priflop_ext
