function(UnpackCEF platform version download_dir)
  set(CEF_DISTRIBUTION "cef_binary_${version}_${platform}_minimal")
  set(CEF_DOWNLOAD_DIR "${download_dir}")
  set(CEF_MIRROR_BASE_URL "https://static.grayjay.app/cef" CACHE STRING "Base URL for downloading CEF archives")

  set(CEF_ROOT "${CEF_DOWNLOAD_DIR}/${CEF_DISTRIBUTION}" CACHE INTERNAL "CEF_ROOT")

  if(NOT IS_DIRECTORY "${CEF_ROOT}")
    set(CEF_DOWNLOAD_FILENAME "${CEF_DISTRIBUTION}.tar.bz2")
    set(CEF_DOWNLOAD_PATH "${CEF_DOWNLOAD_DIR}/${CEF_DOWNLOAD_FILENAME}")

    if(NOT EXISTS "${CEF_DOWNLOAD_PATH}")
      file(MAKE_DIRECTORY "${CEF_DOWNLOAD_DIR}")

      set(CEF_DOWNLOAD_URL "${CEF_MIRROR_BASE_URL}/${CEF_DOWNLOAD_FILENAME}")
      message(STATUS "Downloading ${CEF_DOWNLOAD_URL} -> ${CEF_DOWNLOAD_PATH}")

      file(DOWNLOAD
        "${CEF_DOWNLOAD_URL}"
        "${CEF_DOWNLOAD_PATH}"
        STATUS CEF_DOWNLOAD_STATUS
        SHOW_PROGRESS
        TLS_VERIFY ON
      )

      list(GET CEF_DOWNLOAD_STATUS 0 CEF_DOWNLOAD_STATUS_CODE)
      list(GET CEF_DOWNLOAD_STATUS 1 CEF_DOWNLOAD_STATUS_MESSAGE)

      if(NOT CEF_DOWNLOAD_STATUS_CODE EQUAL 0)
        file(REMOVE "${CEF_DOWNLOAD_PATH}") # remove partial file
        message(FATAL_ERROR
          "Failed to download '${CEF_DOWNLOAD_URL}' "
          "(code=${CEF_DOWNLOAD_STATUS_CODE}): ${CEF_DOWNLOAD_STATUS_MESSAGE}")
      endif()
    endif()

    message(STATUS "Extracting ${CEF_DOWNLOAD_PATH}...")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${CEF_DOWNLOAD_PATH}"
      WORKING_DIRECTORY ${CEF_DOWNLOAD_DIR}
      RESULT_VARIABLE CEF_EXTRACT_RV
    )
    if(NOT CEF_EXTRACT_RV STREQUAL "0")
      message(FATAL_ERROR "CEF extraction failed with code: ${CEF_EXTRACT_RV}")
    endif()
  endif()
endfunction()