#include "vkvideo/medias/wrapper.hpp"

namespace vkvideo {
BufferRef::BufferRef(AVBufferRef *ref) : ref(ref) {}

BufferRef::~BufferRef() { av_buffer_unref(&ref); }

BufferRef::BufferRef(const BufferRef &other) : ref(av_buffer_ref(other.ref)) {}

BufferRef &BufferRef::operator=(const BufferRef &other) {
  reset(av_buffer_ref(other.ref));
  return *this;
}

void BufferRef::reset(AVBufferRef *ref) {
  av_buffer_unref(&this->ref);
  this->ref = ref;
}

std::span<u8> BufferRef::data() { return std::span<u8>{ref->data, ref->size}; }

std::span<const u8> BufferRef::data() const {
  return std::span<const u8>{ref->data, ref->size};
}

AVBufferRef *BufferRef::get() const { return ref; }
} // namespace vkvideo
