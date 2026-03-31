cmake_minimum_required(VERSION 3.14)
# Allow cJSON's old cmake_minimum_required(VERSION 2.8.5) on newer CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
project(dns_claw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler warnings
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wshadow -Wformat=2 -Wformat-security -Werror=format-security")

# Hardened release flags
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE")

# Debug: ASan + UBSan
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")

# ThreadSanitizer build type (use: cmake -DCMAKE_BUILD_TYPE=TSan)
set(CMAKE_C_FLAGS_TSAN "-g -O1 -fsanitize=thread -fno-omit-frame-pointer")

# Linker hardening (Linux only; macOS doesn't support these)
if(NOT APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now -pie")
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────

# cJSON (fetched at configure time — static only)
include(FetchContent)
if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()
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

# libedit (optional — enables command history and line editing)
find_library(LIBEDIT_LIB edit)
find_path(LIBEDIT_INC histedit.h)

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

if(LIBEDIT_LIB AND LIBEDIT_INC)
    target_compile_definitions(dnsclaw PRIVATE HAVE_LIBEDIT)
    target_link_libraries(dnsclaw PRIVATE ${LIBEDIT_LIB})
    target_include_directories(dnsclaw PRIVATE ${LIBEDIT_INC})
    message(STATUS "libedit found — command history enabled")
else()
    message(STATUS "libedit not found — using basic line input")
endif()

# ── Tests ────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(test_base64 tests/test_base64.c)
target_link_libraries(test_base64 PRIVATE dnsclaw_common)
add_test(NAME base64 COMMAND test_base64)

add_executable(test_base32 tests/test_base32.c)
target_link_libraries(test_base32 PRIVATE dnsclaw_common)
add_test(NAME base32 COMMAND test_base32)

add_executable(test_crypto tests/test_crypto.c)
target_link_libraries(test_crypto PRIVATE dnsclaw_common)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_dns_proto tests/test_dns_proto.c)
target_link_libraries(test_dns_proto PRIVATE dnsclaw_common)
add_test(NAME dns_proto COMMAND test_dns_proto)

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS dnsclaw-server dnsclaw DESTINATION bin)
