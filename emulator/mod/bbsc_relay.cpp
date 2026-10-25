// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC mod: the new home of BBSC's own packets (host position / follow, world flags, Dream state).
// In the emulator build they rode on the emulator's P2P signaling. A mod cannot reach that, so
// they go through the BBSC game server instead, which every host already runs and every player's
// game already talks to: one TCP connection to its relay port (20444), frames of
// [u32 little-endian length][payload], and the server hands every frame to every other player
// connected to it. The payloads are the signaling packets unchanged ("SHAD" + type 0x20 follow,
// 0x22 world flags), so the receiving code is the same.
//
// No relay (public servers, or ours not reachable): the same frames go over the game's own P2P
// connection instead (bbsc_channel.cpp), and come back into Dispatch from there.
//
// Server address: BBSC_RELAY=host[:port], else the game server entry of host_overrides.json
// (portable install: <emulator>\user\, else %APPDATA%\shadPS4\) - the address JOIN-HOST wrote.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/bloodborne_re.h"
#include "core/libraries/np/np_signaling/np_signaling_bbsc.h"
#include "core/libraries/np/np_signaling/np_signaling_helpers.h"

namespace BbscMod {
void LogPlain(const char* text);
void OnHostPlacement(const std::string& placement, u32 sender);
void OnAimedHostPlacement(s64 target_user, const std::string& placement, u32 sender);
bool IsPublicServerAddress(const std::string& url_or_host); // bbsc_http.cpp
std::vector<std::wstring> HostOverridesFiles();              // bbsc_http.cpp
u32 P2PChannelSend(const void* data, u32 size);             // bbsc_channel.cpp
u32 P2PChannelPeerCount();
}

namespace {

constexpr u16 DefaultRelayPort = 20444;
constexpr u8 FollowType = 0x20;
constexpr u8 PlacementType = 0x30; // "SHAD" 0x30 + a host's placement text (before the hotfix, ignored now)
// "SHAD" 0x31 + "<target UserId>|<placement>": a host rang that sign (0.0.1 hotfix, bbsc_http.cpp)
constexpr u8 AimedPlacementType = 0x31;
constexpr u8 LifeType = 0x23;   // "SHAD" 0x23 + u32 player, u8 dead, u32 sequence
constexpr u8 ScaleType = 0x24;  // "SHAD" 0x24 + 5 x u16 the host's scaling percentages
constexpr u8 PvpType = 0x25;    // "SHAD" 0x25 + u8 kind (0 host state, 1 guest asks) + u8 on + u32
constexpr u8 ProgressType = 0x26; // "SHAD" 0x26 + u8 count + count x u32 the host's boss flags
constexpr u8 HostBossType = 0x27; // "SHAD" 0x27 + s32 the boss on the host's boss bar (-1 none)
constexpr u8 StainType = 0x2B; // "SHAD" 0x2B + u8 kind (1 new, 2 picked up, 3 lost) + u32 the host's stain
constexpr u8 PeersType = 0x7F; // server -> client: "N other players are connected", 1 byte count
// Every frame since 2026-10-06: "SHAD" 0x40 + u32 sender + u32 session (the host's sender id, 0 = in
// no session) + the packet above. The server sends each frame to everyone connected to it, so the
// receiver keeps only its own session's packets (see "Relay sessions" in bloodborne_re.cpp).
// Frames without it come from an older BBSC and are dropped.
constexpr u8 EnvelopeType = 0x40;
constexpr size_t EnvelopeSize = 13;

#pragma pack(push, 1)
struct FollowPacket {
    u8 magic[4] = {'S', 'H', 'A', 'D'};
    u8 type = FollowType;
    u8 kind = 0;
    u32 packed_map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;
    u32 sequence = 0;
    s32 lamp_point = -1;
};
#pragma pack(pop)
static_assert(sizeof(FollowPacket) == 0x22);

std::mutex send_mutex;
std::mutex dispatch_mutex; // the relay thread and the P2P channel's thread both deliver frames
SOCKET relay = INVALID_SOCKET;
// Frames wait here for the writer thread (2026-10-13): a public player's link dropped exactly 10 s
// after every reconnect while hosting - the 1/s world flags filled the socket while the server
// was stuck, and the game thread's 300 ms send timeout closed the link. Now the game thread only
// queues; a full queue drops frames instead of the link. All under send_mutex.
std::condition_variable send_cv;
std::deque<std::vector<char>> send_queue;
size_t send_queue_bytes = 0;
constexpr size_t SendQueueLimit = 256 * 1024;
SOCKET writing = INVALID_SOCKET; // the socket the writer is sending on right now
u32 dropped_frames = 0;          // since this connection began
std::string lost_reason;         // why the writer gave up on this connection
std::atomic<int> peers{0};
std::atomic<bool> started{false};
u32 own_id = 0; // set once in StartRelay, never 0
std::atomic<bool> logged_old_sender{false};

void Log(const std::string& text) {
    BbscMod::LogPlain(("Relay: " + text).c_str());
    Core::Bloodborne::BbscTimelineEvent("RELAY", text); // status changes only, see the callers
}

std::string ReadFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// "http://26.1.2.3:20443" as the value of the ss4.scej-network.jp key -> "26.1.2.3".
std::string HostFromOverrides(const std::string& json) {
    const size_t key = json.find("ss4.scej-network.jp");
    if (key == std::string::npos) {
        return {};
    }
    const size_t colon = json.find(':', json.find('"', key + 1) + 1);
    const size_t open = json.find('"', colon);
    const size_t close = json.find('"', open + 1);
    if (colon == std::string::npos || open == std::string::npos || close == std::string::npos) {
        return {};
    }
    std::string url = json.substr(open + 1, close - open - 1);
    if (const size_t scheme = url.find("://"); scheme != std::string::npos) {
        url = url.substr(scheme + 3);
    }
    return url.substr(0, url.find_first_of(":/"));
}

bool FindServer(std::string& host, u16& port) {
    port = DefaultRelayPort;
    if (const char* env = std::getenv("BBSC_RELAY"); env != nullptr && env[0] != '\0') {
        std::string value = env;
        if (const size_t colon = value.find(':'); colon != std::string::npos) {
            port = static_cast<u16>(std::atoi(value.c_str() + colon + 1));
            value.resize(colon);
        }
        host = value;
        return true;
    }
    for (const auto& path : BbscMod::HostOverridesFiles()) {
        host = HostFromOverrides(ReadFile(path));
        if (!host.empty()) {
            return true;
        }
    }
    return false;
}

int last_read_result = 0; // 0 = the server closed the link, < 0 = recv error (relay thread only)

bool ReadAll(SOCKET s, void* data, int size) {
    auto* at = static_cast<char*>(data);
    while (size > 0) {
        const int got = recv(s, at, size, 0);
        if (got <= 0) {
            last_read_result = got == 0 ? 0 : -WSAGetLastError();
            return false;
        }
        at += got;
        size -= got;
    }
    return true;
}

// Our session as the relay sees it: hosting -> ourselves, a guest (or joining) -> our host.
u32 SessionKey() {
    const s32 state = Core::Bloodborne::GetBbscRelaySessionState();
    if (state == 3) {
        return own_id;
    }
    if (state == 4 || state == 6) {
        return Core::Bloodborne::GetBbscRelayHost();
    }
    return 0;
}

// Returns how many players it went to: the relay's count, or (no relay connected - public servers)
// the players the P2P channel sent it to (bbsc_channel.cpp).
u32 SendFrame(const void* data, u32 size) {
    std::scoped_lock lock{send_mutex};
    const u32 key = SessionKey();
    const u32 wrapped = static_cast<u32>(EnvelopeSize) + size;
    std::vector<char> frame(4 + wrapped);
    std::memcpy(frame.data(), &wrapped, 4);
    std::memcpy(frame.data() + 4, "SHAD", 4);
    frame[8] = static_cast<char>(EnvelopeType);
    std::memcpy(frame.data() + 9, &own_id, 4);
    std::memcpy(frame.data() + 13, &key, 4);
    std::memcpy(frame.data() + 4 + EnvelopeSize, data, size);
    if (relay == INVALID_SOCKET) {
        return BbscMod::P2PChannelSend(frame.data() + 4, wrapped);
    }
    if (send_queue_bytes + frame.size() > SendQueueLimit) {
        if (dropped_frames++ == 0) {
            Log("the server is not taking our frames (" + std::to_string(send_queue_bytes) +
                " bytes waiting) - dropping new ones until it does");
        }
        return static_cast<u32>(peers.load());
    }
    send_queue_bytes += frame.size();
    send_queue.push_back(std::move(frame));
    send_cv.notify_all();
    return static_cast<u32>(peers.load());
}

std::string SocketError(const char* what) {
    return std::string(what) + " (WSA error " + std::to_string(WSAGetLastError()) + ")";
}

// Sends the queued frames, one socket at a time. A failed send shuts the socket down; the relay
// thread sees that in recv and reconnects.
DWORD WINAPI WriterThread(void*) {
    for (;;) {
        std::vector<char> frame;
        SOCKET s = INVALID_SOCKET;
        {
            std::unique_lock lock{send_mutex};
            send_cv.wait(lock, [] { return !send_queue.empty(); });
            frame = std::move(send_queue.front());
            send_queue.pop_front();
            send_queue_bytes -= frame.size();
            s = relay;
            if (s == INVALID_SOCKET) {
                continue;
            }
            writing = s;
        }
        std::string problem;
        int left = static_cast<int>(frame.size());
        const char* at = frame.data();
        while (left > 0) {
            const int sent = send(s, at, left, 0);
            if (sent <= 0) {
                problem = SocketError("send failed");
                break;
            }
            at += sent;
            left -= sent;
        }
        std::scoped_lock lock{send_mutex};
        writing = INVALID_SOCKET;
        if (!problem.empty() && relay == s) {
            if (lost_reason.empty()) {
                lost_reason = problem;
            }
            shutdown(s, SD_BOTH);
        }
        send_cv.notify_all();
    }
}

void Dispatch(const std::vector<u8>& wrapped) {
    if (wrapped.size() == 2 && wrapped[0] == PeersType) {
        peers = wrapped[1];
        return;
    }
    if (wrapped.size() < 5 || std::memcmp(wrapped.data(), "SHAD", 4) != 0) {
        return;
    }
    if (wrapped[4] != EnvelopeType || wrapped.size() < EnvelopeSize + 5) {
        if (!logged_old_sender.exchange(true)) {
            Log("a player on this server runs an older BBSC (packets without a sender) - their "
                "packets are ignored until they update");
        }
        return;
    }
    u32 sender = 0;
    u32 session = 0;
    std::memcpy(&sender, wrapped.data() + 5, 4);
    std::memcpy(&session, wrapped.data() + 9, 4);
    const std::vector<u8> frame(wrapped.begin() + EnvelopeSize, wrapped.end());
    if (sender == own_id || std::memcmp(frame.data(), "SHAD", 4) != 0) {
        return;
    }
    const s32 state = Core::Bloodborne::GetBbscRelaySessionState();
    const bool guest = state == 4 || state == 6;
    const bool sender_hosting = session != 0 && session == sender;
    // Host-only packets are taken from our host alone.
    const auto from_our_host = [&] {
        return guest && sender_hosting && sender == Core::Bloodborne::GetBbscRelayHost();
    };
    if (frame[4] == FollowType && frame.size() >= 0x1E) {
        FollowPacket follow{};
        std::memcpy(&follow, frame.data(), std::min(frame.size(), sizeof(follow)));
        if (sender_hosting) {
            Core::Bloodborne::NoteBbscRelayHost(sender, follow.packed_map, follow.x, follow.y,
                                                follow.z);
        }
        if (from_our_host()) {
            Core::Bloodborne::OnBbscHostFollowPacket(
                follow.packed_map, follow.x, follow.y, follow.z, follow.heading, follow.sequence,
                frame.size() >= sizeof(follow) ? follow.lamp_point : -1);
        }
    } else if (frame[4] == LifeType && frame.size() >= 14) {
        // Only the players of our own session (same host).
        if (session == 0 || session != SessionKey()) {
            return;
        }
        u32 player = 0;
        u32 sequence = 0;
        std::memcpy(&player, frame.data() + 5, 4);
        std::memcpy(&sequence, frame.data() + 10, 4);
        Core::Bloodborne::OnBbscLifePacket(player, frame[9], sequence);
    } else if (frame[4] == ScaleType && frame.size() >= 15) {
        if (from_our_host()) {
            std::uint16_t percent[5]{};
            std::memcpy(percent, frame.data() + 5, sizeof(percent));
            Core::Bloodborne::OnBbscScalePacket(percent);
        }
    } else if (frame[4] == PvpType && frame.size() >= 11) {
        // Kind 0 is the host's state (for its guests), kind 1 a guest asking its host.
        const bool ours = frame[5] == 0 ? from_our_host() : state == 3 && session == own_id;
        if (ours) {
            Core::Bloodborne::OnBbscPvpPacket(frame[5], frame[6]);
        }
    } else if (frame[4] == ProgressType && frame.size() >= 6) {
        // The host's boss flags for shared progress: u8 count + count x u32.
        const size_t count = std::min<size_t>(frame[5], (frame.size() - 6) / 4);
        if (from_our_host() && count != 0) {
            std::vector<std::uint32_t> flags(count);
            std::memcpy(flags.data(), frame.data() + 6, count * 4);
            Core::Bloodborne::OnBbscProgressPacket(flags.data(), flags.size());
        }
    } else if (frame[4] == HostBossType && frame.size() >= 9) {
        if (from_our_host()) {
            std::int32_t boss = -1;
            std::memcpy(&boss, frame.data() + 5, 4);
            Core::Bloodborne::OnBbscHostBossPacket(boss);
        }
    } else if (frame[4] == StainType && frame.size() >= 10) {
        if (from_our_host()) {
            std::uint32_t stain = 0;
            std::memcpy(&stain, frame.data() + 6, 4);
            Core::Bloodborne::OnBbscStainPacket(frame[5], stain);
        }
    } else if (frame[4] == PlacementType) {
        // A host looking for signs: for players not in a session yet, any host on the server.
        BbscMod::OnHostPlacement(std::string(frame.begin() + 5, frame.end()), sender);
    } else if (frame[4] == AimedPlacementType) {
        const std::string text(frame.begin() + 5, frame.end());
        const size_t bar = text.find('|');
        if (bar != std::string::npos && bar != 0) {
            BbscMod::OnAimedHostPlacement(std::atoll(text.substr(0, bar).c_str()),
                                          text.substr(bar + 1), sender);
        }
    } else if (frame[4] == Libraries::Np::NpSignaling::kSignalingBbscWorldFlagsType) {
        if (from_our_host()) {
            Core::Bloodborne::OnBbscWorldFlagsPacket(frame.data(), static_cast<u32>(frame.size()));
        }
    }
}

DWORD WINAPI RelayThread(void*) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::string last_problem;
    for (;;) {
        std::string host;
        u16 port = 0;
        if (!FindServer(host, port)) {
            if (last_problem != "no server") {
                last_problem = "no server";
                Log("no game server address (host_overrides.json / BBSC_RELAY) - retrying");
            }
            Sleep(10'000);
            continue;
        }
        if (BbscMod::IsPublicServerAddress(host)) {
            // The Hunter's Dream / NoDream run no BBSC relay: BBSC packets take the P2P channel.
            if (last_problem != "public") {
                last_problem = "public";
                Log("public game server (" + host + ") - no relay there; BBSC packets go over "
                    "the P2P channel");
            }
            Sleep(30'000);
            continue;
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        SOCKET s = INVALID_SOCKET;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) == 0) {
            s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
            if (s != INVALID_SOCKET && connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
                closesocket(s);
                s = INVALID_SOCKET;
            }
            freeaddrinfo(found);
        }
        if (s == INVALID_SOCKET) {
            const std::string problem = "cannot reach " + host + ":" + std::to_string(port);
            if (problem != last_problem) {
                last_problem = problem;
                Log(problem + " (game server not started, or an older one without the relay) - "
                              "retrying every 5 s");
            }
            Sleep(5'000);
            continue;
        }
        BOOL on = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
        // Only the writer thread sends now (2026-10-13), so a slow server no longer stalls the
        // game; a send stuck 10 s means the link is dead.
        const DWORD send_timeout_ms = 10'000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout_ms),
                   sizeof(send_timeout_ms));
        {
            std::scoped_lock lock{send_mutex};
            relay = s;
            lost_reason.clear();
            dropped_frames = 0;
        }
        last_problem.clear();
        Log("connected to " + host + ":" + std::to_string(port));
        const u64 connected_at = GetTickCount64();
        u64 frames_in = 0;
        std::string reason;
        for (;;) {
            u32 size = 0;
            if (!ReadAll(s, &size, 4)) {
                break;
            }
            if (size > 64 * 1024) {
                reason = "oversized frame from the server (" + std::to_string(size) + " bytes)";
                break;
            }
            std::vector<u8> frame(size);
            if (size != 0 && !ReadAll(s, frame.data(), static_cast<int>(size))) {
                break;
            }
            ++frames_in;
            std::scoped_lock dispatch_lock{dispatch_mutex};
            Dispatch(frame);
        }
        u32 dropped = 0;
        {
            std::unique_lock lock{send_mutex};
            if (!lost_reason.empty()) {
                reason = lost_reason; // the writer gave up first
            }
            relay = INVALID_SOCKET;
            shutdown(s, SD_BOTH); // wakes a writer still in send() on it
            send_cv.wait(lock, [s] { return writing != s; });
            closesocket(s);
            send_queue.clear();
            send_queue_bytes = 0;
            dropped = dropped_frames;
        }
        if (reason.empty()) {
            reason = last_read_result == 0
                         ? std::string("the server closed the connection")
                         : "read failed (WSA error " + std::to_string(-last_read_result) + ")";
        }
        peers = 0;
        Log("connection lost after " + std::to_string((GetTickCount64() - connected_at) / 1000) +
            " s, " + std::to_string(frames_in) + " frames in, " + std::to_string(dropped) +
            " dropped out: " + reason + " - reconnecting");
        Sleep(2'000);
    }
}

} // namespace

namespace BbscMod {
// Public game server (The Hunter's Dream...): it works like Sony's, so BBSC must summon the vanilla
// way (bbsc_mod.cpp Worker). No host_overrides.json found = not public (review 2026-10-11): a
// self-host install whose file BBSC does not look for must keep the seamless summons, and without
// the file the game reaches no server at all anyway.
bool GameServerIsPublic(std::string* host_out) {
    std::string host;
    u16 port = 0;
    if (!FindServer(host, port)) {
        if (host_out != nullptr) {
            *host_out = "none found";
        }
        return false;
    }
    if (host_out != nullptr) {
        *host_out = host;
    }
    return BbscMod::IsPublicServerAddress(host);
}

// Frames the P2P channel received (same format as the relay's, without the length).
void RelayDispatch(const std::vector<u8>& wrapped) {
    std::scoped_lock lock{dispatch_mutex};
    Dispatch(wrapped);
}

void RelaySendAimedPlacement(s64 target_user, const std::string& placement) {
    std::string frame = "SHAD";
    frame.push_back(static_cast<char>(AimedPlacementType));
    frame += std::to_string(target_user) + "|" + placement;
    SendFrame(frame.data(), static_cast<u32>(frame.size()));
}

void StartRelay() {
    if (!started.exchange(true)) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        own_id = static_cast<u32>((static_cast<u64>(counter.QuadPart) ^ GetCurrentProcessId()) *
                                  2654435761ull) |
                 1u;
        if (HANDLE thread = CreateThread(nullptr, 0, RelayThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
        if (HANDLE thread = CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
}
} // namespace BbscMod

namespace Libraries::Np::NpSignaling::Helpers {

u32 SendBbscFollowToPeers(u32 packed_map, float x, float y, float z, float heading, u32 sequence,
                          s32 lamp_point) {
    FollowPacket packet{};
    packet.packed_map = packed_map;
    packet.x = x;
    packet.y = y;
    packet.z = z;
    packet.heading = heading;
    packet.sequence = sequence;
    packet.lamp_point = lamp_point;
    return SendFrame(&packet, sizeof(packet));
}

u32 SendBbscWorldFlagsToPeers(const void* packet, u32 size) {
    return SendFrame(packet, size);
}

} // namespace Libraries::Np::NpSignaling::Helpers
