#pragma once

#include <cstddef>

namespace spark {

class ArenaAllocator {
 public:
  explicit ArenaAllocator(std::size_t capacity = 1024);
  ~ArenaAllocator();

  void* allocate(std::size_t bytes);
  void reset();

 private:
  unsigned char* buffer;
  std::size_t capacity;
  std::size_t offset;
};

} // namespace spark
