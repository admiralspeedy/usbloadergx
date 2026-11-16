# GameCube controller adapter for Wii U in Wii games

With **GameCube Adapter (Wii U)** on (Settings > Loader Settings, or per game
in the game's Game Load settings), a Wii game that supports GameCube
controllers reads the controllers plugged into a GameCube Controller Adapter
for Wii U (WUP-028, or a third-party adapter in its Wii U mode) as if they
were in the console's own GameCube ports, with rumble. A controller in the
console's own port wins over the adapter's in the same port.

Unlike wup-028-bslug, which needs a symbol file for each game, the game's
`PADRead`, `PADControlMotor`, `SIProbe` and `IOS_IoctlAsync`/`IOS_IoctlvAsync`
are found by scanning its code as loaded, so no per-game files are needed.

## Settings

| Value | What it does |
| --- | --- |
| OFF | Default. The game is not touched. |
| ON | Only where the adapter is known to work (below), and only when an adapter is plugged in at launch (USB HID v5). |
| Forced | Tries anyway, even where it is known to break the launch, and installs without an adapter plugged in. |

Where RiftWii found on hardware that the adapter breaks the launch, **ON**
leaves it off and says why in the gecko log:

- on a Wii U (vWii), only IOS 58 and a d2x cIOS on base 58 reach the adapter;
- a game read from a USB drive through the cIOS failed its disc reads with the
  adapter in use, except with a Wii U's d2x on base 58;
- a d2x cIOS on base 56 or older (USB HID v4) failed its disc reads after the
  launch's USB check, with the game on the SD card too.

Games that check their own code (`exclude_game`) are always left alone.

## How it works

1. `GCAdapter::OpenUSB` (GameBooter, after the apploader or channel loader,
   before `ShutDownDevices`): checks the above, opens `/dev/usb/hid` (v5, then
   v4, then `/dev/usb/ven`) and reads the adapter's device id from the USB
   device list. The handle is handed to the game.
2. `GCAdapter::FindFunctions` (before `gamepatches()`, while the DOL's chunks
   are still registered): runs the scanner over the game as loaded.
3. `GCAdapter::Install` (after every other patch, the code handler and the
   Wiimmfi payload, right before the jump): copies the blob just below the top
   of the game's MEM1 arena and reserves the driver's state at the bottom of
   its MEM2 arena (lowering `0x80003110`/`0x80000034` and raising
   `0x80003124`), then replaces the first instruction of each function with a
   branch into the blob. The moved instruction runs from the blob's replay
   slot.

If anything stands in the way (no PADRead, an ambiguous match, no room, a
patch or code that already hooked PADRead's first instruction) the adapter is
off for that launch and the game boots as usual.

## Where the code comes from

From RiftWii (https://github.com/KakarottoCake/Riftwii, GPL-3.0-or-later),
commit `47db2087503ba82db94531c8dc2dbc4c322d8e99`:

| Here | RiftWii | Changes |
| --- | --- | --- |
| `runtime/*.c`, `*.h`, `*.S`, `pad.ld` | `runtime/pad/`, `runtime/rtgcad.*`, `rtleave.*`, `rtrestart.*`, `rthome.h`, `rthid.h`, `rtpadcal.h`, `rtsiprobe.h` | None. Built as RiftWii's `padan` blob (`-DGCAD_ADAPTER_ONLY -DRT_PAD_NO_HOME`). |
| `runtime/Makefile` | `Makefile.runtime` | The `padan` rules only. |
| `symsearch.cpp`, `symsearch.hpp` | `src/patching/symsearch.cpp`, `include/riftwii/symsearch.hpp` | The include path only. |
| `gcadapter.cpp` | `wii/hooks/padhook.cpp`, `src/patching/hook.cpp`, `settle_gc_adapter` in `wii/launch/boot.cpp` | Cut down to the adapter alone. |

RiftWii's adapter driver follows wup-028-bslug by Alex Chadwick
(https://github.com/Chadderz121/wup-028-bslug, MIT). See the file headers.

Left out of this port (RiftWii has them): other USB controllers (HID pads,
Xbox 360, Switch Pro, DualShock 3), the game's HOME Menu and the way back to
the loader by holding L, R, Z and Start, controller calibration, and veneers
for code builds that use all of MEM1.
