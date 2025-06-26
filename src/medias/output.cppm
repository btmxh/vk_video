export module vkvideo.medias:output;

import vkvideo.core;
import vkvideo.third_party;
import std;

export namespace vkvideo::medias {
class OutputContext {
public:
  OutputContext(std::string_view path)
      : muxer{tp::ffmpeg::OutputFormatContext::create(path)},
        flush_packet{tp::ffmpeg::Packet::create()} {}

  std::pair<tp::ffmpeg::Stream &, tp::ffmpeg::CodecContext &>
  add_stream(tp::ffmpeg::Codec codec) {
    auto &stream = *muxer.add_stream(codec);
    auto &encoder =
        encoders.emplace_back(tp::ffmpeg::CodecContext::create(codec));
    return {stream, encoder};
  }

  void init(i32 stream_idx) {
    if (muxer.has_global_header_flag())
      encoders[stream_idx].add_global_header_flag();
    encoders[stream_idx].open();

    muxer->streams[stream_idx]->time_base = encoders[stream_idx]->time_base;
    muxer->streams[stream_idx]->avg_frame_rate =
        encoders[stream_idx]->framerate;
    encoders[stream_idx].copy_params_to(muxer->streams[stream_idx]->codecpar);
  }

  void begin() { muxer.begin(); }

  void end() {
    for (i32 i = 0; i < static_cast<i32>(encoders.size()); ++i) {
      write_frame(nullptr, i);
      flush_packets(i);
    }

    muxer.end();

    // after this, the object is in an invalid state
  }

  void flush_packets(i32 stream_idx) {
    auto &enc = encoders[stream_idx];
    tp::ffmpeg::RecvError err;
    while (true) {
      std::tie(flush_packet, err) = enc.recv_packet(std::move(flush_packet));
      if (err != tp::ffmpeg::RecvError::eSuccess)
        break;
      flush_packet.rescale_ts(enc->time_base,
                              muxer->streams[stream_idx]->time_base);
      flush_packet->stream_index = stream_idx;
      muxer.write_packet_interleaved(flush_packet);
    }

    flush_packet.unref();
  }

  void write_frame(const tp::ffmpeg::Frame &frame, i32 stream_idx) {
    do {
      flush_packets(stream_idx);
    } while (!encoders[stream_idx].send_frame(frame));
    flush_packets(stream_idx);
  }

private:
  tp::ffmpeg::OutputFormatContext muxer;
  tp::ffmpeg::Packet flush_packet;
  std::vector<tp::ffmpeg::CodecContext> encoders;
};
} // namespace vkvideo::medias
