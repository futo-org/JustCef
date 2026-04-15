foreach(required_var IN ITEMS JUSTCEF_URL JUSTCEF_ZIP_PATH JUSTCEF_EXTRACT_DIR JUSTCEF_VERSION)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "JustCef runtime preparation requires ${required_var}.")
    endif()
endforeach()

function(justcef_version_matches version_file expected_version out_var)
    set(is_current OFF)

    if(EXISTS "${version_file}")
        file(READ "${version_file}" existing_version)
        string(STRIP "${existing_version}" existing_version)
        if(existing_version STREQUAL "${expected_version}")
            set(is_current ON)
        endif()
    endif()

    set(${out_var} "${is_current}" PARENT_SCOPE)
endfunction()

get_filename_component(justcef_zip_dir "${JUSTCEF_ZIP_PATH}" DIRECTORY)
set(justcef_version_file "${JUSTCEF_EXTRACT_DIR}/.justcef.version")
justcef_version_matches("${justcef_version_file}" "${JUSTCEF_VERSION}" justcef_is_current)

if(justcef_is_current)
    message(STATUS "JustCef runtime already prepared at ${JUSTCEF_EXTRACT_DIR}")
else()
    file(MAKE_DIRECTORY "${justcef_zip_dir}")

    message(STATUS "Downloading JustCef runtime from ${JUSTCEF_URL}")
    file(DOWNLOAD
        "${JUSTCEF_URL}"
        "${JUSTCEF_ZIP_PATH}"
        STATUS justcef_download_status
        SHOW_PROGRESS
    )

    list(GET justcef_download_status 0 justcef_download_code)
    list(GET justcef_download_status 1 justcef_download_message)
    if(NOT justcef_download_code EQUAL 0)
        message(FATAL_ERROR "Failed to download JustCef runtime: ${justcef_download_message}")
    endif()

    file(REMOVE_RECURSE "${JUSTCEF_EXTRACT_DIR}")
    file(MAKE_DIRECTORY "${JUSTCEF_EXTRACT_DIR}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${JUSTCEF_ZIP_PATH}"
        WORKING_DIRECTORY "${JUSTCEF_EXTRACT_DIR}"
        RESULT_VARIABLE justcef_extract_result
        ERROR_VARIABLE justcef_extract_error
    )

    if(NOT justcef_extract_result EQUAL 0)
        message(FATAL_ERROR "Failed to extract JustCef runtime: ${justcef_extract_error}")
    endif()

    file(WRITE "${justcef_version_file}" "${JUSTCEF_VERSION}\n")
endif()

if(DEFINED JUSTCEF_STAGE_DIR AND NOT "${JUSTCEF_STAGE_DIR}" STREQUAL "")
    set(justcef_stage_version_file "${JUSTCEF_STAGE_DIR}/.justcef.version")
    justcef_version_matches("${justcef_stage_version_file}" "${JUSTCEF_VERSION}" justcef_stage_is_current)

    if(justcef_stage_is_current)
        message(STATUS "JustCef runtime already staged at ${JUSTCEF_STAGE_DIR}")
    else()
        file(REMOVE_RECURSE "${JUSTCEF_STAGE_DIR}")
        get_filename_component(justcef_stage_parent "${JUSTCEF_STAGE_DIR}" DIRECTORY)
        file(MAKE_DIRECTORY "${justcef_stage_parent}")

        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${JUSTCEF_EXTRACT_DIR}" "${JUSTCEF_STAGE_DIR}"
            RESULT_VARIABLE justcef_stage_result
            ERROR_VARIABLE justcef_stage_error
        )

        if(NOT justcef_stage_result EQUAL 0)
            message(FATAL_ERROR "Failed to stage JustCef runtime: ${justcef_stage_error}")
        endif()

        file(WRITE "${justcef_stage_version_file}" "${JUSTCEF_VERSION}\n")
        message(STATUS "Staged JustCef runtime at ${JUSTCEF_STAGE_DIR}")
    endif()
endif()
