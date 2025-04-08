#pragma once

#include "vkvideo/medias/stream.hpp"
#include "vkvideo/medias/video.hpp"

#include <vkfw/vkfw.hpp>
#include <vkvideo/context/display_mode.hpp>
#include <vkvideo/core/types.hpp>
#include <vkvideo/graphics/vk.hpp>
#include <vkvideo/medias/ffmpeg.hpp>

namespace vkvideo {

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
  ffmpeg::Rational fps;

  // audio-related properties

  /// \brief Sample rate (Hz, per channel)
  i32 sample_rate;

  /// \brief Channel layout (currently mono and stereo is supported)
  ffmpeg::ChannelLayout ch_layout;

  /// \brief Sample format
  ffmpeg::SampleFormat sample_format;

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
  HWAccel hwaccel = HWAccel::eAuto;
  DecodeMode mode = DecodeMode::eAuto;
};

/// \brief Application context, containing a preview window and Vulkan objects
class Context {
public:
  Context(ContextArgs &&args);

  void enable_hardware_acceleration(ffmpeg::CodecContext &cc);

  vkfw::Window get_window() { return window.get(); }
  VkContext &get_vulkan() { return vk; }

  std::unique_ptr<Video> open_video(std::string_view path,
                                    const VideoArgs &args = {});

  bool alive() const;
  void update();

private:
  ContextArgs args;
  vkfw::UniqueInstance instance;
  vkfw::UniqueWindow window;
  VkContext vk;
};
} // namespace vkvideo
