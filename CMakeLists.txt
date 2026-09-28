cmake_minimum_required(VERSION 3.24)

project(
  RufusPlusPlus
  VERSION 0.1.0
  DESCRIPTION "A cross-platform boot-media utility"
  LANGUAGES CXX
)

set(RUFUSPP_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version embedded in the application and distribution package")
set(RUFUSPP_PACKAGE_PLATFORM "" CACHE STRING
    "Platform label used in distribution package filenames")
set(RUFUSPP_PACKAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING
    "Architecture label used in distribution package filenames")
set(RUFUSPP_PACKAGE_VARIANT "" CACHE STRING
    "Optional variant label used in distribution package filenames")

if(NOT RUFUSPP_BUILD_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+([.-][A-Za-z0-9.-]+)?$")
  message(FATAL_ERROR
      "RUFUSPP_BUILD_VERSION must be a numeric version with an optional prerelease suffix")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+"
       RUFUSPP_NUMERIC_BUILD_VERSION "${RUFUSPP_BUILD_VERSION}")
foreach(package_label IN ITEMS RUFUSPP_PACKAGE_PLATFORM
                               RUFUSPP_PACKAGE_ARCH
                               RUFUSPP_PACKAGE_VARIANT)
  if(${package_label} AND
     NOT "${${package_label}}" MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "${package_label} contains unsafe filename characters")
  endif()
endforeach()

if(APPLE)
  enable_language(OBJCXX)
  option(RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
      "Build an unsigned macOS development app that requires whole-process root execution" OFF)
  set(RUFUSPP_MACOS_APP_IDENTIFIER "org.rufusplusplus.app" CACHE STRING
      "Bundle identifier for the signed macOS application")
  set(RUFUSPP_MACOS_HELPER_IDENTIFIER "org.rufusplusplus.app.privileged-helper" CACHE STRING
      "Identifier and Mach service name for the macOS privileged helper")
  set(RUFUSPP_MACOS_TEAM_IDENTIFIER "" CACHE STRING
      "Apple Developer Team ID used to mutually authenticate the app and helper")
  set(RUFUSPP_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING
      "Optional codesign identity used to sign the helper and app after bundling")
  foreach(identifier IN ITEMS RUFUSPP_MACOS_APP_IDENTIFIER RUFUSPP_MACOS_HELPER_IDENTIFIER)
    if(NOT "${${identifier}}" MATCHES "^[A-Za-z0-9.-]+$")
      message(FATAL_ERROR "${identifier} contains characters that are unsafe in a bundle ID")
    endif()
  endforeach()
  if(RUFUSPP_MACOS_TEAM_IDENTIFIER AND
     NOT RUFUSPP_MACOS_TEAM_IDENTIFIER MATCHES "^[A-Z0-9]+$")
    message(FATAL_ERROR "RUFUSPP_MACOS_TEAM_IDENTIFIER must be an Apple Team ID")
  endif()
  if(RUFUSPP_MACOS_UNSIGNED_ROOT_MODE AND
     (RUFUSPP_MACOS_TEAM_IDENTIFIER OR RUFUSPP_MACOS_CODE_SIGN_IDENTITY))
    message(FATAL_ERROR
        "RUFUSPP_MACOS_UNSIGNED_ROOT_MODE cannot be combined with macOS signing settings")
  endif()
endif()

include(CTest)

option(RUFUSPP_BUILD_QT_APP "Build the cross-platform Qt application" ON)
option(RUFUSPP_BUILD_PLATFORM_BACKENDS "Build native platform backends" ON)
option(RUFUSPP_STRICT_WARNINGS "Treat warnings in new cross-platform code as errors" OFF)

add_subdirectory(src/portable)

if(RUFUSPP_BUILD_PLATFORM_BACKENDS)
  add_subdirectory(src/platform)
endif()

if(RUFUSPP_BUILD_QT_APP)
  if(NOT RUFUSPP_BUILD_PLATFORM_BACKENDS)
    message(FATAL_ERROR "The Qt application requires native platform backends")
  endif()
  add_subdirectory(src/qt)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(RUFUSPP_BUILD_QT_APP)
  if(NOT RUFUSPP_PACKAGE_PLATFORM)
    if(APPLE)
      set(RUFUSPP_PACKAGE_PLATFORM "macos")
    elseif(WIN32)
      set(RUFUSPP_PACKAGE_PLATFORM "windows")
    else()
      set(RUFUSPP_PACKAGE_PLATFORM "linux")
    endif()
  endif()
  if(NOT RUFUSPP_PACKAGE_ARCH)
    set(RUFUSPP_PACKAGE_ARCH "unknown")
  endif()

  set(RUFUSPP_PACKAGE_FILE_NAME
      "Rufus++-${RUFUSPP_BUILD_VERSION}-${RUFUSPP_PACKAGE_PLATFORM}-${RUFUSPP_PACKAGE_ARCH}")
  if(RUFUSPP_PACKAGE_VARIANT)
    string(APPEND RUFUSPP_PACKAGE_FILE_NAME
           "-${RUFUSPP_PACKAGE_VARIANT}")
  endif()

  set(CPACK_PACKAGE_NAME "Rufus++")
  set(CPACK_PACKAGE_VENDOR "Rufus++ Contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Cross-platform boot-media utility")
  set(CPACK_PACKAGE_HOMEPAGE_URL
      "https://github.com/farrell236/rufuspp")
  set(CPACK_PACKAGE_VERSION "${RUFUSPP_BUILD_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "${RUFUSPP_PACKAGE_FILE_NAME}")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
  set(CPACK_PACKAGE_CHECKSUM "SHA256")
  set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.txt")
  set(CPACK_MONOLITHIC_INSTALL ON)
  set(CPACK_VERBATIM_VARIABLES ON)

  if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_DMG_VOLUME_NAME "Rufus++ ${RUFUSPP_BUILD_VERSION}")
  elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
  else()
    set(CPACK_GENERATOR "TGZ")
  endif()

  include(CPack)
endif()
