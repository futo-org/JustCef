cmake_minimum_required(VERSION 3.21)
project(justcef_ipc_core_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUSTCEF_NATIVE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(ipc_core_tests
  ipc_core_tests.cc
  ${JUSTCEF_NATIVE_SRC}/endpoint.cc
  ${JUSTCEF_NATIVE_SRC}/event_loop.cc
  ${JUSTCEF_NATIVE_SRC}/pipe.cc
  ${JUSTCEF_NATIVE_SRC}/stream_receiver.cc
  ${JUSTCEF_NATIVE_SRC}/transport.cc
)
target_include_directories(ipc_core_tests PRIVATE ${JUSTCEF_NATIVE_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/stub ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/third_party)
find_package(Threads REQUIRED)
target_link_libraries(ipc_core_tests PRIVATE Threads::Threads)

if(JUSTCEF_SANITIZE)
  target_compile_options(ipc_core_tests PRIVATE -fsanitize=${JUSTCEF_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(ipc_core_tests PRIVATE -fsanitize=${JUSTCEF_SANITIZE})
endif()
