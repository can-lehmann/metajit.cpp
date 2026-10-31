#pragma once

// Copyright 2025 Can Joshua Lehmann
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <cstring>
#include <system_error>
#include <vector>

#include <sys/mman.h>

namespace metajit {
  class CodeHeap {
    struct Region {
      void* ptr;
      size_t size;
    };

    std::vector<Region> _regions;

  public:
    CodeHeap() = default;
    CodeHeap(const CodeHeap&) = delete;
    CodeHeap& operator=(const CodeHeap&) = delete;

    ~CodeHeap() {
      for (const Region& region : _regions) {
        munmap(region.ptr, region.size);
      }
    }

    void* alloc(const void* bytes, size_t size) {
      assert(size > 0);
      void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (ptr == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap");
      }
      memcpy(ptr, bytes, size);
      if (mprotect(ptr, size, PROT_READ | PROT_EXEC) == -1) {
        int error = errno;
        munmap(ptr, size);
        throw std::system_error(error, std::generic_category(), "mprotect");
      }
      try {
        _regions.push_back({ptr, size});
      } catch (...) {
        munmap(ptr, size);
        throw;
      }
      return ptr;
    }
  };
}
