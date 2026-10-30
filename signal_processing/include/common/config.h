// config.h
// "key = value" 형식 설정 파일 로더 
#pragma once

#include <string>
#include <unordered_map>

namespace elevsafe::sp {


// ROI 값과 센서 자세를 엘리베이터가 바뀔 때마다 재컴파일하지 않도록
// 설정 파일로 분리하여 읽어들이는 클래스. (elevator_default.cfg)

class Config {
public:
    bool load(const std::string& path);   // 성공 시 true

    bool has(const std::string& key) const;
    float getFloat(const std::string& key, float default_value) const;
    std::string getString(const std::string& key, const std::string& default_value) const;
    
    // 경로 값 읽기: 상대 경로면 "이 설정 파일이 있는 폴더" 기준으로 바꿔서 돌려준다.
    // 절대 경로는 그대로. 값이 없거나 비어 있으면 default_value 그대로 (바꾸지 않음).
    std::string getPath(const std::string& key, const std::string& default_value) const;

private:
    std::unordered_map<std::string, std::string> values_;   // key -> value 문자열
    std::string base_dir_;   // 설정 파일이 있는 폴더 (load 때 기억, 끝에 '/' 없음)
};

}  // namespace elevsafe::sp
