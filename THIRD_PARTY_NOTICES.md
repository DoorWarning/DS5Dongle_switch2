# Third-party notices

This firmware combines code under several licenses. Each component keeps its own license; the repository-level [LICENSE](./LICENSE) (MIT) covers everything else.

## In this repository

| Component | Files | License |
|---|---|---|
| DS5Dongle (awalol and contributors) | most of `src/`, `tools/`, `cmake/` | MIT, see [LICENSE](./LICENSE) |
| Switch Pro mode / HD rumble (Demogorgon314, `ds5-to-switchpro` branch of a DS5Dongle fork) | `src/switch_pro.*`, `src/switch_hd_rumble.*`, `src/switch_hd_haptics.*`, `src/switch_pro_descriptors.inc`, `tests/` | MIT (same as the repository) |
| Audio pipeline (awalol) | `src/audio.cpp` | **MPL-2.0**: modified copies of this file must stay under MPL-2.0 with source available. See <https://mozilla.org/MPL/2.0/> |
| USB descriptors (HiFiPhile) | `src/usb_descriptors.cpp` | MIT, notice in the file header |
| TinyUSB config template (Ha Thach) | `src/tusb_config.h` | MIT, notice in the file header |
| Raspberry Pi board header | `boards/headers/waveshare_rp2350b_plus_w.h` | BSD-3-Clause, notice in the file header |

## Git submodules (fetched separately)

| Component | Path | License |
|---|---|---|
| Opus (Xiph.Org and others) | `lib/opus` | BSD-3-Clause, see `lib/opus/COPYING` |
| WDL resampler (Cockos) | `lib/WDL` | zlib-style WDL license, see the file headers |

## Pulled in from the Pico SDK at build time (not stored here)

| Component | License |
|---|---|
| Raspberry Pi Pico SDK | BSD-3-Clause |
| TinyUSB | MIT |
| CYW43 driver (George Robotics) | Raspberry Pi licence: may only be used and redistributed with a microcontroller made by Raspberry Pi (see `pico-sdk/lib/cyw43-driver/LICENSE.RP`) |
| BTstack (BlueKitchen) | Raspberry Pi's BTstack licence: use and distribution only with Pico W, Pico WH, Pico 2 W, Pico 2 WH, RM2, and products built on them (see `pico-sdk/src/rp2_common/pico_btstack/LICENSE.RP`). BTstack's own licence otherwise allows non-commercial use only |

Prebuilt `.uf2` files contain the components above. They are meant only for the Raspberry Pi Pico 2 W and for boards built on the RM2 module, such as the Waveshare RP2350B-Plus-W. Using them on other hardware falls outside the BTstack and CYW43 licences. If you redistribute binaries, include this notice and the licences it refers to.

## Trademarks

Nintendo Switch, Joy-Con, Sony, PlayStation and DualSense are trademarks of their respective owners. This project is not affiliated with or endorsed by them. USB vendor/product IDs and device names are used only so the devices interoperate.
