function(UnpackCEF platform version download_dir)
  set(CEF_DISTRIBUTION "cef_binary_${version}_${platform}_minimal")
  set(CEF_DOWNLOAD_DIR "${download_dir}")

  set(CEF_ROOT "${CEF_DOWNLOAD_DIR}/${CEF_DISTRIBUTION}" CACHE INTERNAL "CEF_ROOT")

  if(NOT IS_DIRECTORY "${CEF_ROOT}")
    set(CEF_DOWNLOAD_FILENAME "${CEF_DISTRIBUTION}.tar.bz2")
    set(CEF_DOWNLOAD_PATH "${CEF_DOWNLOAD_DIR}/${CEF_DOWNLOAD_FILENAME}")
    if(NOT EXISTS "${CEF_DOWNLOAD_PATH}")
      message(FATAL_ERROR "Path '${CEF_DOWNLOAD_PATH}' does not exist.")
    endif()

    message(STATUS "Extracting ${CEF_DOWNLOAD_PATH}...")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${CEF_DOWNLOAD_DIR}/${CEF_DOWNLOAD_FILENAME}"
      WORKING_DIRECTORY ${CEF_DOWNLOAD_DIR}
      )
  endif()
endfunction()
