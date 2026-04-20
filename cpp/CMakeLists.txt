find_package(Threads REQUIRED)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JustCefRuntime.cmake")

option(JUSTCEF_STAGE_RUNTIME "Prepare and stage the native JustCef runtime when building targets" ON)

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

target_link_libraries(libjustcef PUBLIC Threads::Threads)
target_include_directories(libjustcef PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/libsync>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/libsync/asio>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(libjustcef PUBLIC dl)
endif()

if(MSVC)
    target_compile_definitions(libjustcef PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
