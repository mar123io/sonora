# The updater's brain: manifest, signature, archive format, patch, journal.
#
# Portable, like sonora_core and sonora_state, and for the same reason turned up
# to its logical conclusion. This is the one subsystem in Sonora that can destroy
# an installation, so every decision it makes is a pure function of values it was
# handed -- NextStep takes a journal and four booleans, ChooseUpdate takes a
# manifest and a version, ApplyPatch takes bytes -- and the Windows half is six
# file operations with no policy in them. The macOS and Linux CI jobs build this
# target and run its tests, which is what keeps that true.
#
# It links two libraries and no others: libsodium for the signature and the hash,
# zstd for the patch. Neither appears in a public header.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  set(SONORA_SODIUM_TARGET unofficial-sodium::sodium)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SONORA_SODIUM_PC QUIET IMPORTED_TARGET libsodium)
  endif()
  if(TARGET PkgConfig::SONORA_SODIUM_PC)
    set(SONORA_SODIUM_TARGET PkgConfig::SONORA_SODIUM_PC)
  else()
    message(FATAL_ERROR
      "libsodium not found. It is the only cryptography in this project and there "
      "is no fallback: ADR 0009 says this repository implements no primitive of "
      "its own, so a build without it is a build with no signature verification.")
  endif()
endif()

find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
  set(SONORA_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
  set(SONORA_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
  set(SONORA_ZSTD_TARGET zstd::libzstd)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SONORA_ZSTD_PC QUIET IMPORTED_TARGET libzstd)
  endif()
  if(TARGET PkgConfig::SONORA_ZSTD_PC)
    set(SONORA_ZSTD_TARGET PkgConfig::SONORA_ZSTD_PC)
  else()
    message(FATAL_ERROR "libzstd not found; it is what --patch-from is made of.")
  endif()
endif()

find_package(nlohmann_json CONFIG REQUIRED)

add_library(sonora_update STATIC
  src/archive.cpp
  src/driver.cpp
  src/filestore.cpp
  src/hash.cpp
  src/journal.cpp
  src/manifest.cpp
  src/patch.cpp
  src/signature.cpp
  src/updater.cpp
  src/version.cpp)
add_library(sonora::update ALIAS sonora_update)

target_include_directories(sonora_update PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

# All PRIVATE: no header in include/ mentions sodium.h, zstd.h or json.hpp. The
# hash is an array of bytes, the patch is a vector of bytes, and the manifest is a
# struct -- so a change of any of the three is a change to one .cpp file.
target_link_libraries(sonora_update PRIVATE
  ${SONORA_SODIUM_TARGET}
  ${SONORA_ZSTD_TARGET}
  nlohmann_json::nlohmann_json)

target_compile_features(sonora_update PUBLIC cxx_std_20)
