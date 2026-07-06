cmake_minimum_required(VERSION 3.16)
project(collect_call_b2bua C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_GNU_SOURCE)

# ── PJSIP ────────────────────────────────────────────────────────────────
set(PJSIP_PREFIX "/usr/local" CACHE PATH "PJSIP install prefix")

execute_process(COMMAND uname -m
    OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
set(PJSIP_SUFFIX "-${ARCH}-unknown-linux-gnu")

find_path(PJSUA_INCLUDE_DIR
    NAMES pjsua-lib/pjsua.h
    PATHS ${PJSIP_PREFIX}/include
    REQUIRED)

set(PJSIP_LIB_NAMES
    pjsua pjsip-ua pjsip-simple pjsip
    pjmedia-codec pjmedia pjmedia-audiodev
    pjnath pjlib-util pj
    resample srtp gsmcodec speex)

set(PJSIP_LINK_LIBS "")
foreach(lib ${PJSIP_LIB_NAMES})
    find_library(FOUND_${lib}
        NAMES "${lib}${PJSIP_SUFFIX}" "${lib}"
        PATHS ${PJSIP_PREFIX}/lib)
    if(FOUND_${lib})
        list(APPEND PJSIP_LINK_LIBS ${FOUND_${lib}})
    endif()
endforeach()

# ── System deps ───────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
find_library(OPENSSL_SSL    ssl)
find_library(OPENSSL_CRYPTO crypto)
find_library(ALSA_LIB       asound)

# ── Sources ───────────────────────────────────────────────────────────────
set(SOURCES
    src/main.c
    src/app_logger.c
    src/b2bua.c
    src/leg_a.c
    src/leg_b.c
    src/session.c
    src/utils.c
    src/validation.c
    src/api_mapping.c
    src/runtime_config.c
    src/prompt_mapping.c
    src/options.c)

add_executable(collect_call ${SOURCES})

target_include_directories(collect_call PRIVATE
    include
    ${PJSUA_INCLUDE_DIR})

target_link_libraries(collect_call
    ${PJSIP_LINK_LIBS}
    Threads::Threads
    ${OPENSSL_SSL}
    ${OPENSSL_CRYPTO}
    ${ALSA_LIB}
    m rt)

target_compile_options(collect_call PRIVATE -Wall -Wextra -O2)

install(TARGETS collect_call DESTINATION /opt/collect_call)
install(DIRECTORY wav/ DESTINATION /opt/collect_call/wav
        FILES_MATCHING PATTERN "*.wav" PATTERN "*.conf")
