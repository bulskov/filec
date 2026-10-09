add_library(filec STATIC)
add_library(filec::filec ALIAS filec)

# path and native are pure string logic — the same files on every platform.
target_sources(filec PRIVATE
    path/path.c
    path/native.c
)

# One fs backend per platform.
if(WIN32)
    target_sources(filec PRIVATE
        win32/fs_win32.c
    )
else()
    target_sources(filec PRIVATE
        posix/fs_posix.c
    )
endif()

target_include_directories(filec PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>   # generated version.h
    $<INSTALL_INTERFACE:include>
)

# seqc propagates arena's include dirs.
target_link_libraries(filec PUBLIC seqc)

target_compile_options(filec PRIVATE
    $<IF:$<BOOL:${MSVC}>,
        /W4,
        -Wall -Wextra -Wpedantic -Wno-unused-parameter>
)
