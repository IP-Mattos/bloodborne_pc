// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: the BBSC (Bloodborne Seamless Co-op) game server's UDP relay, ported from BBSC's
// bbport-1.7.2-bbsc.patch to this transport. Used instead of the SuperMedo relay when the BBSC
// mod is loaded (or SHADPS4_P2P_RELAY=bbsc). The server listens on UDP 3478 of the shadNet
// server's address (SHADPS4_P2P_RELAY_PORT changes the port; SHADPS4_P2P_RELAY=0 turns it off):
//   "BBR1" 0x01 id[16]                 register, every 5 s (2 s until answered)
//   "BBR1" 0x02                        registered
//   "BBR1" 0x03 dst[16] src[16] ep[6]  frame for dst            (ep = our public address + port)
//   "BBR1" 0x04 src[16] ep[6] frame    frame from src
//   "BBR1" 0x05 dst[16]                dst is not on the relay (direct only for 10 s)
// Every known peer goes through the relay from its first packet; a direct copy goes out too
// until the peer answers directly or the relay has carried its traffic for 10 s.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::Net::BbscRelay {

// Decided once from the environment and the loaded modules.
bool Enabled();

// The shadNet server's address (network order), 0 = none. The relay port comes from the env.
void SetServer(u32 addr_nbo);
void SetLocalId(std::string_view online_id);
void SetSelfEndpoint(u32 addr_nbo, u16 port_nbo);
void RememberPeer(std::string_view online_id, u32 addr_nbo, u16 port_nbo);

// True while a server is set (the transport then wakes up at least every second).
bool Active();
u32 ServerAddr(); // network order
u16 ServerPort(); // network order

struct Outgoing {
    std::vector<u8> relay_frame; // empty: the packet is not relayed
    bool send_direct = true;
};
Outgoing Route(u32 dest_addr_nbo, u16 dest_port_nbo, const u8* packet, size_t size);

// A datagram from the relay server. Returns true when it was one of the relay's (and consumed);
// deliver is set when it carries a peer's packet, from the endpoint signaling knows the peer by.
bool Receive(const u8* data, size_t size, bool* deliver, u32* from_addr, u16* from_port,
             size_t* payload_offset);
void NoteDirectReceive(u32 addr_nbo, u16 port_nbo);

// Registration packets that are due now.
std::vector<std::vector<u8>> KeepAlive();

} // namespace Core::Net::BbscRelay
