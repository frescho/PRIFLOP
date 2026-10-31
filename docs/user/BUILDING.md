# Building from source

This guide builds everything on a fresh **Ubuntu 26.04 LTS** (x86-64):
the firmware (`priflop.uf2`), the Primo boot file and PFM, and the host
tests. It was run as written, command by command, on a clean Ubuntu
26.04.1 cloud image.

Every command is typed in a terminal. The paths assume your home directory;
change them if you like, but keep them the same in every step.

## Step 1: the packages

```bash
sudo apt update
sudo apt install -y git build-essential cmake ninja-build perl python3 \
    gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

Ubuntu 26.04 brings `arm-none-eabi-gcc` 14.2, CMake 4.2 and GCC 15.

## Step 2: the build settings

The Pico SDK version this project uses (2.1.1) predates CMake 4 and
GCC 15. These three settings make it build on Ubuntu 26.04; set them in
every terminal you build in (or add them to `~/.bashrc`):

```bash
export CMAKE_POLICY_VERSION_MINIMUM=3.5
export CXXFLAGS="-include cstdint"
export PICO_SDK_PATH=$HOME/pico-sdk
```

- `CMAKE_POLICY_VERSION_MINIMUM`: CMake 4 refuses projects that ask for
  CMake older than 3.5; the SDK's mbedtls does.
- `CXXFLAGS`: the SDK's PIO assembler (`pioasm`, built for your PC during
  the firmware build) misses an `#include <cstdint>` that GCC 15 needs.

## Step 3: the source

```bash
git clone https://github.com/frescho/priflop.git $HOME/priflop
```

## Step 4: the Pico SDK 2.1.1

```bash
git clone --depth 1 --branch 2.1.1 --recurse-submodules --shallow-submodules \
    https://github.com/raspberrypi/pico-sdk.git $HOME/pico-sdk
```

Use 2.1.1: every build tested on hardware used it.

## Step 5: picotool

The build needs picotool to turn the firmware into a UF2 file. Without it,
the SDK downloads whatever picotool is newest at that moment. Build the
matching version once instead:

```bash
git clone --depth 1 --branch 2.1.1 https://github.com/raspberrypi/picotool.git $HOME/picotool-src
cmake -S $HOME/picotool-src -B $HOME/picotool-build -G Ninja \
    -DPICO_SDK_PATH=$PICO_SDK_PATH -DPICOTOOL_NO_LIBUSB=1 \
    -DPICOTOOL_FLAT_INSTALL=1 -DCMAKE_INSTALL_PREFIX=$HOME/picotool
cmake --build $HOME/picotool-build --target install
export picotool_DIR=$HOME/picotool/picotool
```

`PICOTOOL_NO_LIBUSB=1` leaves out USB support: this picotool only converts
files, it does not flash. Set `picotool_DIR` in every build terminal, like
step 2's settings.

## Step 6: the firmware

Generate the configuration header:

```bash
cd $HOME/priflop
mkdir -p obj-pico2w-config
scripts/configparser.pl --genfiles \
    --makeinc obj-pico2w-config/make.inc \
    --header  obj-pico2w-config/autoconf.h \
    configs/config-pico2w
```

Configure and build:

```bash
cmake -S scripts/pico -B build -G Ninja \
    -DPICO_SDK_PATH=$PICO_SDK_PATH \
    -DPICO_BOARD=pico2_w \
    -DAUTOCONF_DIR=$PWD/obj-pico2w-config \
    -DCMAKE_C_COMPILER=$(command -v arm-none-eabi-gcc) \
    -DCMAKE_CXX_COMPILER=$(command -v arm-none-eabi-g++)
cmake --build build
```

The result is `build/priflop.uf2`, about 1 MB. [Getting started](SOFTWARE.md)
says how to flash it. The compiler paths must be full paths, which is what
`$(command -v ...)` gives.

For a board with more flash, add `-DPRIFLOP_FLASH_MB=16` (or 8) to the
first command. The file storage then grows with the chip: 3839 blocks of
4 KB on 16 MB, against 767 on the Pico 2 W's 4 MB. This build has not been
tried on a real 16 MB board yet. Flashing a board with a different size
than before erases its files, so copy them off first.

## Step 7: the Z80 assembler (for the Primo files)

The Primo's boot file and PFM are assembled with `z80asm` from
[z88dk](https://z88dk.org/). Ubuntu has no package for it; build just the
assembler, at the version this project uses:

```bash
git clone --filter=blob:none --no-checkout https://github.com/z88dk/z88dk.git $HOME/z88dk
cd $HOME/z88dk
git checkout 0a73a0209e
git submodule update --init --depth 1 ext/uthash ext/optparse ext/regex
make src/config.h
make -C src/z80asm -j$(nproc)
sudo install -m 755 src/z80asm/z88dk-z80asm /usr/local/bin/
```

The `Library ... not found` lines it may print later are harmless.
**Do not set a variable called `Z80ASM`**: z80asm reads it as extra
arguments.

## Step 8: the Primo files

```bash
cd $HOME/priflop
python3 tools/primo/dos/build_dos.py $HOME/boot.prg
python3 tools/primo/pfm/build_pfm.py $HOME/PFM.PRG
```

`boot.prg` is our DOS, the boot file. On the drive it must be named with the
single byte 0x85, which the release's `deploy` tool does for you; see
[The Primo](PRIMO.md).

## Step 9: the tests

None of these needs a drive:

```bash
cd $HOME/priflop
gcc -I src/pico -I src -o nctest tests/netconfig_test.c \
    src/pico/netconfig.c src/sha256.c -DNETCONFIG_HOST_TEST && ./nctest
gcc -I src/pico -o healthtest tests/health_test.c src/pico/health.c && ./healthtest
python3 tests/webui_test.py
python3 tests/iec_rx_test.py
python3 -m unittest discover -s tools/primo -p 'test_*.py'
```

Each one ends with `passed` or `OK`. Some tests report `skipped`: they need
files that are not part of this project, such as the Primo's ROM or the
program library.

The Primo simulations in `tests/primo_*_sim.py` run the Z80 against the
Primo's own ROM and CDOS, which are not included; they skip themselves when
those files are missing.
