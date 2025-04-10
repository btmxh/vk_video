#pragma once

// stb_image_write API using FFmpeg

#include "vkvideo/core/types.hpp"
#include "vkvideo/medias/ffmpeg.hpp"

#include <span>

namespace vkvideo {
void write_img(std::string_view filename, i32 width, i32 height,
               std::span<u8> data, ffmpeg::PixelFormat data_format);
} // namespace vkvideo
