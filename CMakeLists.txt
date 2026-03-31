cmake_minimum_required(VERSION 3.14)
project(dns_claw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Production flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wformat=2 -Wformat-security")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -fstack-protector-strong")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")

# ── Dependencies ─────────────────────────────────────────────────────────────

# cJSON (fetched at configure time — static only)
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cJSON
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cJSON)

# OpenSSL
if(APPLE)
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH "OpenSSL root")
endif()
find_package(OpenSSL REQUIRED)

# libcurl
if(APPLE)
    set(CURL_ROOT "/opt/homebrew/opt/curl" CACHE PATH "curl root")
endif()
find_package(CURL REQUIRED)

find_package(Threads REQUIRED)

# ── Common library ───────────────────────────────────────────────────────────

add_library(dnsclaw_common STATIC
    src/common/base32.c
    src/common/base64.c
    src/common/config.c
    src/common/crypto.c
    src/common/dns_proto.c
)
target_include_directories(dnsclaw_common PUBLIC include)
target_link_libraries(dnsclaw_common PUBLIC
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
)

# ── Server ───────────────────────────────────────────────────────────────────

add_executable(dnsclaw-server
    src/server/main.c
    src/server/log.c
    src/server/session.c
    src/server/llm.c
    src/server/handler.c
    src/server/transport.c
)
target_link_libraries(dnsclaw-server PRIVATE
    dnsclaw_common
    cjson
    Threads::Threads
)
target_include_directories(dnsclaw-server PRIVATE ${cJSON_SOURCE_DIR})

# ── Client ───────────────────────────────────────────────────────────────────

add_executable(dnsclaw
    src/client/main.c
    src/client/render.c
    src/client/spinner.c
    src/client/protocol.c
    src/client/ui.c
)
target_link_libraries(dnsclaw PRIVATE
    dnsclaw_common
    cjson
    Threads::Threads
)
target_include_directories(dnsclaw PRIVATE ${cJSON_SOURCE_DIR})

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS dnsclaw-server dnsclaw DESTINATION bin)
