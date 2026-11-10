cmake_minimum_required(VERSION 3.20)

project(elevsafe_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/WebSocketDependencies.cmake)
find_package(Threads REQUIRED)

add_library(elevsafe_websocket
    src/websocket_server.cpp
    src/websocket_message.cpp
)
target_include_directories(elevsafe_websocket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(elevsafe_websocket PRIVATE elevsafe_boost_headers nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(elevsafe_websocket PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
if(WIN32)
    target_compile_definitions(elevsafe_websocket PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(elevsafe_websocket PRIVATE ws2_32 mswsock)
endif()

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

target_link_libraries(elevsafe_backend PRIVATE elevsafe_signal_adapter elevsafe_websocket)

add_library(elevsafe_door_simulation src/door_simulation.cpp)
target_include_directories(elevsafe_door_simulation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(elevsafe_backend PRIVATE elevsafe_door_simulation nlohmann_json::nlohmann_json)

enable_testing()
add_executable(door_simulation_test tests/door_simulation_test.cpp)
target_link_libraries(door_simulation_test PRIVATE elevsafe_door_simulation elevsafe_websocket nlohmann_json::nlohmann_json)
add_test(NAME door_simulation_test COMMAND door_simulation_test)

add_executable(ab_profile_test tests/ab_profile_test.cpp)
target_link_libraries(ab_profile_test PRIVATE elevsafe_backend_core)
add_test(NAME ab_profile_test COMMAND ab_profile_test)

add_executable(retina_protocol_test
    tests/retina_protocol_test.cpp
)

target_link_libraries(retina_protocol_test PRIVATE elevsafe_backend_core)

add_test(NAME retina_protocol_test COMMAND retina_protocol_test)

add_executable(retina_reconnect_test tests/retina_reconnect_test.cpp)
target_link_libraries(retina_reconnect_test PRIVATE elevsafe_backend_core elevsafe_boost_headers Threads::Threads)
target_compile_definitions(retina_reconnect_test PRIVATE $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>)
add_test(NAME retina_reconnect_test COMMAND retina_reconnect_test)
set_tests_properties(retina_reconnect_test PROPERTIES TIMEOUT 10)
if(WIN32)
    add_test(NAME radar_reconnect_cli_test COMMAND "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
        -NoProfile -NonInteractive -Command
        "& ([scriptblock]::Create((Get-Content -LiteralPath '${CMAKE_CURRENT_SOURCE_DIR}/tests/radar_reconnect_fixture.ps1' -Raw -Encoding UTF8))) -BackendExe '$<TARGET_FILE:elevsafe_backend>' -SignalConfig '${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg'")
    set_tests_properties(radar_reconnect_cli_test PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

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
add_test(NAME ab_profile_replay_test COMMAND signal_integration_test --ab-profile
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg
    ${CMAKE_CURRENT_SOURCE_DIR}/../signal_processing/data/sample/tiny.csv)

add_executable(signal_replay_compare tests/signal_replay_compare.cpp)
target_link_libraries(signal_replay_compare PRIVATE elevsafe_signal_adapter)
add_test(NAME signal_replay_compare_self_test COMMAND signal_replay_compare --self-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg)
add_test(NAME signal_tcp_compare_self_test COMMAND signal_replay_compare --tcp-self-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg)
set_tests_properties(signal_tcp_compare_self_test PROPERTIES TIMEOUT 30)
add_test(NAME signal_background_test COMMAND ${CMAKE_COMMAND}
    -DCOMPARE_EXE=$<TARGET_FILE:signal_replay_compare>
    -DOFF_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/signal_integration.cfg
    -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/background-self-tests
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/signal_background_test.cmake)
set_tests_properties(signal_background_test PROPERTIES TIMEOUT 60)

add_executable(websocket_message_test tests/websocket_message_test.cpp)
target_link_libraries(websocket_message_test PRIVATE elevsafe_websocket nlohmann_json::nlohmann_json)
add_test(NAME websocket_message_test COMMAND websocket_message_test)

add_executable(websocket_server_test tests/websocket_server_test.cpp)
target_link_libraries(websocket_server_test PRIVATE elevsafe_door_simulation elevsafe_websocket elevsafe_boost_headers nlohmann_json::nlohmann_json)
target_compile_options(websocket_server_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
if(WIN32)
    target_compile_definitions(websocket_server_test PRIVATE _WIN32_WINNT=0x0A00)
endif()
add_test(NAME websocket_server_test COMMAND websocket_server_test)
set_tests_properties(websocket_server_test PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
add_test(NAME websocket_mock_scenarios_test COMMAND websocket_server_test --case mock-scenarios --diagnostics)
set_tests_properties(websocket_mock_scenarios_test PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
add_test(NAME websocket_door_risk_test COMMAND websocket_server_test --case door-risk-scenarios --diagnostics)
set_tests_properties(websocket_door_risk_test PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
add_test(NAME websocket_cli_test COMMAND ${CMAKE_COMMAND}
    -DBACKEND_EXE=$<TARGET_FILE:elevsafe_backend> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/websocket_cli_test.cmake)
set_tests_properties(websocket_message_test PROPERTIES TIMEOUT 30)
set_tests_properties(websocket_cli_test PROPERTIES TIMEOUT 40)
