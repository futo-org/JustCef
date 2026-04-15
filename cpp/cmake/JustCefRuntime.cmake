include_guard(GLOBAL)

set(JUSTCEF_VERSION 1 CACHE STRING "JustCef native runtime version")
set(JUSTCEF_BASE_URL "https://static.grayjay.app/justcef" CACHE STRING "JustCef native runtime base URL")
set(JUSTCEF_RUNTIME_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "JustCef runtime helper directory")

function(justcef_detect_rid out_var)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" justcef_processor)

    if(WIN32)
        if(justcef_processor MATCHES "^(amd64|x86_64)$")
            set(justcef_rid "win-x64")
        elseif(justcef_processor MATCHES "^(arm64|aarch64)$")
            set(justcef_rid "win-arm64")
        elseif(justcef_processor MATCHES "^(x86|i[3-6]86)$")
            set(justcef_rid "win-x86")
        endif()
    elseif(APPLE)
        if(justcef_processor MATCHES "^(arm64|aarch64)$")
            set(justcef_rid "osx-arm64")
        elseif(justcef_processor MATCHES "^(x86_64|amd64)$")
            set(justcef_rid "osx-x64")
        endif()
    elseif(UNIX)
        if(justcef_processor MATCHES "^(x86_64|amd64)$")
            set(justcef_rid "linux-x64")
        elseif(justcef_processor MATCHES "^(arm64|aarch64)$")
            set(justcef_rid "linux-arm64")
        elseif(justcef_processor MATCHES "^arm")
            set(justcef_rid "linux-arm")
        endif()
    endif()

    if(NOT justcef_rid)
        message(FATAL_ERROR "JustCef: unsupported platform or architecture '${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}'.")
    endif()

    set(${out_var} "${justcef_rid}" PARENT_SCOPE)
endfunction()

function(justcef_archive_name rid out_var)
    if(rid STREQUAL "linux-x64")
        set(archive_name "JustCefNative-linux-x64.zip")
    elseif(rid STREQUAL "linux-arm64")
        set(archive_name "JustCefNative-linux-arm64.zip")
    elseif(rid STREQUAL "osx-x64")
        set(archive_name "JustCefNative-osx-x64.zip")
    elseif(rid STREQUAL "osx-arm64")
        set(archive_name "JustCefNative-osx-arm64.zip")
    elseif(rid STREQUAL "win-x64")
        set(archive_name "JustCefNative-windows-AMD64.zip")
    else()
        set(archive_name "")
    endif()

    if(NOT archive_name)
        message(FATAL_ERROR "JustCef: no archive mapping for RID '${rid}'.")
    endif()

    set(${out_var} "${archive_name}" PARENT_SCOPE)
endfunction()

function(justcef_resolve_runtime out_dir_var out_url_var out_zip_path_var out_script_var)
    justcef_detect_rid(justcef_rid)
    justcef_archive_name("${justcef_rid}" justcef_archive_name_value)

    set(justcef_cache_root "${CMAKE_BINARY_DIR}/libjustcef/runtime/${JUSTCEF_VERSION}/${justcef_rid}")
    set(justcef_zip_path "${justcef_cache_root}/${justcef_archive_name_value}")
    set(justcef_extract_dir "${justcef_cache_root}/extracted")
    set(justcef_url "${JUSTCEF_BASE_URL}/${JUSTCEF_VERSION}/${justcef_archive_name_value}")
    set(justcef_script "${JUSTCEF_RUNTIME_MODULE_DIR}/EnsureJustCefRuntime.cmake")

    set(${out_dir_var} "${justcef_extract_dir}" PARENT_SCOPE)
    set(${out_url_var} "${justcef_url}" PARENT_SCOPE)
    set(${out_zip_path_var} "${justcef_zip_path}" PARENT_SCOPE)
    set(${out_script_var} "${justcef_script}" PARENT_SCOPE)
endfunction()

function(justcef_stage_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "JustCef: target '${target}' does not exist.")
    endif()

    justcef_resolve_runtime(justcef_runtime_dir justcef_url justcef_zip_path justcef_script)

    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DJUSTCEF_URL=${justcef_url}"
            "-DJUSTCEF_ZIP_PATH=${justcef_zip_path}"
            "-DJUSTCEF_EXTRACT_DIR=${justcef_runtime_dir}"
            "-DJUSTCEF_VERSION=${JUSTCEF_VERSION}"
            "-DJUSTCEF_STAGE_DIR=$<TARGET_FILE_DIR:${target}>/cef"
            -P "${justcef_script}"
        VERBATIM
    )
endfunction()
