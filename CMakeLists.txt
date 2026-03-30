cmake_minimum_required(VERSION 3.14)
project(dns_llm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Production flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# ── Dependencies ─────────────────────────────────────────────────────────────

# cJSON (fetched at configure time)
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cJSON
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cJSON)

# OpenSSL (Homebrew on macOS)
set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH "OpenSSL root")
find_package(OpenSSL REQUIRED)

# libcurl (Homebrew on macOS)
set(CURL_ROOT "/opt/homebrew/opt/curl" CACHE PATH "curl root")
find_package(CURL REQUIRED)

find_package(Threads REQUIRED)

# ── Common library ───────────────────────────────────────────────────────────

add_library(dns_llm_common STATIC
    src/common/base32.c
    src/common/base64.c
    src/common/dns_proto.c
)
target_include_directories(dns_llm_common PUBLIC include)
target_link_libraries(dns_llm_common PUBLIC
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
)

# ── Server ───────────────────────────────────────────────────────────────────

add_executable(server_bin src/server/main.c)
target_link_libraries(server_bin PRIVATE
    dns_llm_common
    cjson
    Threads::Threads
)
target_include_directories(server_bin PRIVATE ${cJSON_SOURCE_DIR})

# ── Client ───────────────────────────────────────────────────────────────────

add_executable(client_bin src/client/main.c)
target_link_libraries(client_bin PRIVATE
    dns_llm_common
    cjson
    Threads::Threads
)
target_include_directories(client_bin PRIVATE ${cJSON_SOURCE_DIR})
