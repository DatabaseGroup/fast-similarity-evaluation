cmake_minimum_required(VERSION 3.25)
project(src)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CheckIPOSupported)
include(ExternalProject)
include(find_os_cpp)
include(GetGitRevisionDescription)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT error)
if (LTO_SUPPORTED)
    message("Using Link-Time Optimization for release-builds only")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
file(COPY cmdline/wrapper DESTINATION bin)

# used to find git hashes to include in source code
get_git_head_revision(GIT_REFSPEC GIT_SHA1 ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/util/git_sha.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/util/git_sha.cc" @ONLY)

find_package(absl CONFIG REQUIRED)
find_package(Boost REQUIRED OPTIONAL_COMPONENTS program_options)
find_package(nlohmann_json CONFIG REQUIRED)

ExternalProject_Add(tree-similarity
        GIT_REPOSITORY ssh://git@kappa.gsk.moe:4950/patche/tree-similarity.git
        GIT_TAG origin/master
        INSTALL_DIR external/tsim
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/external/tsim
        UPDATE_COMMAND ""
)
ExternalProject_Get_property(tree-similarity INSTALL_DIR)
set(TREE_SIMILARITY_INCLUDE "${INSTALL_DIR}/include")

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
        ${TREE_SIMILARITY_INCLUDE})
message("Include paths: ${INCLUDE_LIBS}")

add_executable(fast
        ${SOURCES})
add_dependencies(fast tree-similarity)
target_link_libraries(fast PRIVATE ${LINK_LIBS})
target_include_directories(fast PRIVATE SYSTEM ${INCLUDE_LIBS})

add_executable(fast_stats
        ${SOURCES})
add_dependencies(fast_stats tree-similarity)
target_link_libraries(fast_stats PRIVATE ${LINK_LIBS})
target_include_directories(fast_stats PRIVATE SYSTEM ${INCLUDE_LIBS})

target_compile_definitions(fast_stats PRIVATE STATISTICS TIMING)
