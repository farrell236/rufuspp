add_executable(rufus_plus_plus_portable_core_tests
  portable_core_tests.cpp
)

target_compile_features(rufus_plus_plus_portable_core_tests PRIVATE cxx_std_17)
target_link_libraries(rufus_plus_plus_portable_core_tests PRIVATE RufusPlusPlus::PortableCore)

add_test(NAME portable-core COMMAND rufus_plus_plus_portable_core_tests)

add_executable(rufus_plus_plus_backend_core_tests
  backend_core_tests.cpp
)

target_compile_features(rufus_plus_plus_backend_core_tests PRIVATE cxx_std_17)
target_include_directories(rufus_plus_plus_backend_core_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/portable/src
)
target_link_libraries(rufus_plus_plus_backend_core_tests PRIVATE RufusPlusPlus::PortableCore)

add_test(NAME backend-core COMMAND rufus_plus_plus_backend_core_tests)

if(UNIX AND TARGET rufus_plus_plus_platform_backend)
  add_executable(rufus_plus_plus_ntfs_image_tests ntfs_image_tests.cpp)
  target_compile_features(rufus_plus_plus_ntfs_image_tests PRIVATE cxx_std_17)
  target_compile_options(rufus_plus_plus_ntfs_image_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_ntfs_image_tests PRIVATE -Werror)
  endif()
  target_link_libraries(rufus_plus_plus_ntfs_image_tests PRIVATE RufusPlusPlus::PlatformBackend)
  rufuspp_bundle_ntfs_formatter(rufus_plus_plus_ntfs_image_tests)
  add_test(NAME direct-ntfs COMMAND rufus_plus_plus_ntfs_image_tests)
  set_tests_properties(direct-ntfs PROPERTIES LABELS "unit;ntfs" TIMEOUT 120)
  add_test(NAME direct-ntfs-large-file COMMAND rufus_plus_plus_ntfs_image_tests --large)
  set_tests_properties(direct-ntfs-large-file PROPERTIES LABELS "integration;ntfs;large-file" TIMEOUT 300)
endif()

add_executable(rufus_plus_plus_image_writer_tests
  image_writer_tests.cpp
)
target_compile_features(rufus_plus_plus_image_writer_tests PRIVATE cxx_std_17)
target_link_libraries(rufus_plus_plus_image_writer_tests PRIVATE RufusPlusPlus::PortableCore)
if(MSVC)
  target_compile_options(rufus_plus_plus_image_writer_tests PRIVATE /W4)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_image_writer_tests PRIVATE /WX)
  endif()
else()
  target_compile_options(rufus_plus_plus_image_writer_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_image_writer_tests PRIVATE -Werror)
  endif()
endif()

add_test(NAME raw-image-writer COMMAND rufus_plus_plus_image_writer_tests raw)
add_test(NAME file-image-writer COMMAND rufus_plus_plus_image_writer_tests file)
set_tests_properties(raw-image-writer file-image-writer PROPERTIES
  LABELS "unit;writers"
  TIMEOUT 60
)

add_executable(rufus_plus_plus_native_write_tests native_write_tests.cpp)
target_compile_features(rufus_plus_plus_native_write_tests PRIVATE cxx_std_17)
target_include_directories(rufus_plus_plus_native_write_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/platform/src
)
target_link_libraries(rufus_plus_plus_native_write_tests PRIVATE RufusPlusPlus::PortableCore)
if(MSVC)
  target_compile_options(rufus_plus_plus_native_write_tests PRIVATE /W4)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_native_write_tests PRIVATE /WX)
  endif()
else()
  target_compile_options(rufus_plus_plus_native_write_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_native_write_tests PRIVATE -Werror)
  endif()
endif()
add_test(NAME native-write-safety COMMAND rufus_plus_plus_native_write_tests)
set_tests_properties(native-write-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 60)

if(APPLE AND TARGET rufus_plus_plus_platform_backend)
  add_executable(rufus_plus_plus_macos_write_ranges_tests macos_write_ranges_tests.mm)
  target_compile_features(rufus_plus_plus_macos_write_ranges_tests PRIVATE cxx_std_17)
  target_compile_options(rufus_plus_plus_macos_write_ranges_tests PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_macos_write_ranges_tests PRIVATE -Werror)
  endif()
  target_include_directories(rufus_plus_plus_macos_write_ranges_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/platform/src)
  target_link_libraries(rufus_plus_plus_macos_write_ranges_tests PRIVATE
    RufusPlusPlus::PortableCore ${FOUNDATION_FRAMEWORK})
  add_test(NAME macos-write-range-protocol COMMAND rufus_plus_plus_macos_write_ranges_tests)
  set_tests_properties(macos-write-range-protocol PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
endif()

option(RUFUSPP_RUN_VIRTUAL_DISK_TESTS
  "Test native transports using automatically created disposable disk images" OFF)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME virtual-disk-runner-safety COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/virtual_disk_runner_tests.py)
  set_tests_properties(virtual-disk-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
  if(APPLE)
    add_test(NAME physical-usb-runner-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/physical_usb_runner_tests.py)
    set_tests_properties(physical-usb-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
    add_test(NAME qemu-boot-preparation-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/qemu_boot_tests.py)
    set_tests_properties(qemu-boot-preparation-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
    add_test(NAME windows-usb-boot-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/windows_usb_boot_tests.py)
    set_tests_properties(windows-usb-boot-safety PROPERTIES LABELS "unit;ntfs;native" TIMEOUT 30)
    add_test(NAME persistence-qemu-runner-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/persistence_qemu_runner_tests.py)
    set_tests_properties(persistence-qemu-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
    add_test(NAME physical-gpt-usb-runner-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/physical_gpt_usb_runner_tests.py)
    set_tests_properties(physical-gpt-usb-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
    add_test(NAME physical-bios-usb-runner-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/physical_bios_usb_runner_tests.py)
    set_tests_properties(physical-bios-usb-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
    add_test(NAME physical-dual-usb-runner-safety COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/physical_dual_usb_runner_tests.py)
    set_tests_properties(physical-dual-usb-runner-safety PROPERTIES LABELS "unit;writers;native" TIMEOUT 30)
  endif()
  add_executable(rufus_plus_plus_image_write_helper image_write_integration_helper.cpp)
  target_compile_features(rufus_plus_plus_image_write_helper PRIVATE cxx_std_17)
  target_include_directories(rufus_plus_plus_image_write_helper PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/src)
  target_link_libraries(rufus_plus_plus_image_write_helper PRIVATE RufusPlusPlus::PortableCore)
  if(MSVC)
    target_compile_options(rufus_plus_plus_image_write_helper PRIVATE /W4)
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_image_write_helper PRIVATE /WX)
    endif()
  else()
    target_compile_options(rufus_plus_plus_image_write_helper PRIVATE -Wall -Wextra -Wpedantic)
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_image_write_helper PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME native-file-image-write COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/image_write_integration.py
    --helper $<TARGET_FILE:rufus_plus_plus_image_write_helper>
    --report ${CMAKE_CURRENT_BINARY_DIR}/image-write-report.json)
  set_tests_properties(native-file-image-write PROPERTIES
    LABELS "integration;writers;image-file" TIMEOUT 180)
endif()

if(RUFUSPP_RUN_VIRTUAL_DISK_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(rufus_plus_plus_virtual_disk_helper virtual_disk_helper.cpp)
  target_compile_features(rufus_plus_plus_virtual_disk_helper PRIVATE cxx_std_17)
  target_include_directories(rufus_plus_plus_virtual_disk_helper PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/src)
  target_link_libraries(rufus_plus_plus_virtual_disk_helper PRIVATE RufusPlusPlus::PortableCore)
  if(MSVC)
    target_compile_options(rufus_plus_plus_virtual_disk_helper PRIVATE /W4)
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_virtual_disk_helper PRIVATE /WX)
    endif()
  else()
    target_compile_options(rufus_plus_plus_virtual_disk_helper PRIVATE -Wall -Wextra -Wpedantic)
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_virtual_disk_helper PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME native-virtual-disk COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/virtual_disk_integration.py
    --helper $<TARGET_FILE:rufus_plus_plus_virtual_disk_helper>)
  set_tests_properties(native-virtual-disk PROPERTIES
    LABELS "integration;writers;virtual-disk" SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

if(TARGET rufus_plus_plus_platform_backend)
  option(RUFUSPP_BUILD_PHYSICAL_USB_TEST
    "Build a manual-only physical USB qualification helper (never added to CTest)" OFF)
  if(RUFUSPP_BUILD_PHYSICAL_USB_TEST)
    if(NOT APPLE OR NOT RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
      message(FATAL_ERROR "Physical USB helper requires macOS unsigned-root development mode")
    endif()
    add_executable(rufus_plus_plus_physical_usb_helper physical_usb_helper.cpp)
    target_compile_features(rufus_plus_plus_physical_usb_helper PRIVATE cxx_std_17)
    target_link_libraries(rufus_plus_plus_physical_usb_helper PRIVATE RufusPlusPlus::PlatformBackend)
    target_compile_options(rufus_plus_plus_physical_usb_helper PRIVATE -Wall -Wextra -Wpedantic)
    if(RUFUSPP_STRICT_WARNINGS)
      target_compile_options(rufus_plus_plus_physical_usb_helper PRIVATE -Werror)
    endif()
  endif()
  add_executable(rufus_plus_plus_platform_backend_tests
    platform_backend_tests.cpp
  )

  target_compile_features(rufus_plus_plus_platform_backend_tests PRIVATE cxx_std_17)
  target_link_libraries(rufus_plus_plus_platform_backend_tests PRIVATE RufusPlusPlus::PlatformBackend)
  if(UNIX)
    rufuspp_bundle_ntfs_formatter(rufus_plus_plus_platform_backend_tests)
  endif()

  add_test(NAME platform-backend COMMAND rufus_plus_plus_platform_backend_tests)
endif()
