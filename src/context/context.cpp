#include "vkvideo/context/context.hpp"

#include "vkfw/vkfw.hpp"
#include "vkvideo/medias/stream.hpp"
#include "vkvideo/medias/video.hpp"

#include <fstream>
#include <utility>

namespace vkvideo {
vkfw::InitHints ContextArgs::init_hint() const {
  return vkfw::InitHints{
      .platform = mode == DisplayMode::Preview ? vkfw::Platform::eX11
                                               : vkfw::Platform::eNull,
  };
}

vkfw::WindowHints ContextArgs::window_hint() const { return {}; }

Context::Context(ContextArgs &&args)
    : args{std::move(args)}, instance{vkfw::initUnique(args.init_hint())},
      window{vkfw::createWindowUnique(
          static_cast<size_t>(args.width), static_cast<size_t>(args.height),
          args.render_output.c_str(), args.window_hint())},
      vk{window.get()} {}

void Context::enable_hardware_acceleration(ffmpeg::CodecContext &ctx) {
  vk.enable_hardware_acceleration(ctx);
  ctx->get_format = [](auto...) { return AV_PIX_FMT_VULKAN; };
  ctx->pix_fmt = AV_PIX_FMT_VULKAN;
}

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

static std::unique_ptr<Stream>
open_video_stream(Context &c, std::string_view path, DecoderType type) {
  if (type == DecoderType::eAuto) {
    type = is_webp_path(path) ? DecoderType::eLibWebP : DecoderType::eFFmpeg;
  }

  switch (type) {
  case DecoderType::eFFmpeg:
    return std::make_unique<FFmpegStream>(path, c);
  case DecoderType::eLibWebP:
    return std::make_unique<AnimWebPStream>(path);
  default:;
  }

  throw std::runtime_error{"Invalid decoder type"};
}

std::unique_ptr<Video> Context::open_video(std::string_view path,
                                           const VideoArgs &args) {
  auto stream = open_video_stream(*this, path, args.type);
  auto mode = args.mode;
  if (mode == DecodeMode::eAuto) {
    mode = DecodeMode::eStream;

    auto num_frames_est = stream->get_num_frames();
    if (num_frames_est.has_value() && num_frames_est.value() < 64) {
      mode = DecodeMode::eReadAll;
    }
  }

  switch (mode) {
  case DecodeMode::eStream:
    return std::make_unique<VideoStream>(std::move(stream));
  case DecodeMode::eReadAll:
    return std::make_unique<VideoVRAM>(*stream, *this);
  default:;
  }

  throw std::runtime_error{"Invalid decode mode"};
}

bool Context::alive() const { return !window->shouldClose(); }

void Context::update() {
  vkfw::pollEvents();
  vk.get_temp_pools().garbage_collect();
}
} // namespace vkvideo
