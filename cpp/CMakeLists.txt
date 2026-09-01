find_package(Threads REQUIRED)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JustCefRuntime.cmake")

option(JUSTCEF_STAGE_RUNTIME "Prepare and stage the native JustCef runtime when building targets" ON)
option(JUSTCEF_PROVIDE_ASIO "Vendor asio inside JustCef (OFF: caller provides asio_headers)" ON)
option(JUSTCEF_PROVIDE_JSON "Vendor json.hpp inside JustCef (OFF: caller provides json_headers)" ON)

if(JUSTCEF_PROVIDE_ASIO)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/asio.hpp")
        message(FATAL_ERROR
            "JUSTCEF_PROVIDE_ASIO=ON but JustCef/cpp/third_party/asio/ is missing. "
            "Either vendor asio there or set -DJUSTCEF_PROVIDE_ASIO=OFF and "
            "define an `asio_headers` INTERFACE target in the caller.")
    endif()
    add_library(asio_headers INTERFACE)
    target_include_directories(asio_headers INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio>)
    target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_HEADER_ONLY)
endif()

if(JUSTCEF_PROVIDE_JSON)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json.hpp")
        message(FATAL_ERROR
            "JUSTCEF_PROVIDE_JSON=ON but JustCef/cpp/third_party/json.hpp is missing. "
            "Either vendor it there or set -DJUSTCEF_PROVIDE_JSON=OFF and "
            "define a `json_headers` INTERFACE target in the caller.")
    endif()
    add_library(json_headers INTERFACE)
    target_include_directories(json_headers INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
endif()

set(LIBJUSTCEF_SOURCES
    AsioSupport.h
    AsyncSignal.h
    Event.h
    IpcTypes.h
    JustCefLogger.cpp
    JustCefLogger.h
    JustCefProcess.cpp
    JustCefProcess.h
    JustCefWindow.cpp
    JustCefWindow.h
    Packet.h
    WindowInternals.h
    DataStream.cpp
    DataStream.h
)

add_library(libjustcef STATIC ${LIBJUSTCEF_SOURCES})
set_target_properties(libjustcef PROPERTIES OUTPUT_NAME justcef)

target_compile_definitions(libjustcef PRIVATE JUSTCEF_EXPECTED_VERSION=${JUSTCEF_VERSION})

target_link_libraries(libjustcef PUBLIC Threads::Threads asio_headers json_headers)
target_include_directories(libjustcef PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(libjustcef PUBLIC dl)
endif()

if(MSVC)
    target_compile_definitions(libjustcef PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
