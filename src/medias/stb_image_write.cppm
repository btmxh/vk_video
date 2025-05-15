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

export namespace vkvideo::medias::stbi {
void write_img(std::string_view filename, i32 width, i32 height,
               std::span<const u8> data, tp::ffmpeg::PixelFormat data_format) {
  auto format = tp::ffmpeg::guess_output_format({}, filename);
  if (!format)
    throw std::runtime_error("Unable to guess output format from filename");

  auto muxer = tp::ffmpeg::OutputFormatContext::create(filename);

  auto codec = tp::ffmpeg::find_enc_codec(format->video_codec);
  if (!codec)
    throw std::runtime_error("Unable to find codec");

  tp::ffmpeg::PixelFormat *pix_fmts;
  tp::ffmpeg::av_call(avcodec_get_supported_config(
      nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
      const_cast<const void **>(reinterpret_cast<void **>(&pix_fmts)),
      nullptr));
  auto pix_fmt =
      avcodec_find_best_pix_fmt_of_list(pix_fmts, data_format, false, nullptr);

  auto encoder = tp::ffmpeg::CodecContext::create(codec);
  encoder->width = width;
  encoder->height = height;
  encoder->pix_fmt = pix_fmt;
  encoder->time_base = {1, 25};
  encoder.open();

  auto stream = muxer.add_stream(codec);
  encoder.copy_params_to(stream->codecpar);
  if (muxer->flags & AVFMT_GLOBALHEADER)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  muxer.begin();

  auto frame = tp::ffmpeg::Frame::create();
  frame->format = data_format;
  frame->width = width;
  frame->height = height;
  tp::ffmpeg::av_call(av_frame_get_buffer(frame.get(), 1));
  std::memcpy(frame->data[0], data.data(), data.size_bytes());

  auto rescaled = tp::ffmpeg::Frame::create();
  rescaled->format = pix_fmt;
  rescaled->width = width;
  rescaled->height = height;

  tp::ffmpeg::VideoRescaler rescaler{};
  rescaler.auto_rescale(rescaled, frame);

  std::array<tp::ffmpeg::Frame, 2> frames = {
      std::move(rescaled),
      nullptr, // flush frame
  };

  for (const auto &frame : frames) {
    encoder.send_frame(frame);
    while (true) {
      auto [packet, err] = encoder.recv_packet();
      if (err == tp::ffmpeg::RecvError::eSuccess)
        muxer.write_packet_interleaved(packet);
      else
        break;
    }
  }

  muxer.end();
}
} // namespace vkvideo::medias::stbi
