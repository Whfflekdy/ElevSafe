#include "elevsafe/frame_buffer.h"

#include <stdexcept>
#include <utility>

namespace elevsafe
{
    FrameBuffer::FrameBuffer(std::size_t capacity) :
        m_capacity(capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("FrameBuffer capacity must be greater than zero");
    }

    void FrameBuffer::push(RadarFrame frame)
    {
        if (m_frames.size() >= m_capacity)
            m_frames.pop_front();

        m_frames.push_back(std::move(frame));
    }

    std::optional<RadarFrame> FrameBuffer::pop()
    {
        if (m_frames.empty())
            return std::nullopt;

        RadarFrame frame = std::move(m_frames.front());
        m_frames.pop_front();
        return frame;
    }

    bool FrameBuffer::empty() const noexcept
    {
        return m_frames.empty();
    }

    std::size_t FrameBuffer::size() const noexcept
    {
        return m_frames.size();
    }

    std::size_t FrameBuffer::capacity() const noexcept
    {
        return m_capacity;
    }

    void FrameBuffer::clear() noexcept
    {
        m_frames.clear();
    }
}
