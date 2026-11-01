include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Source archives stay inside the selected build directory; no global install.
FetchContent_Declare(elevsafe_boost
    URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
    URL_HASH SHA256=2575e74ffc3ef1cd0babac2c1ee8bdb5782a0ee672b1912da40e5b4b591ca01f
    SOURCE_SUBDIR headers_only_no_subproject
)
FetchContent_Declare(elevsafe_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(elevsafe_boost elevsafe_json)

add_library(elevsafe_boost_headers INTERFACE)
target_include_directories(elevsafe_boost_headers SYSTEM INTERFACE ${elevsafe_boost_SOURCE_DIR})
# Keep Boost.System's Windows messages UTF-8 in every consuming translation unit.
# This changes diagnostic text only, not socket behavior or WebSocket payloads.
target_compile_definitions(elevsafe_boost_headers INTERFACE BOOST_ALL_NO_LIB
    $<$<PLATFORM_ID:Windows>:BOOST_SYSTEM_USE_UTF8>)
