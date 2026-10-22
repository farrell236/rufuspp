# Build only the library and formatter, with no host installation or FUSE driver.
enable_language(C)
include(ExternalProject)

set(ntfs_archive "${CMAKE_CURRENT_SOURCE_DIR}/ntfs-3g_ntfsprogs-2026.9.28.tgz")
file(SHA256 "${ntfs_archive}" ntfs_sha256)
if(NOT ntfs_sha256 STREQUAL "350d9415c59f7c3fa74e23985ad2d56423c6a168337368f672e2e362d8f295c8")
  message(FATAL_ERROR "Bundled NTFS-3G archive checksum mismatch")
endif()
set(ntfs_source_root "${CMAKE_CURRENT_BINARY_DIR}/source")
set(ntfs_source "${ntfs_source_root}/ntfs-3g-2026.9.28")
if(NOT EXISTS "${ntfs_source}/configure")
  file(ARCHIVE_EXTRACT INPUT "${ntfs_archive}" DESTINATION "${ntfs_source_root}")
endif()
# Upstream requires pkg-config even when both consumers (FUSE and crypto) are
# disabled. Make it optional only in that configuration; no library code changes.
file(READ "${ntfs_source}/configure" ntfs_configure)
string(REPLACE "if test -z \"$PKG_CONFIG\"; then\n\tas_fn_error $? \"pkg-config not found\""
  "if test -z \"$PKG_CONFIG\" && { test \"$enable_ntfs_3g\" != no || test \"$enable_crypto\" != no; }; then\n\tas_fn_error $? \"pkg-config not found\""
  ntfs_configure "${ntfs_configure}")
file(WRITE "${ntfs_source}/configure" "${ntfs_configure}")
set(ntfs_build "${CMAKE_CURRENT_BINARY_DIR}/build")
find_program(NTFS_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
set(ntfs_cflags "${CMAKE_C_FLAGS} -O2 -fPIC")
set(ntfs_ldflags "${CMAKE_EXE_LINKER_FLAGS}")
if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES ntfs_arch_count)
    if(ntfs_arch_count GREATER 1)
      message(FATAL_ERROR "Build each macOS architecture separately before combining Rufus++ packages")
    endif()
    string(APPEND ntfs_cflags " -arch ${CMAKE_OSX_ARCHITECTURES}")
    string(APPEND ntfs_ldflags " -arch ${CMAKE_OSX_ARCHITECTURES}")
  endif()
  if(CMAKE_OSX_SYSROOT)
    string(APPEND ntfs_cflags " -isysroot ${CMAKE_OSX_SYSROOT}")
    string(APPEND ntfs_ldflags " -isysroot ${CMAKE_OSX_SYSROOT}")
  endif()
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    string(APPEND ntfs_cflags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    string(APPEND ntfs_ldflags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
endif()
ExternalProject_Add(rufus_ntfs_upstream
  SOURCE_DIR "${ntfs_source}"
  BINARY_DIR "${ntfs_build}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
    "CC=${CMAKE_C_COMPILER}" "CFLAGS=${ntfs_cflags}" "LDFLAGS=${ntfs_ldflags}"
    # A host gettext header can remap libc setlocale without linking libintl.
    # The private formatter uses no gettext functions or translations.
    "ac_cv_header_libintl_h=no"
    <SOURCE_DIR>/configure --disable-shared --enable-static --disable-ntfs-3g
    --disable-crypto --disable-plugins --disable-nfconv --disable-ldconfig
    --disable-mount-helper --without-hd --without-uuid
  BUILD_COMMAND ${NTFS_MAKE_EXECUTABLE} -C <BINARY_DIR>/libntfs-3g -j4
    COMMAND ${NTFS_MAKE_EXECUTABLE} -C <BINARY_DIR>/ntfsprogs -j4 mkntfs
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${ntfs_build}/libntfs-3g/.libs/libntfs-3g.a"
                   "${ntfs_build}/ntfsprogs/mkntfs"
  LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_OUTPUT_ON_FAILURE TRUE
)
add_library(rufus_bundled_ntfs STATIC IMPORTED GLOBAL)
set_target_properties(rufus_bundled_ntfs PROPERTIES
  IMPORTED_LOCATION "${ntfs_build}/libntfs-3g/.libs/libntfs-3g.a"
  INTERFACE_INCLUDE_DIRECTORIES "${ntfs_source}/include/ntfs-3g"
)
add_dependencies(rufus_bundled_ntfs rufus_ntfs_upstream)
add_library(RufusPlusPlus::BundledNtfs ALIAS rufus_bundled_ntfs)

function(rufuspp_bundle_ntfs_formatter target)
  add_dependencies(${target} rufus_ntfs_upstream)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${RUFUSPP_NTFS_FORMATTER}" "$<TARGET_FILE_DIR:${target}>/rufus-mkntfs"
    VERBATIM
  )
endfunction()
# Function callers may be in sibling directories.
set(RUFUSPP_NTFS_FORMATTER "${ntfs_build}/ntfsprogs/mkntfs" CACHE INTERNAL "Bundled NTFS formatter")
set(RUFUSPP_NTFS_BUILD_DIR "${ntfs_build}" CACHE INTERNAL "Bundled NTFS configuration headers")
