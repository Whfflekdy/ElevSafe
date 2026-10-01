#include "common/config.h"

#include <fstream>
#include <iostream>

namespace { // 이 파일에서만 쓰는 함수여서 다른 파일에 trim과 같은 이름이 있어도 충돌하지 않도록
// 앞뒤 공백(space, tab, \r, \n) 제거
std::string trim(const std::string& s){
    const char* whitespace = " \t\r\n";
    const auto start = s.find_first_not_of(whitespace);
    if(start == std::string::npos) return ""; // 공백만 있는 경우
    const auto end = s.find_last_not_of(whitespace);
    return s.substr(start, end-start+1);
}
}

bool Config::load(const std::string& path){
    std::ifstream file(path);
    if(!file.is_open()){
        std::cerr<< "[Config] 파일을 열 수 없음: " << path << "\n";
        return false;
    }

    std::string line;
    while(std::getline(file, line)){
        // '#' 위치를 찾아 그 뒤를 잘라내기 
        const auto comment_pos = line.find('#');
        if(comment_pos != std::string::npos){
            line = line.substr(0, comment_pos);
        }

        // trim 후 빈 줄이면 continue
        line = trim(line);
        if(line.empty()) continue;

        // '=' 위치를 찾고, 없으면 경고 출력 후 continue
        const auto equal_pos = line.find('=');
        if(equal_pos == std::string::npos){
            std::cerr << "[Config] '-' 없는 줄 무시: " << line << "\n";
            continue;
        }
        // '=' 앞은 key, 뒤는 value, 각각 trim해서 values_에 저장
        const std::string key = trim(line.substr(0, equal_pos));
        const std::string value = trim(line.substr(equal_pos + 1));
        values_[key] = value;
    }
    return true;
}

bool Config::has(const std::string& key) const{
    // values_에 key가 있는지 확인
    return values_.find(key) != values_.end();
}

// key에 해당하는 값을 float로 변환하여 반환. 없으면 default_value 반환
float Config::getFloat(const std::string& key, float default_value) const {
    auto it = values_.find(key);
    if (it == values_.end()) return default_value;
    try {
        return std::stof(it->second);
    } catch (...) {
        std::cerr << "[Config] '" << key << "' 값을 숫자로 바꿀 수 없음: " << it->second << "\n";
        return default_value;
    }
}