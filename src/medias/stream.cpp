#include "vkvideo/medias/stream.hpp"

#include "vkvideo/context/context.hpp"
#include "vkvideo/medias/ffmpeg.hpp"

#include <webp/demux.h>

#include <fstream>
#include <utility>

namespace vkvideo {
FFmpegStream::FFmpegStream(std::string_view path, Context &ctx,
                           ffmpeg::MediaType stream_type, HWAccel hwaccel) {
  demuxer = ffmpeg::InputFormatContext::open(path);
  demuxer.find_stream_info();

  const AVCodec *codec = nullptr;
  ffmpeg::av_call(stream_idx = av_find_best_stream(
                      demuxer.get(), static_cast<AVMediaType>(stream_type), -1,
                      -1, &codec, 0));

  decoder = ffmpeg::CodecContext::create(codec);
  decoder.copy_params_from(demuxer->streams[stream_idx]->codecpar);
  if (hwaccel == HWAccel::eOn) {
    if (stream_type == ffmpeg::MediaType::Video) {
      ctx.enable_hardware_acceleration(decoder);
    } else {
      std::cerr << "WARNING: Hardware acceleration is only supported for "
                   "video streams\n";
    }
  }
  decoder.open();

  current_packet = ffmpeg::Packet::create();
}

std::pair<ffmpeg::Frame, bool> FFmpegStream::next_frame(ffmpeg::Frame &&frame) {
  auto rescale_pts = [&](i64 &pts) {
    pts = ffmpeg::rescale_to_ns(pts, demuxer->streams[stream_idx]->time_base);
  };

  while (true) {
    ffmpeg::RecvError err;
    std::tie(frame, err) = decoder.recv_frame(std::move(frame));
    switch (err) {
    case ffmpeg::RecvError::eSuccess:
      rescale_pts(frame->pts);
      rescale_pts(frame->duration);
      return {std::move(frame), true};
    case ffmpeg::RecvError::eAgain:
      break;
    case ffmpeg::RecvError::eEof:
      return {std::move(frame), false};
    }

    while (true) {
      std::tie(current_packet, err) =
          demuxer.read_packet(std::move(current_packet));
      if (err != ffmpeg::RecvError::eSuccess ||
          current_packet->stream_index == stream_idx) {
        break;
      }
    }
    assert(err != ffmpeg::RecvError::eAgain);
    if (err == ffmpeg::RecvError::eEof &&
        std::exchange(reach_eof_packet, true)) {
      return {std::move(frame), false};
    }

    decoder.send_packet(current_packet);
  }
}

bool FFmpegStream::seek(i64 pos) {
  pos /= 1000000000 / AV_TIME_BASE;
  ffmpeg::av_call(av_seek_frame(demuxer.get(), -1, pos, AVSEEK_FLAG_BACKWARD));
  return true;
}

std::optional<i32> FFmpegStream::get_num_frames() {
  i32 nb_frames = demuxer->streams[stream_idx]->nb_frames;
  if (nb_frames == 0)
    return std::nullopt;
  return nb_frames;
}

std::optional<i64> FFmpegStream::get_duration() {
  // TODO: this is not entirely accurate
  return ffmpeg::rescale_to_ns(demuxer->streams[stream_idx]->duration,
                               demuxer->streams[stream_idx]->time_base);
}

static std::vector<u8> read_file_bytes(std::string_view path) {
  std::ifstream in{path.data(), std::ios::binary};
  return std::vector<u8>{
      std::istreambuf_iterator<char>{in},
      std::istreambuf_iterator<char>{},
  };
}

AnimWebPStream::AnimWebPStream(std::string_view path) {
  data = read_file_bytes(path);
  data_ptr.bytes = data.data();
  data_ptr.size = data.size();
  decoder.reset(WebPAnimDecoderNew(&data_ptr, nullptr));
  if (!WebPAnimDecoderGetInfo(decoder.get(), &info)) {
    throw std::runtime_error{"Unable to get decoding info"};
  }
}

bool AnimWebPStream::seek(i64 time) {
  WebPAnimDecoderReset(decoder.get());
  last_pts = 0;
  return true;
}

std::pair<ffmpeg::Frame, bool>
AnimWebPStream::next_frame(ffmpeg::Frame &&frame) {
  if (!WebPAnimDecoderHasMoreFrames(decoder.get())) {
    // if we decoded all frame, we know for sure that the total duration
    // is the last PTS value
    if (duration.has_value())
      assert(duration.value() == last_pts);
    duration = last_pts;
    return {std::move(frame), false};
  }

  u8 *data;
  int pts;
  if (!WebPAnimDecoderGetNext(decoder.get(), &data, &pts)) {
    throw std::runtime_error{"Decoding error"};
  }

  i64 pts_i64 = static_cast<i64>(pts) * static_cast<i64>(1e6);

  if (!frame)
    frame = ffmpeg::Frame::create();

  frame->width = info.canvas_width;
  frame->height = info.canvas_height;
  frame->duration = pts_i64 - last_pts;
  frame->pts = std::exchange(last_pts, pts_i64);
  frame->format = ffmpeg::PixelFormat::AV_PIX_FMT_RGBA;

  av_frame_get_buffer(frame.get(), 1);
  memcpy(frame->data[0], data, info.canvas_width * info.canvas_height * 4);

  return {std::move(frame), true};
}

std::optional<i32> AnimWebPStream::get_num_frames() { return info.frame_count; }

std::optional<i64> AnimWebPStream::get_duration() { return duration; }

} // namespace vkvideo
