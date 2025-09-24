if(OS_MAC)
  # Shared macOS resources (menus, icons, etc).
  set(DOTCEF_RESOURCES_SRCS
    resources/mac/English.lproj/InfoPlist.strings
    resources/mac/English.lproj/MainMenu.xib
    resources/mac/Info.plist
    resources/mac/shared.icns
    )

  # Info.plist files used for main and helper app bundles.
  set(DOTCEF_INFO_PLIST resources/mac/Info.plist)
  set(DOTCEF_HELPER_INFO_PLIST resources/mac/helper-Info.plist)
elseif(OS_WINDOWS)
  # Shared Windows resources (version information, icons).
  set(DOTCEF_RESOURCES_SRCS
    resources/win/big.ico
    resources/win/resource.h
    resources/win/shared.rc
    resources/win/small.ico
    )

  # Manifest files used for executables.
  set(DOTCEF_EXE_MANIFEST resources/win/shared.exe.manifest)
  set(DOTCEF_COMPATIBILITY_MANIFEST resources/win/compatibility.manifest)
endif()


#
# Shared configuration.
#

# Support nice project/target organization in Visual Studio and Xcode.
if(OS_MACOSX OR OS_WINDOWS)
  # Enable the creation of project folders.
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)

  # Group target contents by directory.
  macro(SET_DOTCEF_SOURCE_GROUPS srcs)
    foreach(FILE ${srcs}) 
      # Get the absolute directory path.
      get_filename_component(ABS_FILE "${FILE}" ABSOLUTE)
      get_filename_component(PARENT_DIR "${ABS_FILE}" DIRECTORY)

      # Remove the common directory prefix. What remains in the group.
      string(REPLACE "${CMAKE_SOURCE_DIR}/examples/" "" GROUP "${PARENT_DIR}")

      # Convert to Windows slashes.
      string(REPLACE "/" "\\" GROUP "${GROUP}")

      source_group("${GROUP}" FILES "${FILE}")
    endforeach()
  endmacro()
endif()

# Set the configuration-specific binary output directory.
if(GEN_NINJA OR GEN_MAKEFILES)
  # Force Ninja and Make to create a subdirectory named after the configuration.
  set(DOTCEF_TARGET_OUT_DIR "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
else()
  set(DOTCEF_TARGET_OUT_DIR "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
endif()

# Set properties common to all example targets.
macro(SET_DOTCEF_PROPERTIES target)
  # Output all binaries to the configuration-specific build directory.
  set_target_properties(${target} PROPERTIES
                        ARCHIVE_OUTPUT_DIRECTORY "${DOTCEF_TARGET_OUT_DIR}"
                        RUNTIME_OUTPUT_DIRECTORY "${DOTCEF_TARGET_OUT_DIR}"
                        LIBRARY_OUTPUT_DIRECTORY "${DOTCEF_TARGET_OUT_DIR}")

  if(OS_MACOSX OR OS_WINDOWS)
    # Place the target in the "examples" folder in Visual Studio and Xcode.
    set_property(TARGET ${target} PROPERTY FOLDER "examples")
  endif()
endmacro()

# Set properties on an example library target.
macro(SET_DOTCEF_LIBRARY_TARGET_PROPERTIES target)
  SET_LIBRARY_TARGET_PROPERTIES(${target})
  SET_DOTCEF_PROPERTIES(${target})
endmacro()

# Set properties on an example executable target.
macro(SET_DOTCEF_EXECUTABLE_TARGET_PROPERTIES target)
  SET_EXECUTABLE_TARGET_PROPERTIES(${target})
  SET_DOTCEF_PROPERTIES(${target})

  if(OS_LINUX)
    # Set rpath so that libraries can be placed next to the executable.
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE)
  elseif(OS_WINDOWS)
    # Add the custom Windows manifest files to the executable.
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "mt.exe" -nologo
              -manifest \"${CMAKE_CURRENT_SOURCE_DIR}/${DOTCEF_EXE_MANIFEST}\"
                        \"${CMAKE_CURRENT_SOURCE_DIR}/${DOTCEF_COMPATIBILITY_MANIFEST}\"
              -outputresource:"${DOTCEF_TARGET_OUT_DIR}/${target}.exe"\;\#1
      COMMENT "Adding manifest..."
      )
  endif()
endmacro()


#
# Target configuration.
#

#
# Shared configuration.
#

if(OS_LINUX OR OS_WINDOWS)
  # Logical target used to link the libcef library on Linux and Windows.
  # On macOS the CEF framework is loaded dynamically at startup.
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

# Main executable sources.
set(DOTCEF_SRCS
  app_browser.cc
  bufferpool.cc
  bufferpool.h
  datastream.cc
  datastream.h
  client.cc
  client.h
  thread_pool.cc
  thread_pool.h
  devtoolsclient.cc
  devtoolsclient.h
  main.cc
  app_factory.h
  main_util.cc
  main_util.h
  client_manager.cc
  client_manager.h
  client_util.cc
  client_util.h
  main.h
  resource_util.cc
  resource_util.h
  ipc.cc
  ipc.h
  pipe.cc
  pipe.h
  work_queue.h
  )

if(OS_LINUX)
  set(DOTCEF_SRCS
    ${DOTCEF_SRCS}
    app_other.cc
    app_renderer.cc
    client_util_linux.cc
    main_linux.cc
    resource_util_linux.cc
    resource_util_posix.cc
    )
elseif(OS_WINDOWS)
  set(DOTCEF_SRCS
    ${DOTCEF_SRCS}
    app_other.cc
    app_renderer.cc
    client_util_win.cc
    main_win.cc
    resource_util_win.cc
    )
elseif(OS_MAC)
  set(DOTCEF_SRCS
    ${DOTCEF_SRCS}
    client_util_mac.mm
    main_mac.mm
    resource_util_mac.mm
    resource_util_posix.cc
    )

  # On macOS a separate helper executable is used for subprocesses.
  set(DOTCEF_HELPER_SRCS
    app_factory.h
    main_util.cc
    main_util.h
    process_helper_mac.cc
    process_helper_mac_minimal.cc
    process_helper_mac.h
    app_other.cc
    app_renderer.cc
    )
endif()

#
# Shared configuration.
#

# Target executable names.
set(DOTCEF_TARGET "dotcefnative")
if(OS_MAC)
  set(DOTCEF_HELPER_TARGET "dotcefnative_Helper")
  set(DOTCEF_HELPER_OUTPUT_NAME "dotcefnative Helper")
endif()

#
# Linux configuration.
#

if(OS_LINUX)
  #find_package(PkgConfig REQUIRED)
  #pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
  #include_directories(${GTK3_INCLUDE_DIRS})
  #link_directories(${GTK3_LIBRARY_DIRS})
  #add_definitions(${GTK3_CFLAGS_OTHER})

  # Executable target.
  add_executable(${DOTCEF_TARGET} ${DOTCEF_SRCS})
  SET_DOTCEF_EXECUTABLE_TARGET_PROPERTIES(${DOTCEF_TARGET})
  add_dependencies(${DOTCEF_TARGET} libcef_dll_wrapper)
  #target_link_libraries(${DOTCEF_TARGET} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} ${GTK3_LIBRARIES})
  target_link_libraries(${DOTCEF_TARGET} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})

  COPY_FILES("${DOTCEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${DOTCEF_TARGET_OUT_DIR}")
  COPY_FILES("${DOTCEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${DOTCEF_TARGET_OUT_DIR}")
  SET_LINUX_SUID_PERMISSIONS("${DOTCEF_TARGET}" "${DOTCEF_TARGET_OUT_DIR}/chrome-sandbox")
endif()

#
# Mac OS X configuration.
#

if(OS_MAC)
  # Create source groups for Xcode.
  SET_DOTCEF_SOURCE_GROUPS("${DOTCEF_SRCS}")
  SET_DOTCEF_SOURCE_GROUPS("${DOTCEF_HELPER_SRCS}")

  # Output path for the main app bundle.
  set(DOTCEF_APP "${DOTCEF_TARGET_OUT_DIR}/${DOTCEF_TARGET}.app")

  # Variables referenced from the main Info.plist file.
  set(EXECUTABLE_NAME "${DOTCEF_TARGET}")
  set(PRODUCT_NAME "${DOTCEF_TARGET}")

  if(USE_SANDBOX)
    # Logical target used to link the cef_sandbox library.
    ADD_LOGICAL_TARGET("cef_sandbox_lib" "${CEF_SANDBOX_LIB_DEBUG}" "${CEF_SANDBOX_LIB_RELEASE}")
  endif()

  # Main app bundle target.
  add_executable(${DOTCEF_TARGET} MACOSX_BUNDLE ${DOTCEF_SRCS} ${DOTCEF_RESOURCES_SRCS})
  SET_DOTCEF_EXECUTABLE_TARGET_PROPERTIES(${DOTCEF_TARGET})
  add_dependencies(${DOTCEF_TARGET} libcef_dll_wrapper)
  target_link_libraries(${DOTCEF_TARGET} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
  set_target_properties(${DOTCEF_TARGET} PROPERTIES
    RESOURCE "${DOTCEF_RESOURCES_SRCS}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${DOTCEF_INFO_PLIST}"
    )

  # Copy the CEF framework into the Frameworks directory.
  add_custom_command(
    TARGET ${DOTCEF_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_BINARY_DIR}/Chromium Embedded Framework.framework"
            "${DOTCEF_APP}/Contents/Frameworks/Chromium Embedded Framework.framework"
    VERBATIM
    )

  # Create the multiple Helper app bundle targets.
  foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
    # Convert to a list and extract the suffix values.
    string(REPLACE ":" ";" _suffix_list ${_suffix_list})
    list(GET _suffix_list 0 _name_suffix)
    list(GET _suffix_list 1 _target_suffix)
    list(GET _suffix_list 2 _plist_suffix)

    # Define Helper target and output names.
    set(_helper_target "${DOTCEF_HELPER_TARGET}${_target_suffix}")
    set(_helper_output_name "${DOTCEF_HELPER_OUTPUT_NAME}${_name_suffix}")

    # Create Helper-specific variants of the helper-Info.plist file. Do this
    # manually because the configure_file command (which is executed as part of
    # MACOSX_BUNDLE_INFO_PLIST) uses global env variables and would insert the
    # wrong values with multiple targets.
    set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${DOTCEF_HELPER_INFO_PLIST}" _plist_contents)
    string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
    file(WRITE ${_helper_info_plist} ${_plist_contents})

    # Create Helper executable target.
    message(STATUS "DOTCEF_HELPER_SRCS ${DOTCEF_HELPER_SRCS}")
    add_executable(${_helper_target} MACOSX_BUNDLE ${DOTCEF_HELPER_SRCS})
    SET_DOTCEF_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
    add_dependencies(${_helper_target} libcef_dll_wrapper)
    target_link_libraries(${_helper_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    set_target_properties(${_helper_target} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${_helper_info_plist}
      OUTPUT_NAME ${_helper_output_name}
      )

    if(USE_SANDBOX)
      target_link_libraries(${_helper_target} cef_sandbox_lib)
    endif()

    # Add the Helper as a dependency of the main executable target.
    add_dependencies(${DOTCEF_TARGET} "${_helper_target}")

    # Copy the Helper app bundle into the Frameworks directory.
    add_custom_command(
      TARGET ${DOTCEF_TARGET}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${DOTCEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
              "${DOTCEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
      VERBATIM
      )
  endforeach()

  # Manually process and copy over resource files.
  # The Xcode generator can support this via the set_target_properties RESOURCE
  # directive but that doesn't properly handle nested resource directories.
  # Remove these prefixes from input file paths.
  set(PREFIXES
    "resources/mac/"
    )
  COPY_MAC_RESOURCES("${DOTCEF_RESOURCES_SRCS}" "${PREFIXES}" "${DOTCEF_TARGET}" "${CMAKE_CURRENT_SOURCE_DIR}" "${DOTCEF_APP}")
endif()


#
# Windows configuration.
#

if(OS_WINDOWS)
    # Add resources to the sources variable for convenience.
  set(DOTCEF_SRCS
    ${DOTCEF_SRCS}
    ${DOTCEF_RESOURCES_SRCS}
    )

  # Create source groups for Visual Studio.
  SET_DOTCEF_SOURCE_GROUPS("${DOTCEF_SRCS}")

  # Executable target.
  add_executable(${DOTCEF_TARGET} WIN32 ${DOTCEF_SRCS})
  SET_DOTCEF_EXECUTABLE_TARGET_PROPERTIES(${DOTCEF_TARGET})
  add_dependencies(${DOTCEF_TARGET} libcef_dll_wrapper)
  target_link_libraries(${DOTCEF_TARGET} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})

  if(USE_SANDBOX)
    # Logical target used to link the cef_sandbox library.
    ADD_LOGICAL_TARGET("cef_sandbox_lib" "${CEF_SANDBOX_LIB_DEBUG}" "${CEF_SANDBOX_LIB_RELEASE}")
    target_link_libraries(${DOTCEF_TARGET} cef_sandbox_lib ${CEF_SANDBOX_STANDARD_LIBS})
  endif()

  # Copy binary and resource files to the target output directory.
  COPY_FILES("${DOTCEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${DOTCEF_TARGET_OUT_DIR}")
  COPY_FILES("${DOTCEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${DOTCEF_TARGET_OUT_DIR}")
endif()