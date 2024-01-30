cmake_minimum_required(VERSION 3.27)
project(src)

include(cmake/find_os_cpp.cmake)

find_package(absl CONFIG REQUIRED)

add_executable(main
        cmdline/cmdline.cc)
target_link_libraries(main PRIVATE absl::any absl::log absl::base absl::bits absl::flat_hash_map absl::random_random)
