#pragma once

#include <cstddef>

namespace sonora::assets::embedded {

// The table is generated into the build directory by tools/embed_assets.py.
// When ui/dist does not exist the generator emits an empty table, so a checkout
// without a built UI still links -- it just serves nothing.
struct Entry {
  const char* path;  // URL path, leading '/'
  const unsigned char* data;
  std::size_t size;
  const char* mime_type;
};

extern const Entry kEntries[];
extern const std::size_t kEntryCount;

}  // namespace sonora::assets::embedded
