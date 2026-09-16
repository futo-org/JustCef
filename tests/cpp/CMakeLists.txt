cmake_minimum_required(VERSION 3.21)
project(justcef_cpp_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUSTCEF_TESTS_ASIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/third_party/asio" CACHE PATH "Directory containing asio.hpp")
set(JUSTCEF_TESTS_JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/third_party" CACHE PATH "Directory containing json.hpp")
if(NOT EXISTS "${JUSTCEF_TESTS_ASIO_DIR}/asio.hpp" OR NOT EXISTS "${JUSTCEF_TESTS_JSON_DIR}/json.hpp")
    message(FATAL_ERROR "Set JUSTCEF_TESTS_ASIO_DIR to the directory with asio.hpp and JUSTCEF_TESTS_JSON_DIR to the directory with json.hpp.")
endif()
set(JUSTCEF_TESTS_SANITIZER "" CACHE STRING "Sanitizer to build with: thread, address or empty")

if(JUSTCEF_TESTS_SANITIZER)
    add_compile_options(-fsanitize=${JUSTCEF_TESTS_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${JUSTCEF_TESTS_SANITIZER})
endif()

add_library(asio_headers INTERFACE)
target_include_directories(asio_headers INTERFACE "${JUSTCEF_TESTS_ASIO_DIR}")
target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_HEADER_ONLY)

add_library(json_headers INTERFACE)
target_include_directories(json_headers INTERFACE "${JUSTCEF_TESTS_JSON_DIR}")

set(JUSTCEF_PROVIDE_ASIO OFF CACHE BOOL "" FORCE)
set(JUSTCEF_PROVIDE_JSON OFF CACHE BOOL "" FORCE)
set(JUSTCEF_STAGE_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cpp" justcef)

get_filename_component(JUSTCEF_TESTS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(justcef_unit_tests main.cpp TestSupport.cpp UnitTests.cpp)
target_include_directories(justcef_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest")
target_link_libraries(justcef_unit_tests PRIVATE libjustcef)
target_compile_definitions(justcef_unit_tests PRIVATE JUSTCEF_TESTS_REPO_ROOT="${JUSTCEF_TESTS_REPO_ROOT}")

add_executable(justcef_fake_tests main.cpp TestSupport.cpp FakeNative.cpp FakeTests.cpp)
target_include_directories(justcef_fake_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest")
target_link_libraries(justcef_fake_tests PRIVATE libjustcef)
target_compile_definitions(justcef_fake_tests PRIVATE JUSTCEF_TESTS_REPO_ROOT="${JUSTCEF_TESTS_REPO_ROOT}")

enable_testing()
add_test(NAME justcef_unit_tests COMMAND justcef_unit_tests)
add_test(NAME justcef_fake_tests COMMAND justcef_fake_tests)
set_tests_properties(justcef_unit_tests justcef_fake_tests PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
