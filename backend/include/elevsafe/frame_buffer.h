#pragma once

#include "radar_types.h"

#include <cstddef>
#include <deque>
#include <optional>

namespace elevsafe
{
    class FrameBuffer
    {
    public:
        explicit FrameBuffer(std::size_t capacity);

        void push(RadarFrame frame);
        std::optional<RadarFrame> pop();

        bool empty() const noexcept;
        std::size_t size() const noexcept;
        std::size_t capacity() const noexcept;

        void clear() noexcept;

    private:
        std::size_t m_capacity;
        std::deque<RadarFrame> m_frames;
    };
}
