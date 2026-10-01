cmake_minimum_required(VERSION 3.20)

project(elevsafe_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(elevsafe_backend_core
    src/retina_protocol.cpp
    src/retina_client.cpp
)

target_include_directories(elevsafe_backend_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(elevsafe_backend_core PRIVATE ws2_32)
endif()

add_executable(elevsafe_backend
    src/main.cpp
)

target_link_libraries(elevsafe_backend PRIVATE elevsafe_backend_core)

enable_testing()

add_executable(retina_protocol_test
    tests/retina_protocol_test.cpp
)

target_link_libraries(retina_protocol_test PRIVATE elevsafe_backend_core)

add_test(NAME retina_protocol_test COMMAND retina_protocol_test)
