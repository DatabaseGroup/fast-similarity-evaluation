cmake_minimum_required(VERSION 3.27)
project(src)

include(cmake/find_os_cpp.cmake)

# used to find git hashes to include in source code
include(cmake/GetGitRevisionDescription.cmake)
get_git_head_revision(GIT_REFSPEC GIT_SHA1 ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/util/git_sha.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/util/git_sha.cc" @ONLY)

find_package(absl CONFIG REQUIRED)
find_package(Boost REQUIRED OPTIONAL_COMPONENTS program_options)

set(SOURCES
        cmdline/cmdline.cc
        util/git_sha.cc)

set(LINK_LIBS
        absl::any
        absl::log
        absl::base
        absl::bits
        absl::flat_hash_map
        absl::random_random
        Boost::program_options)

add_executable(fast
        ${SOURCES})
target_link_libraries(fast PRIVATE ${LINK_LIBS})

add_executable(fast_stats
        ${SOURCES})
target_link_libraries(fast_stats PRIVATE ${LINK_LIBS})

target_compile_definitions(fast_stats PRIVATE STATISTICS TIMING)
