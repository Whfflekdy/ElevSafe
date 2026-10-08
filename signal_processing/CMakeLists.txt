cmake_minimum_required(VERSION 3.14)
project(signal_processing LANGUAGES CXX)

# C++17 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 경고를 많이 켜서 실수를 컴파일러가 잡게 함
if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()
# 1) 파이프라인 핵심 로직을 라이브러리 하나로 묶음
#    (Step이 추가될 때마다 여기에 .cpp를 한 줄씩 추가)
add_library(radar_core
  src/common/config.cpp
  src/preprocessing/coordinate_transform.cpp
  src/preprocessing/roi_filter.cpp
  src/io/csv_io.cpp
  src/preprocessing/polar_grid.cpp
  src/preprocessing/background_subtractor.cpp
)
# radar_core를 쓰는 쪽은 include/ 를 기준으로 헤더를 찾음
target_include_directories(radar_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 1) 정식 실행 파일 radar_pipeline을 생성하고 로직 묶음인 radar_core 연결
add_executable(radar_pipeline src/main.cpp)
target_link_libraries(radar_pipeline PRIVATE radar_core)

add_executable(build_background src/build_background.cpp)
target_link_libraries(build_background PRIVATE radar_core)

# 2) 확인용 scratch 실행 파일
add_executable(scratch tests/scratch.cpp)
target_link_libraries(scratch PRIVATE radar_core)