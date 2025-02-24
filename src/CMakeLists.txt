cmake_minimum_required(VERSION 3.25)
project(fast)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CheckIPOSupported)
include(ExternalProject)
include(find_os_cpp)
include(GetGitRevisionDescription)

add_compile_options(-Wall -Wextra)
check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT error)
if (LTO_SUPPORTED)
    message(STATUS "Using Link-Time Optimization for release-builds only")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    # set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif ()

include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
if (COMPILER_SUPPORTS_MARCH_NATIVE)
    message(STATUS "Building for native architecture (binary might be non-portable)")
    add_compile_options(-march=native)
endif ()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
file(COPY cmdline/wrapper DESTINATION bin)

# used to find git hashes to include in source code
get_git_head_revision(GIT_REFSPEC GIT_SHA1 ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/util/git_sha.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/util/git_sha.cc" @ONLY)

# - # - # - # - # - # - # - # - # - # - # - # - # - # - # - # - # - #
# - # - # - # - # - External Libraries  # - # - # - # - # - # - # - #
# - # - # - # - # - # - # - # - # - # - # - # - # - # - # - # - # - #
find_package(absl CONFIG REQUIRED)
find_package(Boost REQUIRED OPTIONAL_COMPONENTS program_options)
find_package(nlohmann_json CONFIG REQUIRED)

ExternalProject_Add(tree-similarity
        GIT_REPOSITORY https://frosch.cosy.sbg.ac.at/dschmitt/tree-similarity.git
        GIT_TAG origin/master
        INSTALL_DIR external/tsim
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/external/tsim
)
ExternalProject_Get_property(tree-similarity INSTALL_DIR)
set(TREE_SIMILARITY_INCLUDE "${INSTALL_DIR}/include")

# ExternalProject_Add(ankerl-unordered-dense
#        GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
#        GIT_TAG origin/main
#        INSTALL_DIR external/ankerl-unordered-dense
#        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/external/ankerl-unordered-dense
# )
# ExternalProject_Get_property(ankerl-unordered-dense INSTALL_DIR)
# set(ANKERL_UNORDERED_DENSE_INCLUDE "${INSTALL_DIR}/include")

set(SOURCES
        cmdline/cmdline.cc
        util/git_sha.cc
        join/prefix_join.cc
        join/t_join.cc
        join/pass_join.cc
        join/palloc_join.cc)

set(LINK_LIBS
        absl::any
        absl::log
        absl::base
        absl::bits
        absl::flat_hash_map
        absl::random_random
        Boost::program_options
        nlohmann_json::nlohmann_json)

set(INCLUDE_LIBS
        ${TREE_SIMILARITY_INCLUDE}
        # ${ANKERL_UNORDERED_DENSE_INCLUDE}
)
# message("Include paths: ${INCLUDE_LIBS}")

add_executable(fast
        ${SOURCES})
add_dependencies(fast
        tree-similarity
        # ankerl-unordered-dense
)
target_link_libraries(fast PRIVATE ${LINK_LIBS})
target_include_directories(fast SYSTEM PRIVATE ${INCLUDE_LIBS})

add_executable(fast_stats
        ${SOURCES})
add_dependencies(fast_stats
        tree-similarity
        # ankerl-unordered-dense
)
target_link_libraries(fast_stats PRIVATE ${LINK_LIBS})
target_include_directories(fast_stats SYSTEM PRIVATE ${INCLUDE_LIBS})
target_compile_definitions(fast_stats PRIVATE STATISTICS TIMING)

add_executable(fast_dbg
        ${SOURCES})
add_dependencies(fast_dbg
        tree-similarity
        # ankerl-unordered-dense
)
target_link_libraries(fast_dbg PRIVATE ${LINK_LIBS})
target_include_directories(fast_dbg SYSTEM PRIVATE ${INCLUDE_LIBS})
target_compile_definitions(fast_dbg PRIVATE STATISTICS TIMING FAST_DEBUG)

get_target_property(compile_options fast_stats COMPILE_OPTIONS)
message(STATUS "Compile options: ${compile_options}")
