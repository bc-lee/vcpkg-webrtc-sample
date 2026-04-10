cmake_minimum_required(VERSION 3.24)

project(vcpkg_webrtc_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(absl CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Python3 COMPONENTS Interpreter)
find_package(webrtc CONFIG REQUIRED)

set(WEBRTC_APPRTC_SAMPLE_VIDEO ""
    CACHE FILEPATH "Optional MP4 sample used by local validation and e2e tests.")

set(WEBRTC_APPRTC_CLI_SOURCES
    src/apprtc_common.cc
    src/apprtc_call.cc
    src/apprtc_peer_connection.cc
    src/apprtc_signaling_client.cc
    src/file_video_source.cc
    src/main.cc
    src/sine_wave_capturer.cc
)

add_executable(webrtc_apprtc_cli ${WEBRTC_APPRTC_CLI_SOURCES})
target_include_directories(webrtc_apprtc_cli PRIVATE . src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(webrtc_apprtc_cli PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(webrtc_apprtc_cli
    PRIVATE
        absl::check
        absl::cleanup
        absl::flags
        absl::flags_parse
        absl::flags_usage
        absl::log_initialize
        absl::status
        absl::statusor
        absl::strings
        absl::synchronization
        CURL::libcurl
        ixwebsocket::ixwebsocket
        JsonCpp::JsonCpp
        OpenSSL::SSL
        OpenSSL::Crypto
        webrtc::webrtc
        ${FFMPEG_LIBRARIES}
)

if(MSVC)
    target_compile_options(webrtc_apprtc_cli PRIVATE /W4 /GR- /EHs-c- /wd4100 /wd4127 /wd4702)
else()
    target_compile_options(webrtc_apprtc_cli PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()

if(APPLE)
    target_link_options(webrtc_apprtc_cli PRIVATE -Wl,-no_warn_duplicate_libraries)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(webrtc_apprtc_cli PRIVATE -Wthread-safety)
endif()

add_test(NAME webrtc_apprtc_cli_help COMMAND webrtc_apprtc_cli --helpfull)
set_tests_properties(webrtc_apprtc_cli_help PROPERTIES WILL_FAIL TRUE)
if(WEBRTC_APPRTC_SAMPLE_VIDEO)
    add_test(NAME webrtc_apprtc_cli_validate_inputs
        COMMAND webrtc_apprtc_cli
            --room_id=validate-inputs
            --validate_inputs_only
            --video_path=${WEBRTC_APPRTC_SAMPLE_VIDEO}
    )
endif()

if(Python3_Interpreter_FOUND AND WEBRTC_APPRTC_SAMPLE_VIDEO)
    add_test(NAME webrtc_apprtc_cli_e2e
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/run_e2e.py
            --binary=$<TARGET_FILE:webrtc_apprtc_cli>
            --video=${WEBRTC_APPRTC_SAMPLE_VIDEO}
    )
endif()
