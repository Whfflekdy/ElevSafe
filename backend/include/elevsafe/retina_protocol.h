#pragma once

#include "radar_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace elevsafe
{
    inline constexpr std::size_t kNetworkPacketHeaderSize = 36;
    inline constexpr std::size_t kRadarFrameHeaderSize = 16;
    inline constexpr std::size_t kPointWireSize = 5 * sizeof(float);
    inline constexpr std::size_t kTargetIdWireSize = sizeof(std::int32_t);

    inline constexpr std::uint32_t kPacketMagic = 0xABCD4321u;
    inline constexpr std::uint64_t kFrameMagic = 0x0807060504030201ull;

    inline constexpr std::uint32_t kMaxPointCount = 5000;
    inline constexpr std::size_t kMaxPacketPayloadSize = 16u * 1024u * 1024u;
    inline constexpr std::size_t kMaxBufferedBytes = kNetworkPacketHeaderSize + kMaxPacketPayloadSize;

    class RetinaStreamParser
    {
    public:
        using FrameCallback = std::function<void(RadarFrame&)>;

        struct Statistics
        {
            std::size_t packetsParsed = 0;
            std::size_t packetsRejected = 0;
        };

        explicit RetinaStreamParser(FrameCallback callback = {});

        void setFrameCallback(FrameCallback callback);
        void feed(std::span<const std::uint8_t> bytes);
        void reset();

        std::size_t bufferedBytes() const;
        const Statistics& statistics() const;

    private:
        bool extractOnePacket();
        bool parsePacket(std::span<const std::uint8_t> packet, RadarFrame& out_frame) const;
        void retainMagicTail();

        std::vector<std::uint8_t> m_streamBuffer;
        FrameCallback m_callback;
        Statistics m_statistics;
    };
}
