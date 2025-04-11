#pragma once

#include "vkvideo/core/types.hpp"
namespace vkvideo {
template <class Frame> class Media {
public:
  virtual ~Media() = default;

  Frame get_frame(i64 time) { return {}; }
  void seek(i64 time) {}
  std::optional<i32> get_num_frames() { return std::nullopt; }
  std::optional<i64> get_duration() { return std::nullopt; }
};

} // namespace vkvideo
