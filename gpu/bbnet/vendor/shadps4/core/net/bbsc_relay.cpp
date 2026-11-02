// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

#include "common/logging/log.h"
#include "core/net/bbsc_relay.h"

namespace Core::Net::BbscRelay {

namespace {

constexpr size_t kIdSize = 16;
constexpr size_t kEpSize = 6;
constexpr u8 kMagic[4] = {'B', 'B', 'R', '1'};
constexpr u16 kDefaultPort = 3478;

using Clock = std::chrono::steady_clock;

struct Peer {
    std::string id; // padded to 16
    u32 addr{};     // the endpoint signaling uses; relayed frames arrive "from" it
    u16 port{};
    bool direct_rx{false};
    bool relay_seen{false};
    Clock::time_point relayed_since{};
    Clock::time_point missing_until{};
    bool missing_logged{false};
};

std::mutex g_mutex;
u32 g_server_addr = 0;
u16 g_server_port = 0;
bool g_acked = false;
u32 g_self_addr = 0;
u16 g_self_port = 0;
Clock::time_point g_last_register{};
std::vector<std::string> g_local_ids;
std::unordered_map<std::string, Peer> g_peers; // by padded online id
std::unordered_map<u64, std::string> g_alias;  // endpoint -> online id
u32 g_next_pseudo = 1;

std::string PadId(std::string_view id) {
    std::string out(id.substr(0, kIdSize));
    out.resize(kIdSize, '\0');
    return out;
}

std::string IdText(const std::string& id) {
    return std::string(id.c_str());
}

u64 Key(u32 addr, u16 port) {
    return (static_cast<u64>(addr) << 16) | port;
}

// g_mutex held.
void AddAlias(const std::string& id, u32 addr, u16 port, const char* from) {
    if (addr == 0 || port == 0) {
        return;
    }
    const auto it = g_alias.find(Key(addr, port));
    if (it != g_alias.end() && it->second == id) {
        return;
    }
    g_alias[Key(addr, port)] = id;
    LOG_INFO(Lib_Net, "BBSC relay: '{}' known at {:#x}:{} (from {})", IdText(id), addr,
             ntohs(port), from);
}

// g_mutex held.
Peer* FindPeer(u32 addr, u16 port) {
    const auto alias = g_alias.find(Key(addr, port));
    if (alias == g_alias.end()) {
        return nullptr;
    }
    const auto it = g_peers.find(alias->second);
    return it == g_peers.end() ? nullptr : &it->second;
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_P2P_RELAY");
        const std::string mode = env != nullptr ? env : "";
        if (mode == "0" || mode == "off") {
            return false;
        }
        if (mode == "bbsc") {
            return true;
        }
#ifdef _WIN32
        // bbsc-launch.exe loads bbsc.dll into the process before the game's first instruction.
        return GetModuleHandleA("bbsc.dll") != nullptr;
#else
        return false;
#endif
    }();
    return enabled;
}

void SetServer(u32 addr_nbo) {
    std::lock_guard lock(g_mutex);
    u16 port = kDefaultPort;
    if (const char* p = std::getenv("SHADPS4_P2P_RELAY_PORT"); p != nullptr && *p != 0) {
        const long parsed = std::strtol(p, nullptr, 10);
        if (parsed > 0 && parsed < 65536) {
            port = static_cast<u16>(parsed);
        }
    }
    const u16 port_nbo = addr_nbo != 0 ? htons(port) : 0;
    if (g_server_addr == addr_nbo && g_server_port == port_nbo) {
        return;
    }
    g_server_addr = addr_nbo;
    g_server_port = port_nbo;
    g_acked = false;
    g_last_register = {};
    if (addr_nbo != 0) {
        LOG_INFO(Lib_Net, "BBSC relay: server {:#x}:{}", addr_nbo, port);
    } else {
        LOG_INFO(Lib_Net, "BBSC relay: off");
    }
}

void SetLocalId(std::string_view online_id) {
    if (online_id.empty()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    const std::string id = PadId(online_id);
    if (std::find(g_local_ids.begin(), g_local_ids.end(), id) == g_local_ids.end()) {
        g_local_ids.push_back(id);
        g_last_register = {};
    }
}

void SetSelfEndpoint(u32 addr_nbo, u16 port_nbo) {
    std::lock_guard lock(g_mutex);
    if (g_self_addr != addr_nbo || g_self_port != port_nbo) {
        LOG_INFO(Lib_Net, "BBSC relay: our public endpoint {:#x}:{}", addr_nbo, ntohs(port_nbo));
    }
    g_self_addr = addr_nbo;
    g_self_port = port_nbo;
}

void RememberPeer(std::string_view online_id, u32 addr_nbo, u16 port_nbo) {
    if (online_id.empty() || addr_nbo == 0 || port_nbo == 0) {
        return;
    }
    std::lock_guard lock(g_mutex);
    const std::string id = PadId(online_id);
    // Kept by online id; every endpoint signaling or the relay gave for it stays an alias.
    Peer& peer = g_peers[id];
    peer.id = id;
    peer.addr = addr_nbo;
    peer.port = port_nbo;
    AddAlias(id, addr_nbo, port_nbo, "signaling");
}

bool Active() {
    std::lock_guard lock(g_mutex);
    return g_server_addr != 0;
}

u32 ServerAddr() {
    std::lock_guard lock(g_mutex);
    return g_server_addr;
}

u16 ServerPort() {
    std::lock_guard lock(g_mutex);
    return g_server_port;
}

Outgoing Route(u32 dest_addr_nbo, u16 dest_port_nbo, const u8* packet, size_t size) {
    Outgoing out;
    std::lock_guard lock(g_mutex);
    if (g_server_addr == 0 || g_local_ids.empty() || dest_addr_nbo == g_server_addr) {
        return out;
    }
    Peer* const peer = FindPeer(dest_addr_nbo, dest_port_nbo);
    if (peer == nullptr) {
        return out;
    }
    const auto now = Clock::now();
    if (now < peer->missing_until) {
        return out; // the peer has no relay: direct only
    }
    if (peer->relayed_since.time_since_epoch().count() == 0) {
        peer->relayed_since = now;
        LOG_INFO(Lib_Net, "BBSC relay: '{}' at {:#x}:{} - through the relay", IdText(peer->id),
                 peer->addr, ntohs(peer->port));
    }
    const std::string& self = g_local_ids.front();
    out.relay_frame.reserve(5 + 2 * kIdSize + kEpSize + size);
    out.relay_frame.insert(out.relay_frame.end(), std::begin(kMagic), std::end(kMagic));
    out.relay_frame.push_back(0x03);
    out.relay_frame.insert(out.relay_frame.end(), peer->id.begin(), peer->id.end());
    out.relay_frame.insert(out.relay_frame.end(), self.begin(), self.end());
    u8 ep[kEpSize];
    std::memcpy(ep, &g_self_addr, 4);
    std::memcpy(ep + 4, &g_self_port, 2);
    out.relay_frame.insert(out.relay_frame.end(), ep, ep + kEpSize);
    out.relay_frame.insert(out.relay_frame.end(), packet, packet + size);
    out.send_direct = !peer->direct_rx &&
                      (!peer->relay_seen || now - peer->relayed_since < std::chrono::seconds(10));
    return out;
}

bool Receive(const u8* data, size_t size, bool* deliver, u32* from_addr, u16* from_port,
             size_t* payload_offset) {
    *deliver = false;
    if (size < 5 || std::memcmp(data, kMagic, 4) != 0) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (data[4] == 0x02) {
        if (!g_acked) {
            g_acked = true;
            LOG_INFO(Lib_Net, "BBSC relay: registered with the relay");
        }
        return true;
    }
    if (data[4] == 0x05 && size >= 5 + kIdSize) {
        const std::string id(reinterpret_cast<const char*>(data + 5), kIdSize);
        const auto known = g_peers.find(id);
        if (known != g_peers.end()) {
            Peer& peer = known->second;
            peer.missing_until = Clock::now() + std::chrono::seconds(10);
            if (!peer.missing_logged) {
                peer.missing_logged = true;
                LOG_WARNING(Lib_Net,
                            "BBSC relay: '{}' is not on the relay - direct only; if direct does "
                            "not work either, it cannot connect until it updates",
                            IdText(peer.id));
            }
        }
        return true;
    }
    if (data[4] != 0x04 || size < 5 + kIdSize + kEpSize) {
        return true;
    }
    const std::string id(reinterpret_cast<const char*>(data + 5), kIdSize);
    u32 ep_addr = 0;
    u16 ep_port = 0;
    std::memcpy(&ep_addr, data + 5 + kIdSize, 4);
    std::memcpy(&ep_port, data + 5 + kIdSize + 4, 2);
    if (!g_peers.contains(id)) {
        // A peer signaling never looked up: the endpoint it gave, else a made-up one
        // (169.254.x.x, never sent to directly) so the answers go back through the relay.
        Peer& made = g_peers[id];
        made.id = id;
        if (ep_addr != 0 && ep_port != 0) {
            made.addr = ep_addr;
            made.port = ep_port;
        } else {
            made.addr = htonl(0xA9FE0000u | (g_next_pseudo++ & 0xFFFFu));
            made.port = htons(3173);
        }
        AddAlias(id, made.addr, made.port, "relay, new peer");
    }
    AddAlias(id, ep_addr, ep_port, "relay");
    Peer& peer = g_peers[id];
    peer.relay_seen = true;
    peer.missing_until = {};
    if (peer.relayed_since.time_since_epoch().count() == 0) {
        peer.relayed_since = Clock::now();
        LOG_INFO(Lib_Net, "BBSC relay: '{}' reached us through the relay - answering the same way",
                 IdText(peer.id));
    }
    *deliver = true;
    *from_addr = peer.addr;
    *from_port = peer.port;
    *payload_offset = 5 + kIdSize + kEpSize;
    return true;
}

void NoteDirectReceive(u32 addr_nbo, u16 port_nbo) {
    std::lock_guard lock(g_mutex);
    if (g_peers.empty()) {
        return;
    }
    Peer* const peer = FindPeer(addr_nbo, port_nbo);
    if (peer != nullptr && !peer->direct_rx) {
        peer->direct_rx = true;
        LOG_INFO(Lib_Net, "BBSC relay: '{}' reached us directly", IdText(peer->id));
    }
}

std::vector<std::vector<u8>> KeepAlive() {
    std::vector<std::vector<u8>> frames;
    std::lock_guard lock(g_mutex);
    if (g_server_addr == 0 || g_local_ids.empty()) {
        return frames;
    }
    const auto now = Clock::now();
    const auto interval = g_acked ? std::chrono::seconds(5) : std::chrono::seconds(2);
    if (g_last_register.time_since_epoch().count() != 0 && now - g_last_register < interval) {
        return frames;
    }
    g_last_register = now;
    for (const std::string& id : g_local_ids) {
        std::vector<u8> packet(std::begin(kMagic), std::end(kMagic));
        packet.push_back(0x01);
        packet.insert(packet.end(), id.begin(), id.end());
        frames.push_back(std::move(packet));
    }
    return frames;
}

} // namespace Core::Net::BbscRelay
