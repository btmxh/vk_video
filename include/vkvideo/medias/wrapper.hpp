#pragma once

#include <vkvideo/core/types.hpp>

#include <span>

extern "C" {
#include <libavutil/buffer.h>
}

namespace vkvideo {
class BufferRef {
public:
  BufferRef(AVBufferRef *ref);
  ~BufferRef();

  BufferRef(const BufferRef &other);
  BufferRef &operator=(const BufferRef &other);

  void reset(AVBufferRef *ref = nullptr);
  std::span<u8> data();
  std::span<const u8> data() const;

  AVBufferRef *get() const;

private:
  AVBufferRef *ref;
};
}; // namespace vkvideo
