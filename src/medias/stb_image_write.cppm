module;

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

export module vkvideo.medias:stbi;

import std;
import vkvideo.third_party;
import vkvideo.core;
import :output;

export namespace vkvideo::medias::stbi {
void write_img(std::string_view filename, const tp::ffmpeg::Frame &frame) {
  auto format = tp::ffmpeg::guess_output_format({}, filename);
  if (!format)
    throw std::runtime_error("Unable to guess output format from filename");

  medias::OutputContext output{filename};
  auto codec = tp::ffmpeg::find_enc_codec(format->video_codec);
  if (!codec)
    throw std::runtime_error("Unable to find codec");

  tp::ffmpeg::PixelFormat *pix_fmts;
  tp::ffmpeg::av_call(avcodec_get_supported_config(
      nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
      const_cast<const void **>(reinterpret_cast<void **>(&pix_fmts)),
      nullptr));
  auto pix_fmt = avcodec_find_best_pix_fmt_of_list(
      pix_fmts, static_cast<tp::ffmpeg::PixelFormat>(frame->format), false,
      nullptr);

  auto &&[_, encoder] = output.add_stream(codec);
  encoder->width = frame->width;
  encoder->height = frame->height;
  encoder->pix_fmt = pix_fmt;
  encoder->time_base = {1, 25};
  encoder.open();
  output.init(0);

  output.begin();

  auto rescaled = tp::ffmpeg::Frame::create();
  rescaled->format = pix_fmt;
  rescaled->width = frame->width;
  rescaled->height = frame->height;

  tp::ffmpeg::VideoRescaler rescaler{};
  rescaler.auto_rescale(rescaled, frame);
  output.write_frame(rescaled, 0);

  output.end();
}

void write_img(std::string_view filename, i32 width, i32 height,
               std::span<const u8> data, tp::ffmpeg::PixelFormat data_format) {
  auto frame = tp::ffmpeg::Frame::create();
  frame->format = data_format;
  frame->width = width;
  frame->height = height;
  frame.get_buffer();
  std::memcpy(frame->data[0], data.data(), data.size_bytes());
  write_img(filename, frame);
}
} // namespace vkvideo::medias::stbi
