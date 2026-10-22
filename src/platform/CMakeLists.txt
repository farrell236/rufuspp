add_library(rufus_plus_plus_platform_backend STATIC
  src/block_device_backend_factory.cpp
  src/read_only_media_inspector.cpp
  src/standalone_filesystem_stager.cpp
  src/windows_to_go_image_stager.cpp
  src/ntfs_bootstrap.cpp
)

add_library(RufusPlusPlus::PlatformBackend ALIAS rufus_plus_plus_platform_backend)

# The unmodified, GPL ms-sys tables are pinned together with their source.
foreach(boot_table IN ITEMS br_ntfs_0x0 br_ntfs_0x54)
  file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/windows-ntfs-boot/${boot_table}.h" boot_sha256)
  if(boot_table STREQUAL "br_ntfs_0x0")
    set(expected_boot_sha256 "66fdf077a5e265b829de0040d714634ea732c48bc8e0d131e7cab7b0dde8c6e5")
  else()
    set(expected_boot_sha256 "258cafd9f0f6c1924e0e6f67b775c5466f3accaadae2b80381a18eac39f807d3")
  endif()
  if(NOT boot_sha256 STREQUAL expected_boot_sha256)
    message(FATAL_ERROR "Windows NTFS boot table checksum mismatch: ${boot_table}")
  endif()
endforeach()

target_include_directories(rufus_plus_plus_platform_backend
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(rufus_plus_plus_platform_backend PUBLIC cxx_std_17)
target_link_libraries(rufus_plus_plus_platform_backend PUBLIC RufusPlusPlus::PortableCore)
if(UNIX)
  find_package(Threads REQUIRED)
  target_sources(rufus_plus_plus_platform_backend PRIVATE src/ntfs_file_tree.cpp src/bundled_ntfs_tools.cpp)
  target_link_libraries(rufus_plus_plus_platform_backend PRIVATE RufusPlusPlus::BundledNtfs Threads::Threads)
  add_dependencies(rufus_plus_plus_platform_backend rufus_ntfs_upstream)
endif()

if(APPLE)
  target_sources(rufus_plus_plus_platform_backend PRIVATE
    src/macos_block_device_backend.cpp
    src/macos_installer_operations.mm
  )
  if(NOT RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
    target_sources(rufus_plus_plus_platform_backend PRIVATE
      src/macos_privileged_helper_client.mm
    )
  endif()
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(DISKARBITRATION_FRAMEWORK DiskArbitration REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  target_link_libraries(rufus_plus_plus_platform_backend PRIVATE
    ${COREFOUNDATION_FRAMEWORK}
    ${DISKARBITRATION_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${SECURITY_FRAMEWORK}
  )
  if(NOT RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
    find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
    target_link_libraries(rufus_plus_plus_platform_backend PRIVATE
      ${SERVICEMANAGEMENT_FRAMEWORK}
    )
  endif()
  target_compile_options(rufus_plus_plus_platform_backend PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_compile_definitions(rufus_plus_plus_platform_backend
    PUBLIC
      RUFUSPP_MACOS_UNSIGNED_ROOT_MODE=$<BOOL:${RUFUSPP_MACOS_UNSIGNED_ROOT_MODE}>
  )
  if(NOT RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
    target_compile_definitions(rufus_plus_plus_platform_backend PRIVATE
      RUFUSPP_MACOS_APP_IDENTIFIER="${RUFUSPP_MACOS_APP_IDENTIFIER}"
      RUFUSPP_MACOS_HELPER_IDENTIFIER="${RUFUSPP_MACOS_HELPER_IDENTIFIER}"
      RUFUSPP_MACOS_TEAM_IDENTIFIER="${RUFUSPP_MACOS_TEAM_IDENTIFIER}"
    )

    add_executable(rufus_plus_plus_privileged_helper src/macos_privileged_helper_main.mm)
    set_target_properties(rufus_plus_plus_privileged_helper PROPERTIES
      OUTPUT_NAME "rufus-plus-plus-privileged-helper"
    )
    target_compile_features(rufus_plus_plus_privileged_helper PRIVATE cxx_std_17)
    target_compile_options(rufus_plus_plus_privileged_helper PRIVATE
      -fobjc-arc -Wall -Wextra -Wpedantic
    )
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_privileged_helper PRIVATE -Werror)
    endif()
    target_compile_definitions(rufus_plus_plus_privileged_helper PRIVATE
      RUFUSPP_MACOS_APP_IDENTIFIER="${RUFUSPP_MACOS_APP_IDENTIFIER}"
      RUFUSPP_MACOS_HELPER_IDENTIFIER="${RUFUSPP_MACOS_HELPER_IDENTIFIER}"
      RUFUSPP_MACOS_TEAM_IDENTIFIER="${RUFUSPP_MACOS_TEAM_IDENTIFIER}"
    )
    target_link_libraries(rufus_plus_plus_privileged_helper PRIVATE
      RufusPlusPlus::PlatformBackend
      RufusPlusPlus::PortableCore
    )

    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/macos/org.rufusplusplus.app.privileged-helper.plist.in
      ${CMAKE_CURRENT_BINARY_DIR}/${RUFUSPP_MACOS_HELPER_IDENTIFIER}.plist
      @ONLY
    )
  endif()
elseif(WIN32)
  target_sources(rufus_plus_plus_platform_backend PRIVATE src/windows_block_device_backend.cpp)
  target_link_libraries(rufus_plus_plus_platform_backend PRIVATE setupapi shell32 advapi32 uuid)
elseif(UNIX)
  target_sources(rufus_plus_plus_platform_backend PRIVATE src/linux_block_device_backend.cpp)
else()
  message(FATAL_ERROR "No block-device discovery backend exists for this platform")
endif()

if(MSVC)
  target_compile_options(rufus_plus_plus_platform_backend PRIVATE /W4)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_platform_backend PRIVATE /WX)
  endif()
else()
  target_compile_options(rufus_plus_plus_platform_backend PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_platform_backend PRIVATE -Werror)
  endif()
endif()
