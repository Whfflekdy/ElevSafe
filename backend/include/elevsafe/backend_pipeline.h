#pragma once

#include "frame_buffer.h"

#include <cstddef>
#include <functional>

namespace elevsafe
{
    class BackendPipeline
    {
    public:
        using FrameConsumer = std::function<void(const RadarFrame&)>;

        explicit BackendPipeline(std::size_t frameBufferCapacity);

        void onFrame(const RadarFrame& frame);
        std::size_t drain(const FrameConsumer& consumer);

        std::size_t pendingSize() const noexcept;

    private:
        FrameBuffer m_frameBuffer;
    };
}
