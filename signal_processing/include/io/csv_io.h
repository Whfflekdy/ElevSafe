// csv_io.h
// 녹화 데이터 입출력 (컬럼은 헤더 이름으로 찾는다)
//   입력 필수 컬럼: frame,x,y,z,doppler,power
//   입력 선택 컬럼: timestamp_ms, target_id (없으면 기본값)
//   출력 헤더    : frame,timestamp_ms,x,y,z,doppler,power,target_id,zone,cluster
#pragma once

#include <string>
#include <vector>
#include "common/radar_point.h"

// frame 번호 순으로 정렬된 프레임 목록 (실패 시 빈 벡터)
std::vector<FrameData> readFramesCSV(const std::string& path);

// 프레임 목록을 CSV로 저장 (성공 시 true)
bool writeFramesCSV(const std::string& path, const std::vector<FrameData>& frames);