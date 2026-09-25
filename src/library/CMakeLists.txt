# The library index: SQLite plus FTS5, a folder scanner, and one file that
# includes TagLib.
#
# Portable on purpose, like src/audio: no CEF, no window, no operating system
# beyond std::filesystem. The macOS and Linux CI jobs build it and run its tests,
# which is what keeps the incremental-scan logic and the FTS query escaping from
# being things only Windows ever checks.

# vcpkg ships sqlite3 with an unofficial- config; a system SQLite is found by
# CMake's own module. Both are accepted so that this target also builds in a
# plain Linux container with libsqlite3-dev, which is where it gets verified
# before it ever reaches Windows.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(TARGET unofficial::sqlite3::sqlite3)
  set(SONORA_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  set(SONORA_SQLITE3_TARGET SQLite::SQLite3)
endif()

find_package(taglib CONFIG REQUIRED)

find_package(Threads REQUIRED)

add_library(sonora_library STATIC
  src/library.cpp
  src/scanner.cpp
  src/tag_reader_taglib.cpp)
add_library(sonora::library ALIAS sonora_library)

target_include_directories(sonora_library PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

# Both PRIVATE: nothing above this target sees sqlite3.h or a TagLib header, and
# the two headers this target installs name neither. That is not tidiness -- it
# is what makes the interface a seam the tests can replace.
#
# Threads is PUBLIC because this is a static library: the scanner's std::thread
# leaves undefined symbols for whoever links it, so the flag has to travel.
target_link_libraries(sonora_library
  PUBLIC
    Threads::Threads
  PRIVATE
    ${SONORA_SQLITE3_TARGET}
    TagLib::tag)

target_compile_features(sonora_library PUBLIC cxx_std_20)
