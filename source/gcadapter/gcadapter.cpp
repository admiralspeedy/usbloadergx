// SPDX-License-Identifier: GPL-3.0-or-later
// GameCube controller adapter for Wii U (WUP-028) in Wii games: see
// gcadapter.hpp. Ported from RiftWii's wii/hooks/padhook.cpp and the
// adapter checks in its wii/launch/boot.cpp (settle_gc_adapter), cut down
// to the adapter alone: no other USB pads, no HOME Menu or way back by the
// controller, no calibration, no veneers for code builds.
// RiftWii: https://github.com/KakarottoCake/Riftwii (GPL-3.0-or-later),
// itself building on wup-028-bslug by Alex Chadwick (MIT).
#include "gcadapter.hpp"

#include <gccore.h>
#include <ogc/ipc.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gecko.h"
#include "memory/mem2.h"
#include "patches/gamepatches.h"
#include "settings/SettingsEnums.h"
#include "sys.h"
#include "symsearch.hpp"
#include "runtime/pad_hook.h"
#include "runtime/rtgcad.h"

// The blob (runtime/Makefile), linked in by the main Makefile.
extern "C" const u8 gcpad_bin[];
extern "C" const u8 gcpad_bin_end[];

namespace GCAdapter
{
namespace
{
using riftwii::CodeRange;

constexpr u32 kMem1Start = 0x80000000;
constexpr u32 kMem1End = 0x81800000;
constexpr u32 kMem2Start = 0x90000000;
constexpr u32 kMem2End = 0x94000000;
// Low memory the game reads its arenas from.
constexpr u32 kArenaHi = 0x80000034;      // BootInfo arenaHi (0: not set)
constexpr u32 kFst = 0x80000038;          // the FST, at the top of MEM1
constexpr u32 kMem1ArenaEnd = 0x80003110; // MEM1 arena end
constexpr u32 kMem2ArenaStart = 0x80003124;
constexpr u32 kMem2ArenaEnd = 0x80003128;
constexpr u32 kLowMemEnd = 0x80003F00;    // the end of the OS globals
// The bus clock (Disc_SetLowMem writes the same to 0x800000F8); the time base runs at a quarter of it.
constexpr u32 kBusClock = 0x0E7BE2C0;
constexpr unsigned kScratchRegister = 12; // pad_entry.S's jump back
constexpr u32 kNop = 0x60000000;
// A USB HID module that does not answer turns the adapter off instead of hanging the launch.
constexpr unsigned kHidTimeoutMs = 1500;
// v5 serves 16 handles, picked by the open mode: libogc's USB takes 0. v4 ignores it.
constexpr int kHidHandles[2] = {0, 1};
// Looking for the adapter in the device list: once more after this long when it is not there
// (just after an IOS reload, the devices are still being found).
constexpr unsigned kListRetryMs = 1000;
constexpr u32 kVersionBytes = 32;
constexpr u32 kListBytes = 0x180;
constexpr u32 kListEntryBytes = 12; // v5 device list: device id, VID:PID, then 4 bytes

char g_hid_path[] ATTRIBUTE_ALIGN(32) = "/dev/usb/hid";
char g_ven_path[] ATTRIBUTE_ALIGN(32) = "/dev/usb/ven";
const char *g_path_used = g_hid_path;

// Our own requests' buffers, in MEM2 as libogc's (a Wii U's IOS answered -4
// to every request with a buffer in MEM1).
u32 *g_version_out = nullptr;
u8 *g_list = nullptr;
volatile bool g_async_done = false;
volatile s32 g_async_result = 0;
bool g_hid_stuck = false; // a request timed out: /dev/usb/hid is left alone from then on
bool g_hid_v4 = false;    // v4 only: v5's GetVersion (v4's GetDeviceChange) is not asked

struct Launch
{
	bool open = false;   // OpenUSB succeeded: fd is ours, to hand to the game
	bool found = false;  // FindFunctions succeeded
	s32 fd = -1;
	u32 version = 0;     // 4 or 5
	s32 device = -1;     // the adapter's v5 device id, -1: none seen
	u32 read = 0;        // PADRead
	u32 motor = 0;       // PADControlMotor, 0: no rumble
	u32 idleReset = 0;   // the screen-dimming reset PADRead calls, 0: not found
	u32 siProbe = 0;     // SIProbe, 0: not found
	u32 ioctlAsync = 0;  // the game's IOS_IoctlAsync
	u32 ioctlvAsync = 0; // the game's IOS_IoctlvAsync (needed on v5)
	std::vector<std::pair<u32, u32>> chunks; // the game as loaded: each chunk's address and end
};
Launch g;

u32 read32(u32 address) { return *reinterpret_cast<volatile u32 *>(address); }
void write32(u32 address, u32 value) { *reinterpret_cast<volatile u32 *>(address) = value; }
u32 align_up(u32 v) { return (v + 31) & ~31u; }
bool in_mem1(u32 a) { return a >= kMem1Start && a < kMem1End; }

s32 on_async(s32 result, void *)
{
	g_async_result = result;
	g_async_done = true;
	return 0;
}

// Waits for the reply of a request just submitted (`submitted` is what the
// submit returned). False on a timeout: the request stays with IOS.
bool await_async(s32 submitted, s32 &result)
{
	if (submitted < 0)
	{
		result = submitted;
		return true;
	}
	for (unsigned i = 0; i < kHidTimeoutMs && !g_async_done; ++i)
		usleep(1000);
	if (!g_async_done)
	{
		g_hid_stuck = true;
		return false;
	}
	result = g_async_result;
	return true;
}

bool usb_buffers()
{
	if (g_version_out)
		return true;
	u8 *p = static_cast<u8 *>(MEM2_alloc(kVersionBytes + kListBytes));
	if (!p || (reinterpret_cast<u32>(p) & 31) != 0)
	{
		if (p)
			MEM2_free(p);
		return false;
	}
	g_version_out = reinterpret_cast<u32 *>(p);
	g_list = p + kVersionBytes;
	return true;
}

s32 ask_v5_version(s32 fd, s32 &result)
{
	std::memset(g_version_out, 0, kVersionBytes);
	DCFlushRange(g_version_out, kVersionBytes);
	g_async_done = false;
	if (!await_async(IOS_IoctlAsync(fd, GCAD_V5_GET_VERSION, nullptr, 0, g_version_out, kVersionBytes, on_async, nullptr), result))
		return false;
	DCInvalidateRange(g_version_out, kVersionBytes);
	return true;
}

// Opens /dev/usb/hid and tells v4 from v5: v4's GetVersion answers 0x40001
// itself, v5's writes 0x50001 into its output. v5 is asked first, as libogc
// does (v4's request number 6 is v5's AttachFinish), unless g_hid_v4. When
// /dev/usb/hid answers neither, /dev/usb/ven (v5) as libogc does. Every
// call is asynchronous with a timeout.
bool open_usb_hid(s32 &fd, u32 &version, std::string &why)
{
	fd = -1;
	g_path_used = g_hid_path;
	if (!usb_buffers())
	{
		why = "no MEM2 for the USB requests";
		return false;
	}
	std::string tried;
	for (const int handle : kHidHandles)
	{
		s32 ret = 0;
		g_async_done = false;
		if (!await_async(IOS_OpenAsync(g_hid_path, handle, on_async, nullptr), ret))
		{
			why = "/dev/usb/hid did not answer when opened";
			return false;
		}
		if (ret < 0)
		{
			tried += (tried.empty() ? "" : "; ") + std::string("handle ") + std::to_string(handle) + ": open " + std::to_string(ret);
			continue;
		}
		fd = ret;
		s32 v5 = 0;
		if (!g_hid_v4)
		{
			if (!ask_v5_version(fd, v5))
			{
				why = "/dev/usb/hid did not answer its v5 GetVersion";
				fd = -1; // left open: closing it could wait too
				return false;
			}
			if (v5 == 0 && g_version_out[0] == GCAD_V5_VERSION)
			{
				version = 5;
				return true;
			}
		}
		// v4: request 0 is GetDeviceChange, which answers at once the first
		// time; the handle is opened again for v4's question.
		IOS_Close(fd);
		fd = -1;
		s32 v4 = 0;
		g_async_done = false;
		if (!await_async(IOS_OpenAsync(g_hid_path, handle, on_async, nullptr), ret))
		{
			why = "/dev/usb/hid did not answer when opened again for v4";
			return false;
		}
		if (ret >= 0)
		{
			fd = ret;
			g_async_done = false;
			if (!await_async(IOS_IoctlAsync(fd, GCAD_V4_GET_VERSION, nullptr, 0, nullptr, 0, on_async, nullptr), v4))
			{
				why = "/dev/usb/hid did not answer its v4 GetVersion";
				fd = -1;
				return false;
			}
			if (v4 == static_cast<s32>(GCAD_V4_VERSION))
			{
				version = 4;
				return true;
			}
			IOS_Close(fd);
			fd = -1;
		}
		else
		{
			v4 = ret;
		}
		char buf[96];
		snprintf(buf, sizeof(buf), "handle %d: v5 GetVersion %d (%08x), v4 GetVersion %d", handle, (int)v5,
				 (unsigned)g_version_out[0], (int)v4);
		tried += (tried.empty() ? "" : "; ") + std::string(buf);
	}
	if (!g_hid_v4)
	{
		s32 ret = 0;
		g_async_done = false;
		if (!await_async(IOS_OpenAsync(g_ven_path, 0, on_async, nullptr), ret))
		{
			why = "/dev/usb/hid answers neither as v4 nor as v5 (" + tried + "); /dev/usb/ven did not answer";
			return false;
		}
		if (ret >= 0)
		{
			fd = ret;
			s32 v5 = 0;
			if (!ask_v5_version(fd, v5))
			{
				why = "/dev/usb/ven did not answer its v5 GetVersion";
				fd = -1;
				return false;
			}
			if (v5 == 0 && g_version_out[0] == GCAD_V5_VERSION)
			{
				version = 5;
				g_path_used = g_ven_path;
				return true;
			}
			IOS_Close(fd);
			fd = -1;
		}
	}
	why = "/dev/usb/hid answers neither as v4 nor as v5 (" + tried + ")";
	return false;
}

// v5's first device-change request on a handle answers at once with the
// devices there (its result is their number). The adapter's device id, or
// -1; false when the list could not be had.
bool own_list(s32 fd, s32 &device, std::string &devices)
{
	device = -1;
	devices.clear();
	std::memset(g_list, 0, kListBytes);
	DCFlushRange(g_list, kListBytes);
	s32 count = 0;
	g_async_done = false;
	if (!await_async(IOS_IoctlAsync(fd, GCAD_V5_GET_DEVICE_CHANGE, nullptr, 0, g_list, kListBytes, on_async, nullptr), count))
		return false;
	DCInvalidateRange(g_list, kListBytes);
	if (count < 0)
		return false;
	for (s32 i = 0; i < count && static_cast<u32>(i + 1) * kListEntryBytes <= kListBytes; ++i)
	{
		const u32 id = *reinterpret_cast<const u32 *>(g_list + i * kListEntryBytes);
		const u32 vid_pid = *reinterpret_cast<const u32 *>(g_list + i * kListEntryBytes + 4);
		char one[16];
		snprintf(one, sizeof(one), "%s%04x:%04x", i ? ", " : "", (unsigned)(vid_pid >> 16), (unsigned)(vid_pid & 0xFFFF));
		devices += one;
		if (vid_pid == GCAD_VID_PID && device < 0)
			device = static_cast<s32>(id);
	}
	if (devices.empty())
		devices = "no devices";
	// As libogc does after each device change.
	s32 finish = 0;
	g_async_done = false;
	await_async(IOS_IoctlAsync(fd, GCAD_V5_ATTACH_FINISH, nullptr, 0, nullptr, 0, on_async, nullptr), finish);
	return true;
}

// Where RiftWii found on hardware that the adapter breaks the launch, it
// stays off unless forced (RiftWii's settle_gc_adapter):
//  - on a Wii U only IOS 58 and a d2x cIOS on base 58 reach the adapter;
//  - a game read from a USB drive through the cIOS failed its disc reads
//    with the adapter in use, except on a Wii U's d2x on base 58;
//  - a d2x on base 56 or older (v4 USB) failed its disc reads after the
//    launch's USB check, with the game on the SD card too.
const char *blocked(const LaunchInfo &l)
{
	if (isWiiU() && l.gameIOS != 58 && l.d2xBase != 58)
		return "on a Wii U it needs IOS 58 or a d2x cIOS on base 58";
	if (l.readsFromUSB && !(isWiiU() && l.d2xBase == 58))
		return "the game is read from a USB drive, which the adapter's USB use breaks on this console and cIOS";
	if ((l.readsFromUSB || l.readsFromSD) && l.d2xBase != 0 && l.d2xBase < 57)
		return "the game is read through d2x on base 56 or older, whose USB the adapter breaks";
	return nullptr;
}

const rt_pad_header &header() { return *reinterpret_cast<const rt_pad_header *>(gcpad_bin); }
u32 blob_size() { return static_cast<u32>(gcpad_bin_end - gcpad_bin); }

// The blob is RiftWii's without the HOME Menu: its header ends before the
// HOME Menu's hooks, and its context is RT_PAD_CONTEXT_BASE_BYTES long.
bool blob_whole()
{
	const rt_pad_header &h = header();
	const u32 size = blob_size();
	return size >= offsetof(rt_pad_header, hook_kpad) && h.magic == RT_PAD_MAGIC && h.version == RT_PAD_VERSION &&
		   h.size <= size && h.context_offset + RT_PAD_CONTEXT_BASE_BYTES <= h.size;
}

void store_words(u32 address, const u32 *words, std::size_t count)
{
	volatile u32 *p = reinterpret_cast<volatile u32 *>(address);
	for (std::size_t i = 0; i < count; ++i)
		p[i] = words[i];
}

void sync_code(u32 address, u32 bytes)
{
	DCFlushRange(reinterpret_cast<void *>(address), bytes);
	ICInvalidateRange(reinterpret_cast<void *>(address), bytes);
}

// From RiftWii's src/patching/hook.cpp.
void encode_absolute_jump(unsigned reg, u32 target, u32 out[4])
{
	const u32 r = reg & 31u;
	out[0] = 0x3C000000u | (r << 21) | (target >> 16);              // lis r, target@h
	out[1] = 0x60000000u | (r << 21) | (r << 16) | (target & 0xFFFF); // ori r, r, target@l
	out[2] = 0x7C0903A6u | (r << 21);                                // mtctr r
	out[3] = 0x4E800420u;                                            // bctr
}

bool encode_branch(u32 from, u32 to, u32 &out)
{
	if ((from & 3) != 0 || (to & 3) != 0)
		return false;
	const s64 delta = static_cast<s64>(to) - static_cast<s64>(from);
	if (delta < -0x2000000 || delta > 0x1FFFFFC)
		return false;
	out = 0x48000000u | (static_cast<u32>(delta) & 0x03FFFFFCu); // b to
	return true;
}

// Whether a function's first instruction can run from the blob's replay
// slot instead (it is moved there and replaced with the branch to the hook).
bool displaceable(u32 instruction, unsigned scratch_reg, const char *&why)
{
	const u32 opcode = instruction >> 26;
	if (opcode == 16 || opcode == 18)
	{
		why = "it is a branch";
		return false;
	}
	if (opcode == 17)
	{
		why = "it is a system call";
		return false;
	}
	if (opcode == 19)
	{
		const u32 xo = (instruction >> 1) & 0x3FF;
		if (xo == 16 || xo == 528 || xo == 50)
		{ // bclr, bcctr, rfi
			why = "it is a branch or return";
			return false;
		}
	}
	if (opcode == 0)
	{
		why = "it is not a valid instruction";
		return false;
	}
	if (opcode == 31)
	{
		// The trampoline restores LR before the replay, but CTR is clobbered
		// by the jump that follows it and XER by the C handler.
		const u32 xo = (instruction >> 1) & 0x3FF;
		const u32 spr = ((instruction >> 16) & 31) | (((instruction >> 11) & 31) << 5);
		if ((xo == 339 || xo == 467) && spr != 8)
		{
			why = "it reads or writes a special register other than LR";
			return false;
		}
	}
	if (instruction == kNop)
		return true;
	const u32 rd = (instruction >> 21) & 31;
	const u32 ra = (instruction >> 16) & 31;
	const u32 rb = (instruction >> 11) & 31;
	const bool rb_is_register = opcode == 31 || opcode == 23 || opcode == 4;
	if (rd == scratch_reg || ra == scratch_reg || (rb_is_register && rb == scratch_reg))
	{
		why = "it uses the scratch register";
		return false;
	}
	return true;
}

// The game as loaded: every chunk the apploader (or the DOL loader)
// registered, in MEM1 or MEM2. The searches only take matches they are
// sure of, so the data sections among them do no harm.
std::vector<CodeRange> loaded_ranges()
{
	std::vector<CodeRange> ranges;
	const int count = GetDOLChunkCount();
	for (int i = 0; i < count; ++i)
	{
		u8 *dst = nullptr;
		int len = 0;
		GetDOLChunk(i, &dst, &len);
		const u32 a = reinterpret_cast<u32>(dst);
		if (len <= 0 || (a & 3) != 0)
			continue;
		const bool mem1 = a >= kMem1Start && a + static_cast<u32>(len) <= kMem1End;
		const bool mem2 = a >= kMem2Start && a + static_cast<u32>(len) <= kMem2End;
		if (mem1 || mem2)
			ranges.push_back({a, dst, static_cast<std::size_t>(len)});
	}
	return ranges;
}

void off(const char *why)
{
	gprintf("GameCube adapter: off: %s\n", why);
}

} // namespace

bool OpenUSB(int mode, const LaunchInfo &launch)
{
	Cancel();
	g = Launch{};
	if (mode != GC_ADAPTER_ON && mode != GC_ADAPTER_FORCED)
		return false;
	if (!blob_whole())
	{
		off("the embedded blob is damaged");
		return false;
	}
	const char *risk = blocked(launch);
	if (risk && mode != GC_ADAPTER_FORCED)
	{
		off(risk);
		return false;
	}
	if (risk)
		gprintf("GameCube adapter: on anyway (the setting is Forced), although %s\n", risk);
	if (g_hid_stuck)
	{
		off("/dev/usb/hid did not answer earlier");
		return false;
	}
	// A d2x on base 56 or older has v4's /dev/usb/hid (57's and 58's are v5).
	g_hid_v4 = launch.d2xBase != 0 && launch.d2xBase < 57;

	std::string why, devices;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (!open_usb_hid(g.fd, g.version, why))
		{
			g.fd = -1;
			off(why.c_str());
			return false;
		}
		if (g.version == 4)
			break; // v4's device list is the driver's own first request
		if (!own_list(g.fd, g.device, devices))
		{
			if (!g_hid_stuck)
				IOS_Close(g.fd);
			g.fd = -1;
			off("/dev/usb/hid v5 gave no device list");
			return false;
		}
		if (g.device >= 0 || attempt == 1)
			break;
		// Just after an IOS reload: a fresh handle once more, a little later.
		IOS_Close(g.fd);
		g.fd = -1;
		usleep(kListRetryMs * 1000);
	}
	if (g.version == 5)
	{
		gprintf("GameCube adapter: %s v5 fd %d lists %s\n", g_path_used, (int)g.fd, devices.c_str());
		if (g.device < 0 && mode != GC_ADAPTER_FORCED)
		{
			IOS_Close(g.fd);
			g.fd = -1;
			off("no adapter (057e:0337) is plugged in");
			return false;
		}
	}
	else
	{
		gprintf("GameCube adapter: /dev/usb/hid v4 fd %d\n", (int)g.fd);
	}
	g.open = true;
	return true;
}

bool FindFunctions(void)
{
	if (!g.open)
		return false;
	g.found = false;
	const std::vector<CodeRange> text = loaded_ranges();
	if (text.empty())
	{
		off("the game's code was not registered");
		Cancel();
		return false;
	}
	std::string error;
	riftwii::PadSymbols pad;
	if (!riftwii::find_pad_symbols(text, pad, error))
	{
		off(("the PAD search is unsure: " + error).c_str());
		Cancel();
		return false;
	}
	if (pad.read == 0)
	{
		off("this game has no GameCube controller support (no PADRead)");
		Cancel();
		return false;
	}
	riftwii::IpcSymbols ipc;
	if (!riftwii::find_ipc_symbols(text, ipc, error))
	{
		off(("the game's IOS_IoctlAsync was not found: " + error).c_str());
		Cancel();
		return false;
	}
	u32 ioctlv = ipc.ioctlv_async;
	if (ioctlv == 0)
	{
		riftwii::IpcApi api;
		if (riftwii::find_ipc_api(text, ipc, api, error))
			ioctlv = api.async[riftwii::kIpcIoctlvCmd];
	}
	if (g.version == 5 && ioctlv == 0)
	{
		off("/dev/usb/hid v5 needs the game's IOS_IoctlvAsync, which was not found");
		Cancel();
		return false;
	}
	g.read = pad.read;
	g.motor = pad.control_motor;
	g.idleReset = pad.idle_reset;
	g.siProbe = riftwii::find_gc_si_probe(text, pad.read);
	g.ioctlAsync = ipc.ioctl_async;
	g.ioctlvAsync = ioctlv;
	g.chunks.clear();
	for (const CodeRange &r : text)
		g.chunks.push_back({r.address, r.address + static_cast<u32>(r.size)});
	gprintf("GameCube adapter: PADRead at 0x%08x, PADControlMotor at 0x%08x, SIProbe at 0x%08x, "
			"screen-dimming reset at 0x%08x, IOS_IoctlAsync at 0x%08x, IOS_IoctlvAsync at 0x%08x\n",
			g.read, g.motor, g.siProbe, g.idleReset, g.ioctlAsync, g.ioctlvAsync);
	g.found = true;
	return true;
}

bool Install(void)
{
	if (!g.open || !g.found)
	{
		Cancel();
		return false;
	}
	const rt_pad_header &h = header();
	const u32 code_bytes = align_up(h.size);
	const u32 state_bytes = align_up(GCAD_OWNER_BYTES(g.version));

	// 1. Memory: the blob just below the top of the game's MEM1 arena (below
	//    the FST, and below anything a patch already put there, such as the
	//    Wiimmfi payload), the driver's state at the bottom of its MEM2 arena.
	//    The driver zeroes its state at the game's first PADRead.
	u32 arena1_hi = read32(kMem1ArenaEnd);
	const u32 lows[2] = {read32(kArenaHi), read32(kFst)};
	for (const u32 low : lows)
		if (in_mem1(low) && low > kLowMemEnd && low < arena1_hi)
			arena1_hi = low;
	// Nothing the apploader loaded below that top may be covered: the DOL's
	// sections (the FST and BI2 are at or above it).
	u32 floor = kLowMemEnd;
	for (const auto &chunk : g.chunks)
		if (in_mem1(chunk.first) && chunk.second <= arena1_hi && chunk.second > floor)
			floor = chunk.second;
	const u32 code_base = (arena1_hi - code_bytes) & ~31u;
	const u32 state_base = align_up(read32(kMem2ArenaStart));
	const u32 arena2_end = read32(kMem2ArenaEnd);
	if (!in_mem1(arena1_hi - 1) || arena1_hi < code_bytes || code_base < floor || state_base < kMem2Start ||
		arena2_end > kMem2End || state_base + state_bytes > arena2_end)
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "no room (MEM1 arena top 0x%08x, image end 0x%08x; MEM2 arena 0x%08x-0x%08x)",
				 arena1_hi, floor, read32(kMem2ArenaStart), arena2_end);
		off(buf);
		Cancel();
		return false;
	}

	// 2. PADRead must be hookable (a patch or the code handler may have
	//    hooked it since the search); the other two are optional.
	struct Site
	{
		u32 function, hook, replay, resume;
		const char *name;
	} sites[3] = {{g.read, h.hook_read, h.replay_read, h.continue_read, "PADRead"},
				  {g.motor, h.hook_motor, h.replay_motor, h.continue_motor, "PADControlMotor"},
				  {g.siProbe, h.hook_si_probe, h.replay_si_probe, h.continue_si_probe, "SIProbe"}};
	for (Site &s : sites)
	{
		if (s.function == 0)
			continue;
		const char *why = "";
		u32 branch = 0;
		bool ok = displaceable(read32(s.function), kScratchRegister, why);
		if (ok && !encode_branch(s.function, code_base + s.hook, branch))
		{
			why = "it is out of a branch's reach";
			ok = false;
		}
		if (!ok)
		{
			if (&s == &sites[0])
			{
				char buf[128];
				snprintf(buf, sizeof(buf), "PADRead (0x%08x) cannot be hooked: %s", read32(s.function), why);
				off(buf);
				Cancel();
				return false;
			}
			gprintf("GameCube adapter: %s cannot be hooked (%s)%s\n", s.name, why,
					&s == &sites[1] ? "; no rumble" : "; a game that asks it which ports have a controller sees none there");
			s.function = 0;
		}
	}

	// 3. The blob and its context.
	std::memset(reinterpret_cast<void *>(code_base), 0, code_bytes);
	std::memcpy(reinterpret_cast<void *>(code_base), gcpad_bin, h.size);
	rt_pad_context *ctx = reinterpret_cast<rt_pad_context *>(code_base + h.context_offset);
	if (ctx->magic != RT_PAD_CONTEXT_MAGIC)
	{
		off("the blob's context is not where its header says");
		Cancel();
		return false;
	}
	ctx->state = state_base;
	ctx->cal = 0;
	ctx->ioctl_async = g.ioctlAsync;
	ctx->ioctlv_async = g.ioctlvAsync;
	ctx->complete_entry = code_base + h.complete;
	ctx->fd = g.fd;
	ctx->version = g.version;
	ctx->ticks_per_ms = kBusClock / 4000u;
	ctx->inited = 0;
	ctx->flags = RT_PAD_FLAG_ADAPTER_ONLY;
	ctx->known_dev = g.version == 5 ? g.device : -1;
	ctx->ven_fd = -1;
	ctx->idle_reset = g.idleReset;
	ctx->return_to_menu = 0; // no way back by the controller
	ctx->restart = 0;        // and the game's own Reset is left alone
	DCFlushRange(ctx, RT_PAD_CONTEXT_BASE_BYTES);

	// 4. The replay slots (each function's first word) and the jumps back,
	//    then the branches into the blob.
	for (const Site &s : sites)
	{
		if (s.function == 0)
			continue;
		const u32 replay[4] = {read32(s.function), kNop, kNop, kNop};
		store_words(code_base + s.replay, replay, 4);
		u32 resume[4];
		encode_absolute_jump(kScratchRegister, s.function + 4, resume);
		store_words(code_base + s.resume, resume, 4);
	}
	sync_code(code_base, code_bytes);
	for (const Site &s : sites)
	{
		if (s.function == 0)
			continue;
		u32 branch = 0;
		encode_branch(s.function, code_base + s.hook, branch);
		store_words(s.function, &branch, 1);
		sync_code(s.function & ~31u, 32);
	}

	// 5. The game's arenas, around the blob and the state.
	write32(kMem1ArenaEnd, code_base);
	if (read32(kArenaHi) != 0)
		write32(kArenaHi, code_base);
	write32(kMem2ArenaStart, state_base + state_bytes);
	DCFlushRange(reinterpret_cast<void *>(0x80000020), 32);
	DCFlushRange(reinterpret_cast<void *>(0x80003100), 64);

	gprintf("GameCube adapter: blob %u bytes at 0x%08x, state %u bytes at 0x%08x; hooked PADRead%s%s\n", h.size,
			code_base, state_bytes, state_base, sites[1].function ? ", PADControlMotor" : "",
			sites[2].function ? ", SIProbe" : "");
	g.open = false; // the handle is the game's now
	g.found = false;
	g.fd = -1;
	return true;
}

void Cancel(void)
{
	if (g.open && g.fd >= 0 && !g_hid_stuck)
		IOS_Close(g.fd);
	g.open = false;
	g.found = false;
	g.fd = -1;
}

} // namespace GCAdapter
