#include "spark/arena_allocator.h"

#include <cstdlib>
#include <new>

namespace spark {

ArenaAllocator::ArenaAllocator(std::size_t capacity) : buffer(nullptr), capacity(capacity), offset(0) {
  buffer = static_cast<unsigned char*>(std::malloc(capacity));
}

ArenaAllocator::~ArenaAllocator() {
  std::free(buffer);
}

void* ArenaAllocator::allocate(std::size_t bytes) {
  if (!buffer || offset + bytes > capacity) {
    return nullptr;
  }
  void* ptr = buffer + offset;
  offset += bytes;
  return ptr;
}

void ArenaAllocator::reset() {
  offset = 0;
}

} // namespace spark
