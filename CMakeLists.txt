cmake_minimum_required(VERSION 3.20)
project(plugin_music VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(OmniDeskSDK REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(music_core OBJECT
    src/config.cpp
    src/lrc_parser.cpp
    src/lyrics_cache.cpp
    src/lrclib_client.cpp
    src/adam_client.cpp
    src/llm_translator.cpp
    src/music_engine.cpp
    src/ui_views.cpp
    src/plugin.cpp
)
target_include_directories(music_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(music_core PUBLIC
    OmniDesk::SDK
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
target_compile_options(music_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(plugin_music SHARED
    $<TARGET_OBJECTS:music_core>
    src/plugin_main.cpp
)
target_include_directories(plugin_music PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(plugin_music PRIVATE
    OmniDesk::SDK
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
target_compile_options(plugin_music PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(plugin_music PROPERTIES
    OUTPUT_NAME "plugin_music"
    PREFIX "lib"
)

enable_testing()

add_executable(test_abi tests/test_abi.cpp)
target_link_libraries(test_abi PRIVATE OmniDesk::SDK ${CMAKE_DL_LIBS})
add_test(NAME music_abi_smoke COMMAND test_abi $<TARGET_FILE:plugin_music> "plugin-music")

add_executable(test_music tests/test_music.cpp)
target_link_libraries(test_music PRIVATE
    music_core
    OmniDesk::SDK
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_compile_options(test_music PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME music_suite COMMAND test_music $<TARGET_FILE:plugin_music>)
