// config.h
// "key = value" 형식 설정 파일 로더 
#pragma once

#include <string>
#include <unordered_map>


// ROI 값과 센서 자세를 엘리베이터가 바뀔 때마다 재컴파일하지 않도록
// 설정 파일로 분리하여 읽어들이는 클래스. (elevator_default.cfg)

class Config {
public:
    bool load(const std::string& path);   // 성공 시 true

    bool has(const std::string& key) const;
    float getFloat(const std::string& key, float default_value) const;

private:
    std::unordered_map<std::string, std::string> values_;   // key -> value 문자열
};