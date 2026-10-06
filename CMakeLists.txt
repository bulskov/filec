cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILEC_VERSION)
project(filec
    VERSION ${FILEC_VERSION}
    DESCRIPTION "Portable paths and filesystem access for C11"
    HOMEPAGE_URL "https://github.com/bulskov/filec"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Are we the top-level project, or pulled in via FetchContent/add_subdirectory?
# Tests, sanitizers and install rules are only on by default at top level, so
# consumers (e.g. fskim) don't build filec's test suite.
if(DEFINED PROJECT_IS_TOP_LEVEL)
    set(FILEC_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
elseif(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FILEC_TOP_LEVEL ON)
else()
    set(FILEC_TOP_LEVEL OFF)
endif()

option(FILEC_BUILD_TESTS "Build filec's own test suite" ${FILEC_TOP_LEVEL})
option(FILEC_INSTALL "Generate install rules for filec" ${FILEC_TOP_LEVEL})

# none | address (ASan + UBSan) | thread (TSan).  ASan and TSan cannot be
# combined, so this is a choice rather than two switches.
set(FILEC_SANITIZER "none" CACHE STRING "Sanitizer: none, address, thread")
set_property(CACHE FILEC_SANITIZER PROPERTY STRINGS none address thread)

if(FILEC_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(FILEC_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT FILEC_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "FILEC_SANITIZER must be none, address or thread")
endif()

# --- Dependencies ------------------------------------------------------------
# Fetched from source so they are built with the same compiler and flags
# (including sanitizers).  Dependency names are shared across the library
# family (arena, seqc, ctt, threadc, filec): FetchContent keeps the FIRST
# declaration of a name, so a parent project pins the versions for everyone.
#
# Build against local checkouts with the `dev` preset, or by hand:
#   -DFETCHCONTENT_SOURCE_DIR_SEQC=$HOME/repos/seqc
#   -DFETCHCONTENT_SOURCE_DIR_CTT=$HOME/repos/ctt
include(FetchContent)

set(FILEC_SEQC_GIT_REPOSITORY "https://github.com/bulskov/seqc.git"
    CACHE STRING "Clone URL for the seqc dependency")
set(FILEC_SEQC_GIT_TAG "v2.2.2"
    CACHE STRING "Tag or commit of the seqc dependency")
set(FILEC_CTT_GIT_TAG "v0.3.1"
    CACHE STRING "Tag or commit of the ctt dependency (tests only)")

FetchContent_Declare(seqc
    GIT_REPOSITORY ${FILEC_SEQC_GIT_REPOSITORY}
    GIT_TAG        ${FILEC_SEQC_GIT_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(seqc)

# --- Library -----------------------------------------------------------------
configure_file(
    include/filec/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/filec/version.h)

add_subdirectory(src)

# --- Tests -------------------------------------------------------------------
if(FILEC_BUILD_TESTS)
    FetchContent_Declare(ctt
        GIT_REPOSITORY https://github.com/bulskov/ctt.git
        GIT_TAG        ${FILEC_CTT_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(ctt)

    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install -----------------------------------------------------------------
if(FILEC_INSTALL)
    install(TARGETS filec ARCHIVE DESTINATION lib)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/filec
        DESTINATION include
        PATTERN "*.in" EXCLUDE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/filec/version.h
        DESTINATION include/filec)
endif()
