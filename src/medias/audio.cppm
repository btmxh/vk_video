module;
#include <cassert>
export module vkvideo.medias:audio;

import vkvideo.core;
import vkvideo.third_party;
import :stream;

export namespace vkvideo::medias {

struct AudioFormat {
  tp::ffmpeg::SampleFormat sample_fmt;
  tp::ffmpeg::ChannelLayout ch_layout;
  i32 sample_rate;
};

class Audio {
public:
  Audio() = default;
  virtual ~Audio() = default;

  virtual void seek(i64 time) {}

  virtual i64 get_time() = 0;

  virtual i32 get_samples(i32 num_samples, u8 *const *data) = 0;
};

class AudioRAM;

class AudioStream : public Audio {
public:
  AudioStream(std::string_view path, AudioFormat format)
      : stream{RawFFmpegStream{path, tp::ffmpeg::MediaType::Audio},
               tp::ffmpeg::BufferRef{nullptr}, HWAccel::eOff},
        resampler{tp::ffmpeg::AudioResampler::create(
            format.ch_layout, format.sample_fmt, format.sample_rate,
            stream.get_decoder()->ch_layout, stream.get_decoder()->sample_fmt,
            stream.get_decoder()->sample_rate)} {}

  ~AudioStream() override = default;

  void seek(i64 time) override {
    stream.seek(time);
    resampler.recv(0, nullptr);
    assert(resampler.num_avail_samples() == 0);
  }

  i64 get_time() override { return next_pts - resampler.get_delay(); }

  i32 get_samples(i32 num_samples, u8 *const *data) override {
    decode_until(num_samples);
    return resampler.recv(num_samples, data);
  }

private:
  FFmpegStream stream;
  tp::ffmpeg::AudioResampler resampler;
  i64 next_pts;

  void decode_until(i32 num_samples) {
    auto frame = tp::ffmpeg::Frame::create();
    bool has_next = false;
    while (resampler.num_avail_samples() < num_samples) {
      std::tie(frame, has_next) = stream.next_frame(std::move(frame));
      if (!has_next) {
        break;
      }

      resampler.send(frame);
      next_pts = frame->pts + frame->duration;
    }
  }

  friend class AudioRAM;
};

class AudioRAM : public Audio {
public:
  AudioRAM(std::string_view path, AudioFormat format) {
    AudioStream stream{path, format};
    stream.decode_until(std::numeric_limits<i32>::max());
    frame = tp::ffmpeg::Frame::create();
    frame->format = format.sample_fmt;
    frame->nb_samples = stream.resampler.num_avail_samples();
    frame->sample_rate = format.sample_rate;
    *reinterpret_cast<tp::ffmpeg::ChannelLayout *>(&frame->ch_layout) =
        format.ch_layout;
    frame.get_buffer();
    frame->nb_samples = stream.get_samples(frame->nb_samples, frame->data);
  }

  void seek(i64 time) override {
    sample_idx = time * frame->sample_rate / static_cast<i64>(1e9);
  }

  i64 get_time() override {
    return sample_idx * static_cast<i64>(1e9) / frame->sample_rate;
  }

  i32 get_samples(i32 num_samples, u8 *const *data) override {
    num_samples = std::min(num_samples, frame->nb_samples - sample_idx);
    auto size = tp::ffmpeg::get_sample_fmt_size(sample_fmt());
    for (i32 i = 0; i < std::size(frame->data); ++i) {
      if (!data[i] || !frame->data[i])
        continue;
      std::copy(&frame->data[i][size * sample_idx],
                &frame->data[i][num_samples * sample_idx], data[i]);
    }

    sample_idx += num_samples;
    return num_samples;
  }

private:
  tp::ffmpeg::Frame frame;
  i32 sample_idx;

  tp::ffmpeg::SampleFormat sample_fmt() const {
    return static_cast<tp::ffmpeg::SampleFormat>(frame->format);
  }
};

} // namespace vkvideo::medias
