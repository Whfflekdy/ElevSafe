#include "elevsafe/backend_pipeline.h"

namespace elevsafe
{
    BackendPipeline::BackendPipeline(std::size_t frameBufferCapacity) :
        m_frameBuffer(frameBufferCapacity)
    {
    }

    void BackendPipeline::onFrame(const RadarFrame& frame)
    {
        m_frameBuffer.push(frame);
    }

    std::size_t BackendPipeline::drain(const FrameConsumer& consumer)
    {
        if (!consumer)
            return 0;

        std::size_t drained = 0;
        while (true)
        {
            auto frame = m_frameBuffer.pop();
            if (!frame.has_value())
                break;

            consumer(*frame);
            ++drained;
        }

        return drained;
    }

    std::size_t BackendPipeline::pendingSize() const noexcept
    {
        return m_frameBuffer.size();
    }
}
