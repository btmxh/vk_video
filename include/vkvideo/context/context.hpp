#pragma once

#include "channellayout.h"
#include "codeccontext.h"
#include "rational.h"
#include "sampleformat.h"
#include "stream.h"

#include <vkfw/vkfw.hpp>

#include <vkvideo/context/display_mode.hpp>
#include <vkvideo/core/types.hpp>
#include <vkvideo/graphics/vk.hpp>

namespace vkvideo {

class Context;

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
  av::Rational fps;

  // audio-related properties

  /// \brief Sample rate (Hz, per channel)
  i32 sample_rate;

  /// \brief Channel layout (currently mono and stereo is supported)
  av::ChannelLayout ch_layout;

  /// \brief Sample format
  av::SampleFormat sample_format;

  /// \brief Path of the output media file
  ///
  /// This will be used as preview window title in preview mode
  std::string render_output;

private:
  friend class Context;
  vkfw::InitHints init_hint() const;
  vkfw::WindowHints window_hint() const;
};

/// \brief Application context, containing a preview window and Vulkan objects
class Context {
public:
  Context(ContextArgs &&args);

  template <class T, av::Direction dir>
  void enable_hardware_acceleration(av::VideoCodecContext<T, dir> &ctx) {
    vk.enable_hardware_acceleration(ctx);
    ctx.raw()->get_format = [](auto...) { return AV_PIX_FMT_VULKAN; };
    ctx.raw()->pix_fmt = AV_PIX_FMT_VULKAN;
  }

  VkContext &get_vulkan() { return vk; }

private:
  ContextArgs args;
  vkfw::UniqueInstance instance;
  vkfw::UniqueWindow window;
  VkContext vk;
};
} // namespace vkvideo
