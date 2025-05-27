export module vkvideo.context:context;

import std;
import vkfw;
import vkvideo.core;
import vkvideo.graphics;
import vkvideo.medias;
import vkvideo.third_party;

export namespace vkvideo::context {

enum class DisplayMode {
  Preview,
  Render,
};

class Context;
class CodecContext;

/// \brief Project setting arguments
struct ContextArgs {
  /// \brief Display mode (preview or render)
  ///
  /// Preview mode will show a preview window, while render mode will just
  /// render the media file headlessly
  DisplayMode mode;

  // video-related properties

  /// \brief Video dimensions of the output media
  i32 width, height;

  /// \brief Video frames per second
  tp::ffmpeg::Rational fps;

  // audio-related properties

  /// \brief Sample rate (Hz, per channel)
  i32 sample_rate;

  /// \brief Channel layout (currently mono and stereo is supported)
  tp::ffmpeg::ChannelLayout ch_layout;

  /// \brief Sample format
  tp::ffmpeg::SampleFormat sample_format;

  /// \brief Path of the output media file
  ///
  /// This will be used as preview window title in preview mode
  std::string render_output;

private:
  friend class Context;
  vkfw::InitHints init_hint() const;
  vkfw::WindowHints window_hint() const;
};

enum class DecoderType {
  eAuto = 0, // libwebp if is webp file, ffmpeg otherwise
  eFFmpeg,
  eLibWebP,
};

enum class DecodeMode {
  eAuto = 0,
  eStream,  // stream the file (for large media files)
  eReadAll, // read everything to RAM/VRAM (for small media clips)
};

struct VideoArgs {
  DecoderType type = DecoderType::eAuto;
  medias::HWAccel hwaccel = medias::HWAccel::eAuto;
  DecodeMode mode = DecodeMode::eAuto;
};

/// \brief Application context, containing a preview window and Vulkan objects
class Context {
public:
  Context(ContextArgs &&args)
      : args{std::move(args)}, instance{vkfw::initUnique(args.init_hint())},
        window{vkfw::createWindowUnique(static_cast<std::size_t>(args.width),
                                        static_cast<std::size_t>(args.height),
                                        args.render_output.c_str(),
                                        args.window_hint())},
        vk{window.get()} {
    window->callbacks()->on_key = [this](auto, vkfw::Key key, auto,
                                         vkfw::KeyAction action, auto) {
      if (action == vkfw::KeyAction::ePress)
        return;
      static constexpr i64 seek_amount = 1e9;
      std::lock_guard lock{audio_mutex};
      switch (key) {
      case vkfw::Key::eLeft:
        master_clock.seek(-std::min(seek_amount, master_clock.get_time()));
        audio->seek(master_clock.get_time());
        break;
      case vkfw::Key::eRight:
        master_clock.seek(seek_amount);
        audio->seek(master_clock.get_time());
        break;
      case vkfw::Key::eSpace:
        master_clock.is_paused() ? master_clock.play() : master_clock.pause();
        break;
      default:
        return;
      }
    };
  }

  vkfw::Window get_window() { return window.get(); }
  graphics::VkContext &get_vulkan() { return vk; }

  std::unique_ptr<medias::Video> open_video(std::string_view path,
                                            const VideoArgs &args = {});

  bool alive() const;
  void update();

  i64 get_time() { return master_clock.get_time(); }

  void open_audio(std::string_view path) {
    std::lock_guard lock{audio_mutex};
    audio = std::make_unique<medias::AudioStream>(
        path, medias::AudioFormat{
                  .sample_fmt = args.sample_format,
                  .ch_layout = args.ch_layout,
                  .sample_rate = args.sample_rate,
              });
    auto &dev = tp::portaudio::System::instance().defaultOutputDevice();
    audio_stream =
        std::make_unique<tp::portaudio::MemFunCallbackStream<Context>>(
            tp::portaudio::StreamParameters{
                tp::portaudio::DirectionSpecificStreamParameters::null(),
                tp::portaudio::DirectionSpecificStreamParameters{
                    dev,
                    args.ch_layout->nb_channels,
                    [this]() {
                      switch (args.sample_format) {
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_U8:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_U8P:
                        return tp::portaudio::SampleDataFormat::UINT8;
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S16:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S16P:
                        return tp::portaudio::SampleDataFormat::INT16;
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S32:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S32P:
                        return tp::portaudio::SampleDataFormat::INT32;
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_FLT:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_FLTP:
                        return tp::portaudio::SampleDataFormat::FLOAT32;
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_DBL:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_DBLP:
                        throw std::runtime_error{
                            "AV_SAMPLE_FMT_DBL* is not supported"};
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S64:
                      case tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_S64P:
                        throw std::runtime_error{
                            "AV_SAMPLE_FMT_S64* is not supported"};
                      default:
                        throw std::runtime_error{"Invalid sample format"};
                        break;
                      }
                    }(),
                    tp::ffmpeg::sample_fmt_is_interleaved(args.sample_format),
                    dev.defaultLowOutputLatency(),
                    nullptr,
                },
                static_cast<double>(args.sample_rate),
                1024,
                0,
            },
            *this, &Context::audio_callback);
    audio_stream->start();
  }

private:
  ContextArgs args;
  vkfw::UniqueInstance instance;
  vkfw::UniqueWindow window;
  graphics::VkContext vk;

  tp::portaudio::AutoSystem audio_sys;
  std::mutex audio_mutex;
  std::unique_ptr<medias::Audio> audio;
  std::unique_ptr<tp::portaudio::Stream> audio_stream;

  SteadyClock master_clock;

  void fill_silence(void *out, i32 offset, i32 num_samples) {
    bool interleaved =
        tp::ffmpeg::sample_fmt_is_interleaved(args.sample_format);
    i32 sample_size = (interleaved ? args.ch_layout->nb_channels : 1) *
                      tp::ffmpeg::get_sample_fmt_size(args.sample_format);
    u8 zero_byte =
        args.sample_format == tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_U8 ||
                args.sample_format ==
                    tp::ffmpeg::SampleFormat::AV_SAMPLE_FMT_U8P
            ? 0x80
            : 0;
    if (interleaved) {
      std::memset(reinterpret_cast<u8 *>(out) + sample_size * offset, zero_byte,
                  sample_size * num_samples);
    } else {
      u8 *const *ptr = reinterpret_cast<u8 *const *>(out);
      for (i32 i = 0; i < args.ch_layout->nb_channels; ++i) {
        std::memset(ptr[i] + sample_size * offset, zero_byte,
                    sample_size * num_samples);
      }
    }
  }

  int audio_callback(const void *, void *out, unsigned long num_frames,
                     const tp::portaudio::PaStreamCallbackTimeInfo *time_info,
                     tp::portaudio::PaStreamCallbackFlags) {
    std::lock_guard lock{audio_mutex};
    u8 *u8_out = static_cast<u8 *>(out);
    auto out_samples = tp::ffmpeg::sample_fmt_is_interleaved(args.sample_format)
                           ? &u8_out
                           : static_cast<u8 *const *>(out);
    i32 offset = 0;
    if (audio && !master_clock.is_paused()) {
      auto out_delay = time_info->outputBufferDacTime - time_info->currentTime;
      auto master_out_time = master_clock.get_time() + out_delay;
      auto audio_out_time = audio->get_time();
      auto sync_delay = master_out_time - audio_out_time;
      std::println("Master clock: {:.3f}, Audio clock: {:.3f}, delay {:.3f}",
                   master_out_time * 1e-9, audio_out_time * 1e-9,
                   sync_delay * 1e-9);
      offset = audio->get_samples(num_frames, out_samples);
    }

    fill_silence(out, offset, num_frames - offset);
    return 0;
  }
};
} // namespace vkvideo::context

namespace vkvideo::context {
vkfw::InitHints ContextArgs::init_hint() const {
  auto force_x11 = []() {
    auto env = std::getenv("VKVIDEO_FORCE_X11");
    return env && std::strcmp(env, "1") == 0;
  }();
  return vkfw::InitHints{
      .platform =
          mode == DisplayMode::Preview
              ? (force_x11 ? vkfw::Platform::eX11 : vkfw::Platform::eAny)
              : vkfw::Platform::eNull,
  };
}

vkfw::WindowHints ContextArgs::window_hint() const { return {}; }

static bool is_webp_path(std::string_view path) {
  std::ifstream file(path.data(), std::ios::binary);
  if (!file) {
    return false;
  }

  std::array<unsigned char, 12> header{};
  if (!file.read(reinterpret_cast<char *>(header.data()), header.size())) {
    return false;
  }

  // Check for RIFF + 4 unknown bytes + WEBPVP8
  return header[0] == 0x52 && header[1] == 0x49 && header[2] == 0x46 &&
         header[3] == 0x46 && header[8] == 0x57 && header[9] == 0x45 &&
         header[10] == 0x42 && header[11] == 0x50;
}

struct warn_t {
  template <class... Args>
  void operator()(std::format_string<Args...> fmt, Args &&...args) {
    std::cerr << "WARN: "
              << std::format(fmt, std::forward<decltype(args)>(args)...)
              << std::endl;
  }
};

std::unique_ptr<medias::Video> Context::open_video(std::string_view path,
                                                   const VideoArgs &args) {
  warn_t warn;
  DecoderType type = args.type;
  if (type == DecoderType::eAuto) {
    type = is_webp_path(path) ? DecoderType::eLibWebP : DecoderType::eFFmpeg;
  }

  DecodeMode mode = args.mode;
  // 16 MiB
  constexpr static std::size_t READ_ALL_THRESHOLD = std::size_t{16} << 20;

  std::unique_ptr<medias::Stream> stream;
  // TODO: respect the VKVIDEO_HAVE_WEBP flag
  switch (type) {
  case DecoderType::eFFmpeg: {
    medias::RawFFmpegStream raw_ffmpeg_stream{path,
                                              tp::ffmpeg::MediaType::Video};
    if (mode == DecodeMode::eAuto) {
      mode = raw_ffmpeg_stream.est_vram_bytes().value_or(
                 std::numeric_limits<std::size_t>::max()) <= READ_ALL_THRESHOLD
                 ? DecodeMode::eReadAll
                 : DecodeMode::eStream;
    }

    auto hwaccel = args.hwaccel;
    if (mode == DecodeMode::eReadAll) {
      if (hwaccel == medias::HWAccel::eOn)
        warn("Hardware-acceleration is not supported for read-all decode "
             "mode");
      hwaccel = medias::HWAccel::eOff;
    } else {
      if (hwaccel == medias::HWAccel::eAuto)
        hwaccel = medias::HWAccel::eOn;
    }

    stream = std::make_unique<medias::FFmpegStream>(
        std::move(raw_ffmpeg_stream), get_vulkan().get_hwaccel_ctx(), hwaccel);
    break;
  }
  case DecoderType::eLibWebP: {
    std::ifstream input{path.data(), std::ios::binary};
    input.exceptions(std::ios::failbit | std::ios::badbit);
    std::vector<char> webp_data(std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{});
    if (mode == DecodeMode::eAuto) {
      tp::webp::Demuxer demuxer{std::span<const u8>{
          reinterpret_cast<const u8 *>(webp_data.data()),
          reinterpret_cast<const u8 *>(webp_data.data() + webp_data.size())}};
      // currently we are not handling anything special with non-RGBA formats
      auto est_bytes = static_cast<std::size_t>(demuxer.num_frames()) *
                       demuxer.width() * demuxer.height() * 4;
      mode = est_bytes <= READ_ALL_THRESHOLD ? DecodeMode::eReadAll
                                             : DecodeMode::eStream;
    }

    if (args.hwaccel == medias::HWAccel::eOn) {
      warn("Hardware acceleration is not supported for libwebp decoder");
    }

    stream = std::make_unique<medias::AnimWebPStream>(std::move(webp_data));
    break;
  }
  default:
    throw std::runtime_error{"Invalid decoder type"};
  }

  switch (mode) {
  case DecodeMode::eStream:
    return std::make_unique<medias::VideoStream>(std::move(stream), vk);
  case DecodeMode::eReadAll:
    return std::make_unique<medias::VideoVRAM>(*stream, vk);
  default:;
  }

  throw std::runtime_error{"Invalid decode mode"};
}

bool Context::alive() const { return !window->shouldClose(); }

void Context::update() {
  vkfw::pollEvents();
  vk.get_temp_pools().garbage_collect();
}
} // namespace vkvideo::context
