// door_state.h
// 문 상태. A → B (FrameContext.door_state), B → A (PipelineResult.estimated_door_state) 양쪽에서 쓴다.
#pragma once

namespace elevsafe::sp {

enum class DoorState { UNKNOWN, OPEN, CLOSED };

const char* doorStateToString(DoorState s);

}  // namespace elevsafe::sp
