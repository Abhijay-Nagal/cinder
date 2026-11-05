cmake_minimum_required(VERSION 3.16)
project(cinder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Build with -DSANITIZE=ON to catch memory bugs (AddressSanitizer + UBSan).
option(SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Everything except main() lives in a library so the tests can link it.
add_library(cinder_core
  src/resp.cpp
  src/store.cpp
  src/commands.cpp
  src/server.cpp
)
target_include_directories(cinder_core PUBLIC src)

add_executable(cinder src/main.cpp)
target_link_libraries(cinder PRIVATE cinder_core)

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE cinder_core)
add_test(NAME unit_tests COMMAND unit_tests)
