#include "vkvideo/medias/stb_image_write.hpp"

#include "vkvideo/medias/ffmpeg.hpp"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

namespace vkvideo {
void write_img(std::string_view filename, i32 width, i32 height,
               std::span<u8> data, ffmpeg::PixelFormat data_format) {
  auto format = ffmpeg::guess_output_format({}, filename);
  if (!format)
    throw std::runtime_error("Unable to guess output format from filename");

  auto muxer = ffmpeg::OutputFormatContext::create(filename);

  auto codec = ffmpeg::find_enc_codec(format->video_codec);
  if (!codec)
    throw std::runtime_error("Unable to find codec");

  AVPixelFormat *pix_fmts;
  ffmpeg::av_call(avcodec_get_supported_config(
      nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
      const_cast<const void **>(reinterpret_cast<void **>(&pix_fmts)),
      nullptr));
  auto pix_fmt =
      avcodec_find_best_pix_fmt_of_list(pix_fmts, data_format, false, nullptr);

  auto encoder = ffmpeg::CodecContext::create(codec);
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

  auto frame = ffmpeg::Frame::create();
  frame->format = data_format;
  frame->width = width;
  frame->height = height;
  ffmpeg::av_call(av_frame_get_buffer(frame.get(), 1));
  std::memcpy(frame->data[0], data.data(), data.size_bytes());

  auto rescaled = ffmpeg::Frame::create();
  rescaled->format = pix_fmt;
  rescaled->width = width;
  rescaled->height = height;

  ffmpeg::VideoRescaler rescaler{};
  rescaler.auto_rescale(rescaled, frame);

  std::array<ffmpeg::Frame, 2> frames = {
      std::move(rescaled),
      nullptr, // flush frame
  };

  for (const auto &frame : frames) {
    encoder.send_frame(frame);
    while (true) {
      auto [packet, err] = encoder.recv_packet();
      if (err == ffmpeg::RecvError::eSuccess)
        muxer.write_packet_interleaved(packet);
      else
        break;
    }
  }

  muxer.end();
}
} // namespace vkvideo
