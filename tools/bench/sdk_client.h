/*---------------------------------------------------------*\
|| sdk_client.h                                              ||
||                                                           ||
||   Minimal synchronous OpenRGB SDK client over loopback   ||
||   TCP for the Milestone 1 benchmark producer. Protocol  ||
||   version is negotiated down to the cap in sdk_wire.h.   ||
||   A drain thread consumes asynchronous server packets   ||
||   so the socket never backs up.                          ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "sdk_wire.h"

#include <cstdint>
#include <string>
#include <vector>
namespace openrgb_bench
{
    struct ClientStats
    {
        uint64_t packets_received = 0;   /* drained async packets     */
        uint64_t bytes_received   = 0;
        uint64_t send_errors     = 0;
    };

    class SdkClient
    {
    public:
        SdkClient();
        ~SdkClient();

        SdkClient(const SdkClient&) = delete;
        SdkClient& operator=(const SdkClient&) = delete;

        /* throws std::runtime_error on failure */
        void Connect(const std::string& host, uint16_t port,
                     const std::string& client_name,
                     uint32_t timeout_ms = 4000);

        void Disconnect();

        uint32_t      ProtocolVersion() const { return protocol_version; }
        ClientStats   Stats() const;

        uint32_t      ControllerCount();
        /* throws std::runtime_error on transport/parse failure */
        SdkController ControllerData(uint32_t dev_id);

        void SetCustomMode(uint32_t dev_id);
        void UpdateMode(uint32_t dev_id, int32_t mode_index,
                        const std::vector<uint8_t>& raw_mode);
        void UpdateLEDs(uint32_t dev_id, const std::vector<uint32_t>& colors);

    private:
        void SendPacket(uint32_t dev_id, uint32_t pkt_id,
                        const std::vector<uint8_t>& body);
        /* reads one full packet; returns false on closed socket/timeout */
        bool RecvPacket(PacketHeader& header, std::vector<uint8_t>& body);
        /* request/reply matching: one outstanding synchronous request;
           unexpected async packets are counted and dropped */
        std::vector<uint8_t> Request(uint32_t dev_id, uint32_t pkt_id,
                                    const std::vector<uint8_t>& body,
                                    uint32_t expected_reply_id);
        /* non-blocking drain of unsolicited packets */
        void DrainPending();

        /* raw OS socket handle (SOCKET); uintptr_t keeps winsock out of
           the header on the bench-producer side */
        uintptr_t    sock;
        uint32_t      protocol_version;
        ClientStats   stats;
    };
}
