// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC as a mod: a DLL that bbsc-launch.exe injects into shadPS4.exe before it starts.
//
// The DLL runs in the emulator's process, where the game's memory sits at its guest addresses
// (eboot at 0x800000000 in the shadp2p fork; the official shadPS4 nightly of 2026-10-09 loads it
// at 0x9B00000 - FindEbootBase looks for it), so it needs nothing from the emulator:
//   1. wait until the eboot is in memory and every byte patch site reads back as the bytes that
//      must be there (all of them before writing any - another game or version is left alone);
//   2. write the byte patches (core/bbsc_patch_tables.h, the same list as the emulator build);
//   3. install every hook with the emulator build's own code, core/bloodborne_re.cpp, compiled
//      into this DLL with BBSC_MOD; its emulator calls are answered here and in bbsc_hooks.cpp
//      (hook trampolines) and bbsc_relay.cpp (BBSC's packets, through the game server);
//   4. keep watching: a patch site the emulator wrote back is written again.
// Session suspensions and the lamp role toggles come from bloodborne_re.cpp as in the emulator
// build (SetPersistentSessionPatches / SetHostLampPatches below).
//
// Log: logs\bbsc-mod-<date>-<time>-<pid>.log next to the DLL (one per game window, so two
// windows on one PC do not overwrite each other); capture files in captures\ next to it.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#include <string>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/bbsc_cinematic.h"
#include "core/bbsc_patch_tables.h"
#include "core/bloodborne_re.h"

// One line in this run's BBSC timeline (bloodborne_re.cpp, "Timeline").
#define BBSC_TIMELINE(kind, ...) Core::Bloodborne::BbscTimelineEvent(kind, fmt::format(__VA_ARGS__))
#include "core/debugger.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/memory.h"

using namespace Core::Bbsc::PatchTables;

// Public version (2026-10-11): 0.MINOR.PATCH while testing - PATCH for bug fixes, MINOR for new
// features, 1.0.0 the finished release. Keep it equal to "version" in mod.json.
#define BBSC_VERSION "0.0.2"

namespace BbscMod {
void LogPlain(const char* text);
u64 EbootAddress();
void StartRelay();
void StartP2PChannel(); // bbsc_channel.cpp
bool GameServerIsPublic(std::string* host_out); // bbsc_relay.cpp
void InstallHttpWrappers();
void StartDiscordStatus();
} // namespace BbscMod
namespace Core::Bbsc {
void InstallPadWrapper();
} // namespace Core::Bbsc

namespace {

constexpr u64 ForkEbootBase = 0x800000000ULL; // where the shadp2p fork puts it
u64 EbootBase = 0; // found by FindEbootBase before anything else uses it
constexpr u64 EbootSize = 0x056D4000ULL; // CUSA03173 01.09 image size
constexpr DWORD WaitForImageMs = 180'000;
constexpr DWORD SettleMs = 20;            // let the emulator finish its own load-time patching
constexpr DWORD WatchMs = 500;

HMODULE self_module = nullptr;
std::filesystem::path mod_dir;
FILE* log_file = nullptr;
std::mutex log_mutex;

void OpenLog() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self_module, path, MAX_PATH);
    mod_dir = std::filesystem::path{path}.parent_path();
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[96]{};
    swprintf(name, 96, L"bbsc-mod-%04u%02u%02u-%02u%02u%02u-%lu.log", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    std::error_code ignored;
    std::filesystem::create_directories(mod_dir / L"logs", ignored);
    log_file = _wfopen((mod_dir / L"logs" / name).c_str(), L"w");
}

// ---- Byte patches --------------------------------------------------------------------------------

// VirtualQuery is a kernel call that shares the address-space lock with the emulator's constant
// page-protection changes (GPU memory tracking). bloodborne_re.cpp checks every memory read, so
// thousands of these per frame took the game to ~1 fps (2026-09-30, two testers). The emulator
// build answers the same checks from its own memory map; here each game thread remembers the last
// few regions it asked about for a moment.
struct CachedRegion {
    u64 start = 0;
    u64 end = 0;
    DWORD state = 0;
    DWORD protect = 0;
    u64 stamp_ms = 0;
};
constexpr u64 RegionCacheMs = 250;
constexpr size_t RegionCacheSize = 16;

bool QueryRegion(u64 address, CachedRegion& out) {
    thread_local std::array<CachedRegion, RegionCacheSize> cache{};
    thread_local size_t next = 0;
    const u64 now = GetTickCount64();
    for (const CachedRegion& region : cache) {
        if (address >= region.start && address < region.end &&
            now - region.stamp_ms <= RegionCacheMs) {
            out = region;
            return true;
        }
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    CachedRegion& slot = cache[next];
    next = (next + 1) % RegionCacheSize;
    slot.start = reinterpret_cast<u64>(info.BaseAddress);
    slot.end = slot.start + info.RegionSize;
    slot.state = info.State;
    slot.protect = info.Protect;
    slot.stamp_ms = now;
    out = slot;
    return true;
}

bool Readable(u64 address, size_t size) {
    u64 at = address;
    const u64 end = address + size;
    while (at < end) {
        CachedRegion region;
        if (!QueryRegion(at, region) || region.state != MEM_COMMIT ||
            (region.protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = region.end;
    }
    return true;
}

bool MatchesAt(u64 base, u64 offset, const u8* bytes, size_t size) {
    const u64 address = base + offset;
    return Readable(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

bool Matches(u64 offset, const u8* bytes, size_t size) {
    const u64 address = EbootBase + offset;
    return Readable(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

void Write(u64 offset, const u8* bytes, size_t size) {
    void* address = reinterpret_cast<void*>(EbootBase + offset);
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    std::memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
}

struct Site {
    const char* name;
    u64 offset;
    std::array<u8, 6> original{};
    std::array<u8, 6> patched{};
    size_t size;
    Group group;
};

template <size_t N>
std::array<u8, 6> Pad(const std::array<u8, N>& bytes) {
    std::array<u8, 6> out{};
    std::memcpy(out.data(), bytes.data(), N);
    return out;
}

constexpr size_t SiteCount =
    Retargets.size() + EntryReturns.size() + ByteEdits.size() + WideEdits.size() + EntryJumps.size();
std::array<Site, SiteCount> sites{};
std::mutex patch_mutex;
bool patches_ready = false;
bool session_on = true; // session groups: off while a suspension runs
bool role_on = false;   // role groups (lamp): on while hosting

void BuildSites() {
    size_t i = 0;
    for (const auto& e : Retargets) {
        sites[i++] = {e.name, e.site, Pad(EncodeRel32(e.opcode, e.site, e.original)),
                      Pad(EncodeRel32(e.opcode, e.site, e.target)), 5, e.group};
    }
    for (const auto& e : EntryReturns) {
        std::array<u8, 6> ret{};
        ret[0] = 0xC3;
        sites[i++] = {e.name, e.site, e.prologue, ret, 1, e.group};
    }
    for (const auto& e : ByteEdits) {
        sites[i++] = {e.name, e.site, Pad(e.original), Pad(e.replacement), 2, e.group};
    }
    for (const auto& e : WideEdits) {
        sites[i++] = {e.name, e.site, Pad(e.original), Pad(e.replacement), 4, e.group};
    }
    for (const auto& e : EntryJumps) {
        sites[i++] = {e.name, e.site, Pad(e.prologue), Pad(EncodeRel32(0xE9, e.site, e.target)), 5,
                      e.group};
    }
}

// The game image: where the fork puts it, else the start of any executable region whose first few
// patch sites read as Bloodborne's (original or patched). AllSitesKnown then checks every site.
u64 FindEbootBase() {
    const auto fits = [](u64 base) {
        int checked = 0;
        for (const Site& s : sites) {
            if (s.offset + s.size > EbootSize) {
                continue;
            }
            if (!MatchesAt(base, s.offset, s.original.data(), s.size) &&
                !MatchesAt(base, s.offset, s.patched.data(), s.size)) {
                return false;
            }
            if (++checked == 4) {
                return true;
            }
        }
        return checked != 0;
    };
    if (fits(ForkEbootBase)) {
        return ForkEbootBase;
    }
    constexpr DWORD Executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION info{};
    u64 at = 0x10000;
    while (at < 0x7FFF'FFFF'0000ULL &&
           VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != 0) {
        const u64 base = reinterpret_cast<u64>(info.BaseAddress);
        if (info.State == MEM_COMMIT && (info.Protect & Executable) != 0 &&
            (info.Protect & PAGE_GUARD) == 0 && fits(base)) {
            return base;
        }
        at = base + info.RegionSize;
    }
    return 0;
}

bool AllSitesKnown(const Site** first_bad) {
    for (const Site& s : sites) {
        if (!GroupEnabled(s.group)) {
            continue;
        }
        if (s.offset + s.size > EbootSize ||
            (!Matches(s.offset, s.original.data(), s.size) &&
             !Matches(s.offset, s.patched.data(), s.size))) {
            *first_bad = &s;
            return false;
        }
    }
    return true;
}

// Whether a site should be patched right now.
bool Wanted(const Site& s) {
    if (!GroupEnabled(s.group)) {
        return false;
    }
    if (IsRoleGroup(s.group)) {
        return role_on;
    }
    if (IsSessionGroup(s.group)) {
        return session_on;
    }
    return true;
}

// Brings every enabled site to its wanted form. Returns how many were written.
int SyncSites() {
    int count = 0;
    for (const Site& s : sites) {
        if (!GroupEnabled(s.group)) {
            continue;
        }
        const auto& want = Wanted(s) ? s.patched : s.original;
        if (!Matches(s.offset, want.data(), s.size)) {
            Write(s.offset, want.data(), s.size);
            ++count;
        }
    }
    return count;
}

// ---- Start-up ------------------------------------------------------------------------------------

// LICENSE.txt (the GPL) and NOTICE.txt (copyright + source link) ship in the mod folder so a copy
// passed on keeps them. Missing or emptied -> BBSC stays off and says why.
bool NoticesPresent() {
    auto holds = [](const wchar_t* name, const char* text) {
        FILE* file = _wfopen((mod_dir / name).c_str(), L"rb");
        if (file == nullptr) {
            return false;
        }
        std::string content(64 * 1024, '\0');
        content.resize(fread(content.data(), 1, content.size(), file));
        fclose(file);
        return content.find(text) != std::string::npos;
    };
    // SUPPORTERS.txt (2026-10-11, the user): the supporters list travels with BBSC like the
    // license; its names change freely, only the file and its header are required.
    return holds(L"LICENSE.txt", "GNU GENERAL PUBLIC LICENSE") &&
           holds(L"NOTICE.txt", "github.com/Classmg22/BBSC") &&
           holds(L"SUPPORTERS.txt", "BBSC SUPPORTERS");
}

DWORD WINAPI Worker(void*) {
    OpenLog();
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // The build time tells two DLLs of one version apart in a tester's log (2026-10-18 triage: no
    // log said which 0.0.1 DLL a report came from).
    BbscMod::LogPlain("BBSC mod loaded - version " BBSC_VERSION " (built " __DATE__ " " __TIME__ ")");
    if (!NoticesPresent()) {
        LOG_ERROR(Mod, "LICENSE.txt / NOTICE.txt / SUPPORTERS.txt missing or changed in {} - BBSC "
                       "is off",
                  Common::FS::PathToUTF8String(mod_dir));
        BBSC_TIMELINE("START", "LICENSE.txt / NOTICE.txt / SUPPORTERS.txt missing or changed in {} - BBSC "
                       "is off",
                  Common::FS::PathToUTF8String(mod_dir));
        MessageBoxW(nullptr,
                    L"LICENSE.txt, NOTICE.txt or SUPPORTERS.txt is missing from the BBSC mod "
                    L"folder (or was changed), so BBSC is off.\n\nThese files have to travel with "
                    L"BBSC.\nDownload the full package again "
                    L"from:\nhttps://github.com/Classmg22/BBSC",
                    L"BBSC", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 0;
    }
    LOG_INFO(Mod, "BBSC mod in pid {} ({})", GetCurrentProcessId(),
             Common::FS::PathToUTF8String(std::filesystem::path{exe}));
    // Public servers (2026-10-11, shadNet's author: The Hunter's Dream is exact to the official
    // server, custom summon behaviour does not work there). Wozzardman's seamless summon patches
    // (cross-map sign search, host-placement summon build) and the 5-hunter search patches change
    // what the game asks the summon server for, so they are off there whatever the switches say
    // (review 2026-10-11: the Mods tab writes its defaults, BBSC_MOD_SEAMLESS=1 among them, at
    // every start, so "a switch the player set wins" would have kept them on).
    {
        std::string server;
        if (BbscMod::GameServerIsPublic(&server)) {
            const auto force_off = [](const char* name) {
                _putenv_s(name, "0");
                SetEnvironmentVariableA(name, "0");
            };
            force_off("BBSC_MOD_SEAMLESS");
            force_off("SHADPS4_BBSC_FIVE_HUNTERS");
            LOG_INFO(Mod, "Public game server ({}): vanilla summons - seamless summon patches and the "
                          "5-hunter search off (the session itself stays seamless)",
                     server);
            BBSC_TIMELINE("START", "Public game server ({}): vanilla summons - seamless summon patches and the "
                          "5-hunter search off (the session itself stays seamless)",
                     server);
        }
    }
    BuildSites();

    const DWORD started = GetTickCount();
    while ((EbootBase = FindEbootBase()) == 0) {
        if (GetTickCount() - started > WaitForImageMs) {
            LOG_ERROR(Mod, "No Bloodborne CUSA03173 01.09 image in memory after {} s - not this "
                           "game or not this version; nothing was changed",
                      WaitForImageMs / 1000);
            BBSC_TIMELINE("START", "No Bloodborne CUSA03173 01.09 image in memory after {} s - not this "
                           "game or not this version; nothing was changed",
                      WaitForImageMs / 1000);
            return 0;
        }
        Sleep(50);
    }
    LOG_INFO(Mod, "Game image found at {:#x}{}", EbootBase,
             EbootBase == ForkEbootBase ? "" : " (not the shadp2p fork's address)");
    // shadPS4 maps the game's code writable; other hosts (bbhost) keep it read-only, and the
    // shared hook code writes some code bytes directly. Make the image's code writable like shadPS4.
    {
        int unlocked = 0;
        MEMORY_BASIC_INFORMATION info{};
        u64 at = EbootBase;
        while (at < EbootBase + EbootSize &&
               VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != 0) {
            const u64 next = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
            if (info.State == MEM_COMMIT &&
                (info.Protect == PAGE_EXECUTE_READ || info.Protect == PAGE_EXECUTE)) {
                DWORD old = 0;
                const u64 end = std::min<u64>(next, EbootBase + EbootSize);
                if (VirtualProtect(info.BaseAddress, end - reinterpret_cast<u64>(info.BaseAddress),
                                   PAGE_EXECUTE_READWRITE, &old)) {
                    ++unlocked;
                }
            }
            at = next;
        }
        if (unlocked != 0) {
            LOG_INFO(Mod, "Game code made writable ({} region(s)) - this host maps it read-only",
                     unlocked);
        }
    }
    const Site* bad = nullptr;
    while (!AllSitesKnown(&bad)) {
        if (GetTickCount() - started > WaitForImageMs) {
            LOG_ERROR(Mod,
                      "No Bloodborne CUSA03173 01.09 image after {} s (first unknown site: {} at "
                      "{:#x}) - not this game or not this version; nothing was changed",
                      WaitForImageMs / 1000, bad ? bad->name : "?", bad ? bad->offset : 0);
            return 0;
        }
        Sleep(1);
    }
    Sleep(SettleMs);
    const DWORD verified = GetTickCount() - started;

    MemoryPatcher::g_eboot_address = EbootBase;
    MemoryPatcher::g_eboot_image_size = EbootSize;
    MemoryPatcher::g_game_serial = "CUSA03173";

    // Wozzardman's seamless patches first: one of them (0x019471B1) is also in BBSC's list, and it
    // checks for the original bytes.
    Core::Bloodborne::InstallSeamlessCoopPatches();

    int written = 0;
    {
        std::scoped_lock lock{patch_mutex};
        patches_ready = true;
        written = SyncSites();
    }
    LOG_INFO(Mod, "Image verified after {} ms: {} byte patch sites written", verified, written);
    BBSC_TIMELINE("START", "Image verified after {} ms: {} byte patch sites written", verified, written);

    Core::Bloodborne::InstallReverseEngineeringTrace();
    BbscMod::StartRelay();
    BbscMod::StartP2PChannel();
    BbscMod::InstallHttpWrappers();
    Core::Bbsc::InstallPadWrapper();
    BbscMod::StartDiscordStatus();
    LOG_INFO(Mod, "BBSC mod ready after {} ms", GetTickCount() - started);

    for (;;) {
        Sleep(WatchMs);
        std::scoped_lock lock{patch_mutex};
        if (const int redone = SyncSites(); redone != 0) {
            LOG_WARNING(Mod, "{} patch site(s) had changed back - written again", redone);
        }
    }
}

} // namespace

// ---- Logging -------------------------------------------------------------------------------------

namespace BbscMod {

void LogLine(char level, const char* category, const std::string& text) {
    std::scoped_lock lock{log_mutex};
    if (log_file == nullptr) {
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::fprintf(log_file, "[%02u:%02u:%02u.%03u] %c %s: %s\n", now.wHour, now.wMinute, now.wSecond,
                 now.wMilliseconds, level, category, text.c_str());
    std::fflush(log_file);
}

void LogPlain(const char* text) {
    LogLine('I', "Mod", text);
}

u64 EbootAddress() {
    return EbootBase;
}

// Import slot reads and writes that never take the game down (2026-10-19, BBport player, 0.0.2
// test): the game closed 3 s after start, five times in a row - "Fault: read access to
// 00000008053e4f48 at bbsc.dll+0x1933": BBSC read the Sendto slot while BBport's loader had that
// page unreadable (it links the eboot's imports itself at start). A slot is only touched while
// its page is committed and readable, and a fault in between (the page changing right after the
// check) is caught here. Used by the slot guard below and by the swaps in bbsc_http.cpp and
// bbsc_channel.cpp.
namespace {
bool SlotPageReadable(u64 address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & Readable) != 0;
}

bool TryReadSlot(const volatile u64* slot, u64* value) {
    __try {
        *value = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryWriteSlot(volatile u64* slot, u64 value) {
    __try {
        *slot = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReadSlotSafely(u64 address, u64* value) {
    return SlotPageReadable(address) &&
           TryReadSlot(reinterpret_cast<const volatile u64*>(address), value);
}

bool WriteSlotSafely(u64 address, u64 value) {
    if (!SlotPageReadable(address)) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), 8, PAGE_READWRITE, &old)) {
        return false;
    }
    const bool written = TryWriteSlot(reinterpret_cast<volatile u64*>(address), value);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), 8, old, &ignored);
    return written;
}

// Import slots the DLL swapped for its wrappers (bbsc_http.cpp, bbsc_channel.cpp). The official
// shadPS4 links the eboot's imports again when the game loads a library later (2026-10-10: no
// wrapper ever ran on the nightly - the slots were written back to the emulator's functions), so a
// thread puts the wrapper back whenever a slot no longer points at it, taking the new value as the
// original.
namespace {
struct GuardedSlot {
    u64 slot_offset;
    u64 wrapper;
    void** original; // the wrapper's function pointer (an aligned 8-byte store)
    const char* name;
    int rewritten = 0;
};
std::mutex guarded_mutex;
std::vector<GuardedSlot> guarded_slots;

DWORD WINAPI SlotGuardThread(void*) {
    int skipped = 0;
    for (;;) {
        Sleep(5);
        std::scoped_lock lock{guarded_mutex};
        for (auto& guarded : guarded_slots) {
            const u64 slot = EbootBase + guarded.slot_offset;
            u64 value = 0;
            if (!ReadSlotSafely(slot, &value)) {
                if (++skipped == 1) {
                    LOG_INFO(Mod, "Import {}: its page is not readable right now (the emulator is "
                                  "still linking) - checked again later",
                             guarded.name);
                }
                continue;
            }
            if (value == guarded.wrapper || value == 0 ||
                (value >= EbootBase && value < EbootBase + EbootSize)) {
                continue;
            }
            *reinterpret_cast<void* volatile*>(guarded.original) = reinterpret_cast<void*>(value);
            if (!WriteSlotSafely(slot, guarded.wrapper)) {
                continue; // the next round tries again
            }
            if (++guarded.rewritten <= 3) {
                LOG_INFO(Mod, "Import {} was linked again by the emulator - wrapper put back ({}x)",
                         guarded.name, guarded.rewritten);
            }
        }
    }
}
} // namespace

void GuardSlot(u64 slot_offset, u64 wrapper, void** original, const char* name) {
    std::scoped_lock lock{guarded_mutex};
    if (guarded_slots.empty()) {
        if (HANDLE thread = CreateThread(nullptr, 0, SlotGuardThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    guarded_slots.push_back({slot_offset, wrapper, original, name});
}

} // namespace BbscMod

// ---- What bloodborne_re.cpp asks the emulator for ----------------------------------------------

namespace Core {

bool MemoryManager::IsValidMapping(u64 address, u64 size) {
    return Readable(address, size == 0 ? 1 : size);
}

s32 MemoryManager::QueryProtection(u64 address, void** start, void** end, u32* prot) {
    CachedRegion info;
    if (!QueryRegion(address, info) || info.state != MEM_COMMIT) {
        return -1;
    }
    *start = reinterpret_cast<void*>(info.start);
    *end = reinterpret_cast<void*>(info.end);
    u32 bits = 0;
    switch (info.protect & 0xFF) {
    case PAGE_READONLY:
        bits = 1;
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        bits = 3;
        break;
    case PAGE_EXECUTE_READ:
        bits = 5;
        break;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        bits = 7;
        break;
    case PAGE_EXECUTE:
        bits = 4;
        break;
    default:
        bits = 0;
    }
    if ((info.protect & PAGE_GUARD) != 0) {
        bits = 0;
    }
    *prot = bits;
    return 0;
}

namespace Debugger {
int GetCurrentPid() {
    return static_cast<int>(GetCurrentProcessId());
}
} // namespace Debugger

namespace Bbsc {

void InstallPersistentSessionPatches() {} // the DLL applies them itself (Worker)

void SetPersistentSessionPatches(bool enabled) {
    std::scoped_lock lock{patch_mutex};
    if (!patches_ready || session_on == enabled) {
        return;
    }
    session_on = enabled;
    const int count = SyncSites();
    LOG_INFO(Debug, "BBSC persistence {} ({} session sites; echoes, lamp, fog walls and deaths "
                    "untouched)",
             enabled ? "re-applied" : "suspended", count);
}

void SetHostLampPatches(bool hosting) {
    std::scoped_lock lock{patch_mutex};
    if (!patches_ready || role_on == hosting) {
        return;
    }
    role_on = hosting;
    const int count = SyncSites();
    LOG_INFO(Debug, "BBSC lamp: {} ({} sites)",
             hosting ? "prompt allowed (hosting a session)"
                     : "vanilla gates (solo or guest - a guest never gets the lamp)",
             count);
}

// The F1 camera. The emulator build feeds it from its SDL window events (bbsc_cinematic.cpp); a mod
// sees no window events, so the keys are polled here instead, only while this process's window is
// in front (Nucleus runs two). While it is on, CineWindowProc keeps the keys and the mouse from the
// game (2026-10-12; before, they reached it too). bloodborne_re.cpp turns it on by itself when you
// die in co-op and off when you respawn.
// 2026-10-05 (the user: better mouse controls): the mouse looks around without a button held - the
// cursor is hidden and kept in the middle of the game window - and the wheel sets the speed (the
// game window is subclassed for WM_MOUSEWHEEL / WM_SETCURSOR while the camera is on; PageUp /
// PageDown still work).
std::mutex cine_mutex;
CinematicInput cine_state;
std::array<bool, 256> cine_was_down{};
bool cine_looking = false;
POINT cine_anchor{};
HWND cine_window = nullptr;          // the subclassed game window (SDL's "SDL_app")
WNDPROC cine_window_proc = nullptr;  // its own window procedure
std::atomic<bool> cine_mouse_owned{false}; // the camera is on and has the mouse
std::atomic<int> cine_wheel{0};      // wheel notches since the last TakeCinematicInput

LRESULT CALLBACK CineWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (cine_mouse_owned) {
        if (message == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            cine_wheel += delta > 0 ? 1 : (delta < 0 ? -1 : 0);
            return 0; // not for the game
        }
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        // 2026-10-12 (the user: "each button does a million things"): the camera's keys walked,
        // dodged and used items at the same time, and the mouse turned the game's camera. While
        // the camera is on, presses and movement stop here; the emulator (SDL) never sees them.
        // Releases still go through, so a key held when the camera came on is not left stuck.
        // The camera reads the keys itself (GetAsyncKeyState), so it is not affected. Alt+F4 passes.
        switch (message) {
        case WM_KEYDOWN:
        case WM_CHAR:
        case WM_DEADCHAR:
        case WM_SYSCHAR:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEMOVE:
            return 0;
        case WM_SYSKEYDOWN:
            if (wparam != VK_F4) {
                return 0;
            }
            break;
        case WM_INPUT: {
            // Raw input (SDL's relative mouse): drop movement and presses, keep releases.
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                bool release = false;
                if (raw.header.dwType == RIM_TYPEKEYBOARD) {
                    release = (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0;
                } else if (raw.header.dwType == RIM_TYPEMOUSE) {
                    constexpr USHORT Ups = RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP |
                                           RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP |
                                           RI_MOUSE_BUTTON_5_UP;
                    release = (raw.data.mouse.usButtonFlags & Ups) != 0;
                }
                if (!release) {
                    return DefWindowProcW(window, message, wparam, lparam);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return CallWindowProcW(cine_window_proc, window, message, wparam, lparam);
}

// The game window when it is in front, else nullptr. Subclassed the first time it is seen.
HWND CineGameWindowInFront() {
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (window == nullptr || pid != GetCurrentProcessId()) {
        return nullptr;
    }
    char cls[32]{};
    GetClassNameA(window, cls, sizeof(cls));
    if (std::strcmp(cls, "SDL_app") != 0) {
        return nullptr; // e.g. the console
    }
    if (cine_window != window) {
        if (cine_window != nullptr && cine_window_proc != nullptr) {
            SetWindowLongPtrW(cine_window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(cine_window_proc));
        }
        cine_window = window;
        cine_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CineWindowProc)));
        LOG_INFO(Debug, "BBSC cinematic camera: mouse wheel and cursor hooked on the game window");
    }
    return window;
}

// The controller: the game's scePadReadState import slot (0x053E4EA0, tools/gamedata/
// eboot_imports.py) is swapped for PadReadState below, which sees the pad data the emulator hands
// the game - any controller the emulator supports. L3 + R3 together toggles the camera; while it is
// on the sticks and buttons fly it and the game gets a pad at rest (the hunter does not move).
constexpr u64 PadReadStateSlot = 0x053E4EA0;
using PadReadStateFn = int PS4_SYSV_ABI (*)(s32 handle, u8* data);
PadReadStateFn original_pad_read = nullptr;
constexpr u32 PadL3 = 0x2, PadR3 = 0x4, PadUp = 0x10, PadRight = 0x20, PadDown = 0x40,
              PadLeft = 0x80, PadL1 = 0x400, PadR1 = 0x800, PadTriangle = 0x1000,
              PadCircle = 0x2000, PadCross = 0x4000, PadSquare = 0x8000, PadTouchpad = 0x100000,
              PadL2 = 0x100, PadR2 = 0x200;
// Not a pad button: HeldPadButtons sets it while the left stick (or WASD) moves the hunter.
constexpr u32 PadMoving = 0x40000000;
struct CinePad {
    u32 buttons = 0;
    u32 last_buttons = 0;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f, l2 = 0.0f, r2 = 0.0f;
    int speed_steps = 0, smooth_steps = 0;
    int switch_steps = 0; // R1 / L1 presses: the hunter to watch while dead
    int free_presses = 0; // touchpad (Xbox: View) presses: watch <-> fly freely while dead
    bool zoom_reset = false;
};
CinePad cine_pad;

float PadAxis(u8 value) {
    const float v = (static_cast<float>(value) - 128.0f) / 127.0f;
    constexpr float Dead = 0.15f;
    return std::abs(v) < Dead ? 0.0f : std::clamp((v - std::copysign(Dead, v)) / (1.0f - Dead), -1.0f, 1.0f);
}

// When the game last read the pad through the wrapper (GetTickCount64), see the XInput fallback.
std::atomic<u64> pad_read_last_ms{0};
std::atomic<u64> pad_reads{0};

// OrbisPadData: +0 buttons (u32), +4 left stick x/y, +6 right stick x/y, +8 L2, +9 R2 (u8).
int PS4_SYSV_ABI PadReadState(s32 handle, u8* data) {
    const int result = original_pad_read(handle, data);
    if (result != 0 || data == nullptr) {
        return result;
    }
    pad_read_last_ms = GetTickCount64();
    ++pad_reads;
    u32 buttons = 0;
    std::memcpy(&buttons, data, 4);
    if (std::hypot(PadAxis(data[4]), PadAxis(data[5])) > 0.3f) {
        buttons |= PadMoving;
    }
    std::scoped_lock lock{cine_mutex};
    const u32 pressed = buttons & ~cine_pad.last_buttons;
    cine_pad.last_buttons = buttons;
    if ((buttons & (PadL3 | PadR3)) == (PadL3 | PadR3) && (pressed & (PadL3 | PadR3)) != 0) {
        const bool enabled = !cine_state.enabled;
        cine_state = CinematicInput{};
        cine_state.enabled = enabled;
        LOG_INFO(Debug, "BBSC cinematic camera: {} (L3 + R3)", enabled ? "on" : "off");
    }
    if (!cine_state.enabled) {
        cine_pad = CinePad{.last_buttons = buttons};
        return result;
    }
    cine_pad.buttons = buttons;
    cine_pad.lx = PadAxis(data[4]);
    cine_pad.ly = PadAxis(data[5]);
    cine_pad.rx = PadAxis(data[6]);
    cine_pad.ry = PadAxis(data[7]);
    cine_pad.l2 = static_cast<float>(data[8]) / 255.0f;
    cine_pad.r2 = static_cast<float>(data[9]) / 255.0f;
    cine_pad.speed_steps += ((pressed & PadUp) ? 1 : 0) - ((pressed & PadDown) ? 1 : 0);
    cine_pad.smooth_steps += ((pressed & PadRight) ? 1 : 0) - ((pressed & PadLeft) ? 1 : 0);
    cine_pad.zoom_reset = cine_pad.zoom_reset || (pressed & PadTriangle) != 0;
    cine_pad.switch_steps += ((pressed & PadR1) ? 1 : 0) - ((pressed & PadL1) ? 1 : 0);
    cine_pad.free_presses += (pressed & PadTouchpad) != 0 ? 1 : 0;
    // The game sees a controller at rest (connected, nothing pressed).
    const u32 rest_buttons = buttons & 0x80000000u;
    std::memcpy(data, &rest_buttons, 4);
    data[4] = data[5] = data[6] = data[7] = 128;
    data[8] = data[9] = 0;
    return result;
}

void HideGamePadWhileCameraOn(); // see "The game's own controller reads"

void InstallPadWrapper() {
    HideGamePadWhileCameraOn();
    auto* slot = reinterpret_cast<u64*>(EbootBase + PadReadStateSlot);
    for (int i = 0; i < 60'000; ++i) {
        const u64 value = *slot;
        if (value != 0 && (value < EbootBase || value >= EbootBase + EbootSize)) {
            original_pad_read = reinterpret_cast<PadReadStateFn>(value);
            DWORD old = 0;
            VirtualProtect(slot, 8, PAGE_READWRITE, &old);
            *slot = reinterpret_cast<u64>(&PadReadState);
            DWORD ignored = 0;
            VirtualProtect(slot, 8, old, &ignored);
            LOG_INFO(Mod, "Pad: controller works the F1 camera too (L3 + R3)");
            return;
        }
        Sleep(1);
    }
    LOG_WARNING(Mod, "Pad: scePadReadState slot never filled - F1 camera keyboard only");
}

// The controller fallback (2026-10-21, the user: "when dead I cannot freelook with my controller").
// The wrapper above never sees a read on BBport (log: "0 game pad read(s) so far, the last never"),
// so when the game has not read the pad through it for 500 ms, the camera reads the controller
// itself: first through SDL3, which BBport reads the controller with (bin\SDL3.dll, already loaded
// in this process: any controller the game sees, DualShock and DualSense included), then through
// XInput (Xbox pads, Steam Input). The game still gets that controller's input then (the hunter is
// dead when the camera comes on by itself). Called with cine_mutex held.
void ApplyCinePadLocked(u32 buttons, float lx, float ly, float rx, float ry, float l2, float r2,
                        const char* source) {
    // For HeldPadButtons (the revive stops on them): the triggers as L2 / R2, the stick moving.
    buttons |= (l2 > 0.5f ? PadL2 : 0) | (r2 > 0.5f ? PadR2 : 0) |
               (std::hypot(lx, ly) > 0.3f ? PadMoving : 0);
    const u32 pressed = buttons & ~cine_pad.last_buttons;
    cine_pad.last_buttons = buttons;
    if ((buttons & (PadL3 | PadR3)) == (PadL3 | PadR3) && (pressed & (PadL3 | PadR3)) != 0) {
        const bool enabled = !cine_state.enabled;
        cine_state = CinematicInput{};
        cine_state.enabled = enabled;
        LOG_INFO(Debug, "BBSC cinematic camera: {} (L3 + R3, {})", enabled ? "on" : "off", source);
    }
    if (!cine_state.enabled) {
        return;
    }
    cine_pad.buttons = buttons;
    cine_pad.lx = lx;
    cine_pad.ly = ly; // the PS4 pad's y grows downwards
    cine_pad.rx = rx;
    cine_pad.ry = ry;
    cine_pad.l2 = l2;
    cine_pad.r2 = r2;
    cine_pad.speed_steps += ((pressed & PadUp) ? 1 : 0) - ((pressed & PadDown) ? 1 : 0);
    cine_pad.smooth_steps += ((pressed & PadRight) ? 1 : 0) - ((pressed & PadLeft) ? 1 : 0);
    cine_pad.zoom_reset = cine_pad.zoom_reset || (pressed & PadTriangle) != 0;
    // While dead: R1 / L1 the next / previous hunter, the touchpad (View) watch <-> free. Only the
    // game's own pad reads counted them before (2026-10-24: not working with the controller).
    cine_pad.switch_steps += ((pressed & PadR1) ? 1 : 0) - ((pressed & PadL1) ? 1 : 0);
    cine_pad.free_presses += (pressed & PadTouchpad) != 0 ? 1 : 0;
}

float StickAxis(float v) {
    constexpr float Dead = 0.15f;
    return std::abs(v) < Dead ? 0.0f : std::clamp((v - std::copysign(Dead, v)) / (1.0f - Dead), -1.0f, 1.0f);
}

// SDL3, looked up by name in the SDL3.dll BBport loaded (nothing linked, nothing loaded by us).
struct SdlPadApi {
    using GetGamepadsFn = u32* (*)(int* count);
    using FreeFn = void (*)(void* memory);
    using OpenGamepadFn = void* (*)(u32 instance_id);
    using GetAxisFn = s16 (*)(void* gamepad, int axis);
    using GetButtonFn = bool (*)(void* gamepad, int button);
    GetGamepadsFn get_gamepads = nullptr;
    FreeFn free = nullptr;
    OpenGamepadFn open = nullptr;
    GetAxisFn axis = nullptr;
    GetButtonFn button = nullptr;
};

// The game's own controller reads (2026-10-22, the user: "free cam still has some input on the
// character (R2 L2)"). BBport polls the pad itself through SDL_GetGamepadAxis /
// SDL_GetGamepadButton, imported from SDL3.dll, so the sticks, triggers and buttons that fly the
// camera also moved and attacked with the hunter. While the camera is on, those two entries of the
// exe's import table answer a pad at rest. The camera's own reads (below) look the functions up in
// SDL3.dll itself and still see the real pad.
std::atomic<bool> cine_pad_hidden{false};
SdlPadApi::GetAxisFn game_pad_axis = nullptr;
SdlPadApi::GetButtonFn game_pad_button = nullptr;

s16 GamePadAxis(void* gamepad, int axis) {
    return cine_pad_hidden ? 0 : game_pad_axis(gamepad, axis);
}

bool GamePadButton(void* gamepad, int button) {
    return cine_pad_hidden ? false : game_pad_button(gamepad, button);
}

// Points the exe's import of `name` from `dll` at `replacement`; the original, or nullptr when the
// exe does not import it (an emulator build with SDL linked in).
void* PatchExeImport(const char* dll, const char* name, void* replacement) {
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return nullptr;
    }
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) {
            continue;
        }
        const DWORD names_rva = desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + names_rva);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) {
                continue;
            }
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                return nullptr;
            }
            void* original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return original;
        }
    }
    return nullptr;
}

void HideGamePadWhileCameraOn() {
    game_pad_axis = reinterpret_cast<SdlPadApi::GetAxisFn>(
        PatchExeImport("SDL3.dll", "SDL_GetGamepadAxis", reinterpret_cast<void*>(&GamePadAxis)));
    game_pad_button = reinterpret_cast<SdlPadApi::GetButtonFn>(
        PatchExeImport("SDL3.dll", "SDL_GetGamepadButton", reinterpret_cast<void*>(&GamePadButton)));
    if (game_pad_axis != nullptr && game_pad_button != nullptr) {
        LOG_INFO(Mod, "Pad: the game's controller reads come through BBSC - at rest while the camera is on");
        return;
    }
    // One without the other would leave half the pad to the game: put a patched one back.
    if (game_pad_axis != nullptr) {
        PatchExeImport("SDL3.dll", "SDL_GetGamepadAxis", reinterpret_cast<void*>(game_pad_axis));
    }
    if (game_pad_button != nullptr) {
        PatchExeImport("SDL3.dll", "SDL_GetGamepadButton", reinterpret_cast<void*>(game_pad_button));
    }
    game_pad_axis = nullptr;
    game_pad_button = nullptr;
    LOG_INFO(Mod, "Pad: the exe does not import SDL3's gamepad reads - the camera can't keep the pad "
                  "from the game");
}

bool PollSdlPadLocked() {
    static SdlPadApi sdl = [] {
        SdlPadApi api;
        HMODULE module = GetModuleHandleA("SDL3.dll");
        if (module == nullptr) {
            LOG_INFO(Mod, "Pad: SDL3 is not loaded here - the F1 camera falls back to XInput");
            return api;
        }
        api.get_gamepads = reinterpret_cast<SdlPadApi::GetGamepadsFn>(GetProcAddress(module, "SDL_GetGamepads"));
        api.free = reinterpret_cast<SdlPadApi::FreeFn>(GetProcAddress(module, "SDL_free"));
        api.open = reinterpret_cast<SdlPadApi::OpenGamepadFn>(GetProcAddress(module, "SDL_OpenGamepad"));
        api.axis = reinterpret_cast<SdlPadApi::GetAxisFn>(GetProcAddress(module, "SDL_GetGamepadAxis"));
        api.button = reinterpret_cast<SdlPadApi::GetButtonFn>(GetProcAddress(module, "SDL_GetGamepadButton"));
        const bool ok = api.get_gamepads && api.free && api.open && api.axis && api.button;
        if (!ok) {
            api = SdlPadApi{};
        }
        LOG_INFO(Mod, "Pad: SDL3 controller fallback for the F1 camera {}", ok ? "ready" : "missing functions");
        return api;
    }();
    if (sdl.get_gamepads == nullptr) {
        return false;
    }
    static void* gamepad = nullptr;
    static u32 gamepad_id = 0;
    static u64 looked_ms = 0;
    const u64 now = GetTickCount64();
    if (gamepad == nullptr || now - looked_ms > 5'000) {
        looked_ms = now;
        int count = 0;
        u32* ids = sdl.get_gamepads(&count);
        const u32 first = ids != nullptr && count > 0 ? ids[0] : 0;
        if (ids != nullptr) {
            sdl.free(ids);
        }
        if (first == 0) {
            gamepad = nullptr;
            return false;
        }
        if (gamepad == nullptr || first != gamepad_id) {
            gamepad = sdl.open(first); // a second open of the same pad: SDL counts it
            gamepad_id = first;
            LOG_INFO(Mod, "Pad: SDL3 controller {} {} for the F1 camera", first,
                     gamepad != nullptr ? "opened" : "could not be opened");
        }
    }
    if (gamepad == nullptr) {
        return false;
    }
    // SDL_GamepadButton: 0 south, 2 west, 3 north, 7/8 sticks, 9/10 shoulders, 11-14 d-pad up,
    // down, left, right. SDL_GamepadAxis: 0-3 sticks (y down positive), 4/5 triggers (0..32767).
    const auto pressed = [&](int b) { return sdl.button(gamepad, b); };
    u32 buttons = 0;
    buttons |= pressed(11) ? PadUp : 0;
    buttons |= pressed(12) ? PadDown : 0;
    buttons |= pressed(13) ? PadLeft : 0;
    buttons |= pressed(14) ? PadRight : 0;
    buttons |= pressed(7) ? PadL3 : 0;
    buttons |= pressed(8) ? PadR3 : 0;
    buttons |= pressed(9) ? PadL1 : 0;
    buttons |= pressed(10) ? PadR1 : 0;
    buttons |= pressed(0) ? PadCross : 0;
    buttons |= pressed(1) ? PadCircle : 0;
    buttons |= pressed(2) ? PadSquare : 0;
    buttons |= pressed(3) ? PadTriangle : 0;
    buttons |= pressed(20) || pressed(4) ? PadTouchpad : 0; // touchpad, or Back / View
    const auto axis = [&](int a) { return static_cast<float>(sdl.axis(gamepad, a)) / 32767.0f; };
    ApplyCinePadLocked(buttons, StickAxis(axis(0)), StickAxis(axis(1)), StickAxis(axis(2)),
                       StickAxis(axis(3)), std::clamp(axis(4), 0.0f, 1.0f),
                       std::clamp(axis(5), 0.0f, 1.0f), "SDL3");
    return true;
}

struct XiGamepad {
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
};
struct XiState {
    DWORD packet;
    XiGamepad pad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD user, XiState* state);

bool PollXInputPadLocked() {
    static XInputGetStateFn get_state = [] {
        for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            if (HMODULE module = LoadLibraryA(dll)) {
                if (auto fn = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"))) {
                    LOG_INFO(Mod, "Pad: XInput fallback for the F1 camera ready ({})", dll);
                    return fn;
                }
            }
        }
        LOG_WARNING(Mod, "Pad: no XInput - the F1 camera has no XInput fallback");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    if (get_state == nullptr) {
        return false;
    }
    XiState state{};
    bool found = false;
    for (DWORD user = 0; user < 4 && !found; ++user) {
        found = get_state(user, &state) == ERROR_SUCCESS;
    }
    if (!found) {
        return false;
    }
    const WORD xb = state.pad.buttons;
    u32 buttons = 0;
    buttons |= (xb & 0x0001) ? PadUp : 0;
    buttons |= (xb & 0x0002) ? PadDown : 0;
    buttons |= (xb & 0x0004) ? PadLeft : 0;
    buttons |= (xb & 0x0008) ? PadRight : 0;
    buttons |= (xb & 0x0040) ? PadL3 : 0;
    buttons |= (xb & 0x0080) ? PadR3 : 0;
    buttons |= (xb & 0x0100) ? PadL1 : 0;
    buttons |= (xb & 0x0200) ? PadR1 : 0;
    buttons |= (xb & 0x1000) ? PadCross : 0;
    buttons |= (xb & 0x2000) ? PadCircle : 0;
    buttons |= (xb & 0x4000) ? PadSquare : 0;
    buttons |= (xb & 0x8000) ? PadTriangle : 0;
    buttons |= (xb & 0x0020) ? PadTouchpad : 0; // Back / View
    const auto axis = [](SHORT v) { return StickAxis(static_cast<float>(v) / 32767.0f); };
    // XInput's y grows upwards.
    ApplyCinePadLocked(buttons, axis(state.pad.lx), -axis(state.pad.ly), axis(state.pad.rx),
                       -axis(state.pad.ry), static_cast<float>(state.pad.left_trigger) / 255.0f,
                       static_cast<float>(state.pad.right_trigger) / 255.0f, "XInput");
    return true;
}

bool CineKeyPressed(int key, bool down) {
    const bool pressed = down && !cine_was_down[key];
    cine_was_down[key] = down;
    return pressed;
}

CinematicInput TakeCinematicInput() {
    std::scoped_lock lock{cine_mutex};
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool front = pid == GetCurrentProcessId();
    auto down = [front](int key) { return front && (GetAsyncKeyState(key) & 0x8000) != 0; };
    if (CineKeyPressed(VK_F1, down(VK_F1))) {
        const bool enabled = !cine_state.enabled;
        cine_state = CinematicInput{};
        cine_state.enabled = enabled;
        LOG_INFO(Debug, "BBSC cinematic camera: {} (F1)", enabled ? "on" : "off");
    }
    const u64 tick = GetTickCount64();
    const bool game_reads_pad = tick - pad_read_last_ms.load() < 500;
    static const char* pad_source = "none";
    if (!game_reads_pad) {
        pad_source = PollSdlPadLocked() ? "SDL3" : PollXInputPadLocked() ? "XInput" : "no controller found";
    }
    static bool was_enabled = false;
    if (cine_state.enabled && !was_enabled) {
        const u64 last = pad_read_last_ms.load();
        LOG_INFO(Debug, "BBSC cinematic camera: controller - {} game pad read(s) so far, the last {} "
                        "- {}",
                 pad_reads.load(), last == 0 ? std::string{"never"} : fmt::format("{} ms ago", tick - last),
                 game_reads_pad ? "the game's pad" : pad_source);
    }
    was_enabled = cine_state.enabled;
    cine_pad_hidden = cine_state.enabled && game_pad_axis != nullptr;
    if (!cine_state.enabled) {
        cine_looking = false;
        cine_mouse_owned = false;
        cine_wheel = 0;
        return {};
    }
    CinematicInput& c = cine_state;
    const CinePad& p = cine_pad;
    c.forward = down('W');
    c.back = down('S');
    c.left = down('A');
    c.right = down('D');
    c.up = down('E') || down(VK_SPACE);
    c.down = down('Q') || down('C');
    c.fast = down(VK_SHIFT) || (p.buttons & PadCross) != 0;
    c.slow = down(VK_MENU) || (p.buttons & PadSquare) != 0;
    c.zoom_in = down('Z');
    c.zoom_out = down('X');
    c.zoom_reset = CineKeyPressed('V', down('V')) || p.zoom_reset;
    cine_pad.zoom_reset = false;
    c.roll_left = down(VK_OEM_COMMA);
    c.roll_right = down(VK_OEM_PERIOD);
    c.roll_reset = CineKeyPressed(VK_OEM_2, down(VK_OEM_2));
    c.look_up = down(VK_UP);
    c.look_down = down(VK_DOWN);
    c.look_left = down(VK_LEFT);
    c.look_right = down(VK_RIGHT);
    c.wheel = (CineKeyPressed(VK_PRIOR, down(VK_PRIOR)) ? 1 : 0) -
              (CineKeyPressed(VK_NEXT, down(VK_NEXT)) ? 1 : 0) + p.speed_steps +
              cine_wheel.exchange(0);
    c.smooth_steps = (CineKeyPressed(VK_OEM_6, down(VK_OEM_6)) ? 1 : 0) -
                     (CineKeyPressed(VK_OEM_4, down(VK_OEM_4)) ? 1 : 0) + p.smooth_steps;
    cine_pad.speed_steps = 0;
    cine_pad.smooth_steps = 0;
    // Watching a living hunter while dead: Tab / E / left click next, Shift+Tab / Q previous,
    // R1 / L1 on the controller; F flies freely instead and back (bloodborne_re.cpp).
    const bool shift = down(VK_SHIFT);
    c.switch_target = (CineKeyPressed(VK_TAB, down(VK_TAB)) ? (shift ? -1 : 1) : 0) +
                      (CineKeyPressed('E', down('E')) ? 1 : 0) -
                      (CineKeyPressed('Q', down('Q')) ? 1 : 0) +
                      (CineKeyPressed(VK_LBUTTON, down(VK_LBUTTON)) ? 1 : 0) + p.switch_steps;
    cine_pad.switch_steps = 0;
    c.toggle_free = CineKeyPressed('F', down('F')) || p.free_presses > 0;
    cine_pad.free_presses = 0;
    c.pad_move_x = p.lx;
    c.pad_move_y = -p.ly; // stick up = smaller y = forward
    c.pad_rise = ((p.buttons & PadR1) ? 1.0f : 0.0f) - ((p.buttons & PadL1) ? 1.0f : 0.0f);
    c.pad_look_x = p.rx;
    c.pad_look_y = -p.ry;
    c.pad_zoom = p.r2 - p.l2;
    // Mouse look, no button needed: the cursor's movement, put back in the middle of the game
    // window each frame (hidden there by CineWindowProc).
    c.mouse_dx = 0.0f;
    c.mouse_dy = 0.0f;
    POINT now{};
    RECT client{};
    HWND window = front ? CineGameWindowInFront() : nullptr;
    if (window != nullptr && GetClientRect(window, &client) && GetCursorPos(&now)) {
        POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
        ClientToScreen(window, &center);
        if (cine_looking) {
            c.mouse_dx = static_cast<float>(now.x - cine_anchor.x);
            c.mouse_dy = static_cast<float>(now.y - cine_anchor.y);
        }
        cine_anchor = center;
        cine_looking = true;
        SetCursorPos(center.x, center.y);
        cine_mouse_owned = true;
    } else {
        cine_looking = false; // another window is in front: the mouse is the user's
        cine_mouse_owned = false;
    }
    return c;
}

void CinematicForceOff() {
    std::scoped_lock lock{cine_mutex};
    cine_state = CinematicInput{};
    cine_pad = CinePad{.last_buttons = cine_pad.last_buttons};
    cine_looking = false;
    cine_mouse_owned = false;
    cine_pad_hidden = false;
}

unsigned HeldPadButtons() {
    // Only while our window is in front, the controller as the keys: it reaches every game on the
    // PC (2026-10-24, two games on one PC: Circle held to run in one sent the other to the lamp).
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        return 0;
    }
    unsigned buttons = 0;
    {
        // last_buttons, not buttons: buttons only follows the pad while the camera is on
        // (2026-10-24: a living hunter's X never reached the revive).
        std::scoped_lock lock{cine_mutex};
        buttons = cine_pad.last_buttons;
    }
    buttons |= (GetAsyncKeyState('E') & 0x8000) != 0 ? PadCross : 0;
    buttons |= (GetAsyncKeyState('G') & 0x8000) != 0 ? PadCircle : 0;
    const auto key = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
    if (key('W') || key('A') || key('S') || key('D')) {
        buttons |= PadMoving;
    }
    if (key(VK_LBUTTON) || key(VK_RBUTTON) || key(VK_SPACE)) {
        buttons |= PadR1; // attacks, the dodge: anything that is not the revive
    }
    return buttons;
}

void CinematicForceOn() {
    std::scoped_lock lock{cine_mutex};
    if (cine_state.enabled) {
        return;
    }
    cine_state = CinematicInput{};
    cine_state.enabled = true;
    cine_pad = CinePad{.last_buttons = cine_pad.last_buttons};
    cine_looking = false;
}

// The mod reads the controller itself (PadReadState: the whole pad goes to the camera while it is
// on), so watching needs nothing more here.
void CinematicSetWatching(bool) {}
void CinematicPadRead(unsigned, unsigned char, unsigned char, unsigned char, unsigned char) {}

} // namespace Bbsc
} // namespace Core

namespace Common::FS {
std::filesystem::path GetUserPath(PathType type) {
    return type == PathType::CapturesDir ? mod_dir / "captures" : mod_dir;
}
} // namespace Common::FS

namespace Libraries::Np::NpMatching2 {
int BbscReplayRoomMembers(bool) {
    return 0; // the emulator's room cache is out of reach for a mod
}
} // namespace Libraries::Np::NpMatching2

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
