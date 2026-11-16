// SPDX-License-Identifier: GPL-3.0-or-later
// GameCube controller adapter for Wii U (WUP-028) in Wii games.
//
// Ported from RiftWii (https://github.com/KakarottoCake/Riftwii,
// wii/hooks/padhook.cpp), which builds on wup-028-bslug by Alex Chadwick.
// Unlike wup-028-bslug, which needs a symbol file for each game, the
// game's PADRead, PADControlMotor, SIProbe and IOS_IoctlAsync /
// IOS_IoctlvAsync are found by scanning its code (symsearch.cpp), so any
// game with GameCube controller support can use the adapter.
//
// How it works: a small position-independent blob (runtime/, RiftWii's
// WUP-028 driver) is copied just below the top of the game's MEM1 arena,
// and its state is reserved at the bottom of the game's MEM2 arena. The
// first instruction of PADRead (and of PADControlMotor and SIProbe, when
// found) is replaced with a branch into the blob. After the game's own
// PADRead runs, the blob fills every port that has no controller plugged
// into the console with the adapter's controller in the same port, and
// passes rumble on. The driver talks to the adapter through /dev/usb/hid
// with the game's own IOS functions; the handle is opened here, with the
// game's IOS already loaded, and handed over to the game.
//
// Anything that stands in the way turns the adapter off for that launch
// (with a reason in the gecko log) and never stops the game from booting.
#pragma once

#include <gctypes.h>

namespace GCAdapter
{
	// What the launch knows about where the game is read from, for the
	// hardware checks (see Allowed in gcadapter.cpp).
	struct LaunchInfo
	{
		int gameIOS;         // the IOS the game runs on
		int d2xBase;         // its d2x base IOS, 0 when it is not a d2x cIOS
		bool readsFromUSB;   // the cIOS reads the game (or its emulated NAND) from a USB drive
		bool readsFromSD;    // the cIOS reads the game (or its emulated NAND) from the SD card
	};

	// Settings value (GameCFG/Settings gcAdapter): GC_ADAPTER_OFF,
	// GC_ADAPTER_ON (only where it is known to work, and only when an adapter
	// is seen), GC_ADAPTER_FORCED (try anyway). See SettingsEnums.h.

	// With the game's IOS loaded and before libogc's USB is shut down
	// (GameBooter::ShutDownDevices): checks the launch, opens /dev/usb/hid
	// and looks for the adapter. False when the adapter is off for this game.
	bool OpenUSB(int mode, const LaunchInfo &launch);

	// With the game's DOL loaded and before the patches (the DOL's chunks
	// are still registered, gamepatches.c): finds the PAD functions and the
	// game's IOS functions. False when the adapter is off for this game.
	bool FindFunctions(void);

	// After every other patch (game patches, code handler, Wiimmfi), right
	// before the jump to the game: places the blob, lowers the game's arenas
	// and hooks the functions. False when the adapter is off for this game.
	bool Install(void);

	// Gives the USB handle back when the adapter is not installed.
	void Cancel(void);
}
