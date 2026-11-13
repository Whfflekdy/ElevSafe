cmake_minimum_required(VERSION 3.16)
project(ElevSafeTracking LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Eigen3 REQUIRED NO_MODULE)

# 폴더 안의 .cpp를 자동으로 모으기
file(GLOB CORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/id_manager/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detection/*.cpp
)
# 테스트 파일은 라이브러리에서 제외
list(FILTER CORE_SRCS EXCLUDE REGEX "_test\\.cpp$")

add_library(tracking_core STATIC
    ${CORE_SRCS}
    third_party/kalman-cpp/kalman.cpp
)


target_include_directories(tracking_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/id_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/detection
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kalman-cpp
)

target_link_libraries(tracking_core PUBLIC Eigen3::Eigen)

# 테스트 파일도 자동으로 모으기
enable_testing()
file(GLOB TEST_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/id_manager/*_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detection/*_test.cpp
)

foreach(src ${TEST_SRCS})
    get_filename_component(t ${src} NAME_WE)
    add_executable(${t} ${src})
    target_link_libraries(${t} PRIVATE tracking_core)
    if(t STREQUAL "detection_loader_test")
    add_test(NAME ${t} COMMAND ${t} ${CMAKE_CURRENT_SOURCE_DIR}/S0-1_t01.csv)
    else()
    add_test(NAME ${t} COMMAND ${t})
    endif()
endforeach()

add_executable(tracking_offline offline_runner.cpp)
target_link_libraries(tracking_offline PRIVATE tracking_core)