cmake_minimum_required(VERSION 3.20)

project(elevsafe_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../signal_processing signal_processing)

add_library(elevsafe_backend_core
    src/retina_protocol.cpp
    src/retina_client.cpp
    src/frame_buffer.cpp
    src/backend_pipeline.cpp
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

add_library(elevsafe_signal_adapter src/signal_adapter.cpp)
target_link_libraries(elevsafe_signal_adapter PUBLIC elevsafe_backend_core radar_core)
target_compile_options(elevsafe_signal_adapter PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(elevsafe_backend PRIVATE elevsafe_signal_adapter)

enable_testing()

add_executable(retina_protocol_test
    tests/retina_protocol_test.cpp
)

target_link_libraries(retina_protocol_test PRIVATE elevsafe_backend_core)

add_test(NAME retina_protocol_test COMMAND retina_protocol_test)

add_executable(frame_buffer_test
    tests/frame_buffer_test.cpp
)

target_link_libraries(frame_buffer_test PRIVATE elevsafe_backend_core)

add_test(NAME frame_buffer_test COMMAND frame_buffer_test)

add_executable(backend_pipeline_test
    tests/backend_pipeline_test.cpp
)

target_link_libraries(backend_pipeline_test PRIVATE elevsafe_backend_core)

add_test(NAME backend_pipeline_test COMMAND backend_pipeline_test)

add_executable(relative_frame_clock_test
    tests/relative_frame_clock_test.cpp
)

target_link_libraries(relative_frame_clock_test PRIVATE elevsafe_backend_core)

add_test(NAME relative_frame_clock_test COMMAND relative_frame_clock_test)

add_executable(signal_adapter_test tests/signal_adapter_test.cpp)
target_link_libraries(signal_adapter_test PRIVATE elevsafe_signal_adapter)
add_test(NAME signal_adapter_test COMMAND signal_adapter_test)

add_executable(signal_integration_test tests/signal_integration_test.cpp)
target_link_libraries(signal_integration_test PRIVATE elevsafe_signal_adapter)
add_test(NAME signal_integration_test COMMAND signal_integration_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg
    ${CMAKE_CURRENT_SOURCE_DIR}/../signal_processing/data/sample/tiny.csv
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/invalid_signal.cfg)
