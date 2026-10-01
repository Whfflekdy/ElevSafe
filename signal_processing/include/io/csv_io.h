// csv_io.h
// 녹화 데이터 입출력
// 녹화된 데이터 파일을 읽고 처리 결과를 파일로 저장하는 기능
// 실측 데이터로 ROI 값을 조정할 때나 시각화 도구로 결과를 볼 때 필요. 
//   입력 헤더: frame,x,y,z,doppler,power,target_id
//   출력 헤더: frame,x,y,z,doppler,power,target_id,zone
#pragma once

#include <string>
#include <vector>
#include "common/radar_point.h"

// frame 번호 순으로 정렬된 프레임 목록 반환 (실패 시 빈 벡터)
std::vector<Frame> readFramesCSV(const std::string& path);

// 프레임 목록을 CSV로 저장 (성공 시 true)
bool writeFramesCSV(const std::string& path, const std::vector<Frame>& frames);