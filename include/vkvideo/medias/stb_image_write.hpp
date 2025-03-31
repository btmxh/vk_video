#pragma once

// stb_image_write API using FFmpeg

#include "vkvideo/core/types.hpp"
namespace vkvideo {
void write_img_grayscale(std::string_view filename, i32 width, i32 height,
                         const void *data);
} // namespace vkvideo
