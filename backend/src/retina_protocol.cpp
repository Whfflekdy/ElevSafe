#include "elevsafe/retina_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace elevsafe
{
    namespace
    {
        bool hasBytes(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count)
        {
            return offset <= bytes.size() && count <= bytes.size() - offset;
        }

        bool readU32LE(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint32_t& value)
        {
            if (!hasBytes(bytes, offset, sizeof(std::uint32_t)))
                return false;

            value = static_cast<std::uint32_t>(bytes[offset])
                | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
            offset += sizeof(std::uint32_t);
            return true;
        }

        bool readU64LE(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value)
        {
            if (!hasBytes(bytes, offset, sizeof(std::uint64_t)))
                return false;

            value = static_cast<std::uint64_t>(bytes[offset])
                | (static_cast<std::uint64_t>(bytes[offset + 1]) << 8)
                | (static_cast<std::uint64_t>(bytes[offset + 2]) << 16)
                | (static_cast<std::uint64_t>(bytes[offset + 3]) << 24)
                | (static_cast<std::uint64_t>(bytes[offset + 4]) << 32)
                | (static_cast<std::uint64_t>(bytes[offset + 5]) << 40)
                | (static_cast<std::uint64_t>(bytes[offset + 6]) << 48)
                | (static_cast<std::uint64_t>(bytes[offset + 7]) << 56);
            offset += sizeof(std::uint64_t);
            return true;
        }

        bool readFloatLE(std::span<const std::uint8_t> bytes, std::size_t& offset, float& value)
        {
            std::uint32_t bits = 0;
            if (!readU32LE(bytes, offset, bits))
                return false;

            static_assert(sizeof(float) == sizeof(bits));
            std::memcpy(&value, &bits, sizeof(value));
            return true;
        }

        bool readI32LE(std::span<const std::uint8_t> bytes, std::size_t& offset, std::int32_t& value)
        {
            std::uint32_t raw = 0;
            if (!readU32LE(bytes, offset, raw))
                return false;

            std::memcpy(&value, &raw, sizeof(value));
            return true;
        }

        bool isPacketMagicAt(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            if (!hasBytes(bytes, offset, sizeof(std::uint32_t)))
                return false;

            return bytes[offset] == 0x21
                && bytes[offset + 1] == 0x43
                && bytes[offset + 2] == 0xcd
                && bytes[offset + 3] == 0xab;
        }

    }

    RetinaStreamParser::RetinaStreamParser(FrameCallback callback) :
        m_callback(std::move(callback))
    {
        m_streamBuffer.reserve(8192);
    }

    void RetinaStreamParser::setFrameCallback(FrameCallback callback)
    {
        m_callback = std::move(callback);
    }

    void RetinaStreamParser::feed(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
            return;

        if (bytes.size() > kMaxBufferedBytes - std::min(m_streamBuffer.size(), kMaxBufferedBytes))
        {
            m_streamBuffer.clear();
            ++m_statistics.packetsRejected;
            retainMagicTail();
            return;
        }

        m_streamBuffer.insert(m_streamBuffer.end(), bytes.begin(), bytes.end());

        while (extractOnePacket())
        {
        }
    }

    void RetinaStreamParser::reset()
    {
        m_streamBuffer.clear();
        m_statistics = {};
    }

    std::size_t RetinaStreamParser::bufferedBytes() const
    {
        return m_streamBuffer.size();
    }

    const RetinaStreamParser::Statistics& RetinaStreamParser::statistics() const
    {
        return m_statistics;
    }

    bool RetinaStreamParser::extractOnePacket()
    {
        std::size_t packetStart = std::numeric_limits<std::size_t>::max();

        for (std::size_t magicOffset = 0; magicOffset + sizeof(std::uint32_t) <= m_streamBuffer.size(); ++magicOffset)
        {
            if (magicOffset >= 4 && isPacketMagicAt(m_streamBuffer, magicOffset))
            {
                packetStart = magicOffset - 4;
                break;
            }
        }

        if (packetStart == std::numeric_limits<std::size_t>::max())
        {
            retainMagicTail();
            return false;
        }

        if (packetStart > 0)
            m_streamBuffer.erase(m_streamBuffer.begin(), m_streamBuffer.begin() + static_cast<std::ptrdiff_t>(packetStart));

        if (m_streamBuffer.size() < kNetworkPacketHeaderSize)
            return false;

        if (!isPacketMagicAt(m_streamBuffer, 4))
        {
            m_streamBuffer.erase(m_streamBuffer.begin());
            ++m_statistics.packetsRejected;
            return true;
        }

        std::size_t headerOffset = 16;
        std::uint32_t packageSize = 0;
        if (!readU32LE(m_streamBuffer, headerOffset, packageSize))
            return false;

        if (packageSize > kMaxPacketPayloadSize)
        {
            m_streamBuffer.erase(m_streamBuffer.begin());
            ++m_statistics.packetsRejected;
            return true;
        }

        const std::size_t packetSize = kNetworkPacketHeaderSize + static_cast<std::size_t>(packageSize);
        if (m_streamBuffer.size() < packetSize)
            return false;

        RadarFrame frame;
        const std::span<const std::uint8_t> packet(m_streamBuffer.data(), packetSize);
        const bool parsed = parsePacket(packet, frame);

        m_streamBuffer.erase(
            m_streamBuffer.begin(),
            m_streamBuffer.begin() + static_cast<std::ptrdiff_t>(packetSize));

        if (!parsed)
        {
            ++m_statistics.packetsRejected;
            return true;
        }

        ++m_statistics.packetsParsed;
        if (m_callback)
            m_callback(frame);

        return true;
    }

    bool RetinaStreamParser::parsePacket(std::span<const std::uint8_t> packet, RadarFrame& out_frame) const
    {
        if (packet.size() < kNetworkPacketHeaderSize + kRadarFrameHeaderSize)
            return false;

        if (!isPacketMagicAt(packet, 4))
            return false;

        std::size_t headerOffset = 16;
        std::uint32_t packageSize = 0;
        if (!readU32LE(packet, headerOffset, packageSize))
            return false;

        if (packageSize > kMaxPacketPayloadSize
            || packet.size() != kNetworkPacketHeaderSize + static_cast<std::size_t>(packageSize))
            return false;

        std::size_t frameOffset = kNetworkPacketHeaderSize;
        std::uint64_t frameMagic = 0;
        std::uint32_t frameCount = 0;
        std::uint32_t pointCount = 0;

        if (!readU64LE(packet, frameOffset, frameMagic)
            || !readU32LE(packet, frameOffset, frameCount)
            || !readU32LE(packet, frameOffset, pointCount))
            return false;

        if (frameMagic != kFrameMagic || pointCount > kMaxPointCount)
            return false;

        const std::size_t pointBytes = static_cast<std::size_t>(pointCount) * kPointWireSize;
        const std::size_t targetIdBytes = static_cast<std::size_t>(pointCount) * kTargetIdWireSize;
        const std::size_t pointSectionEnd = kNetworkPacketHeaderSize + kRadarFrameHeaderSize + pointBytes + targetIdBytes;

        if (pointSectionEnd > packet.size())
            return false;

        out_frame.frameCount = frameCount;
        out_frame.points.clear();
        out_frame.points.reserve(pointCount);

        std::size_t pointOffset = kNetworkPacketHeaderSize + kRadarFrameHeaderSize;
        for (std::uint32_t index = 0; index < pointCount; ++index)
        {
            RadarPoint point;
            if (!readFloatLE(packet, pointOffset, point.x)
                || !readFloatLE(packet, pointOffset, point.y)
                || !readFloatLE(packet, pointOffset, point.z)
                || !readFloatLE(packet, pointOffset, point.doppler)
                || !readFloatLE(packet, pointOffset, point.power))
                return false;

            out_frame.points.push_back(point);
        }

        std::size_t targetIdOffset = kNetworkPacketHeaderSize + kRadarFrameHeaderSize + pointBytes;
        for (std::uint32_t index = 0; index < pointCount; ++index)
        {
            if (!readI32LE(packet, targetIdOffset, out_frame.points[index].targetId))
                return false;
        }

        // The remaining payload is intentionally left uninterpreted until the target wire layout is confirmed.
        return true;
    }

    void RetinaStreamParser::retainMagicTail()
    {
        constexpr std::size_t bytesBeforeMagic = sizeof(std::uint32_t);
        constexpr std::size_t magicSize = sizeof(std::uint32_t);
        constexpr std::size_t bytesToKeep = bytesBeforeMagic + magicSize - 1;
        if (m_streamBuffer.size() > bytesToKeep)
        {
            m_streamBuffer.erase(
                m_streamBuffer.begin(),
                m_streamBuffer.end() - static_cast<std::ptrdiff_t>(bytesToKeep));
        }
    }
}
