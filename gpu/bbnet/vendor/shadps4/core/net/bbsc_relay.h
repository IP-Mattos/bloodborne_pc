// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: BBSC game server's UDP relay, from bbport-1.7.2-bbsc.patch. UDP 3478 on the shadNet
// server's address. SHADPS4_P2P_RELAY=bbsc|0 forces on/off, SHADPS4_P2P_RELAY_PORT overrides.
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

// Network order, 0 = none. The relay port comes from the env.
void SetServer(u32 addr_nbo);
void SetLocalId(std::string_view online_id);
void SetSelfEndpoint(u32 addr_nbo, u16 port_nbo);
void RememberPeer(std::string_view online_id, u32 addr_nbo, u16 port_nbo);

// While true the transport wakes at least every second.
bool Active();
u32 ServerAddr(); // network order
u16 ServerPort(); // network order

struct Outgoing {
    std::vector<u8> relay_frame; // empty: the packet is not relayed
    bool send_direct = true;
};
Outgoing Route(u32 dest_addr_nbo, u16 dest_port_nbo, const u8* packet, size_t size);

// True when consumed as a relay frame; deliver is set when it carries a peer's packet, from the
// endpoint signaling knows the peer by.
bool Receive(const u8* data, size_t size, bool* deliver, u32* from_addr, u16* from_port,
             size_t* payload_offset);
void NoteDirectReceive(u32 addr_nbo, u16 port_nbo);

// Registration packets that are due now.
std::vector<std::vector<u8>> KeepAlive();

} // namespace Core::Net::BbscRelay
