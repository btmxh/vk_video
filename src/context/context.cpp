#include "vkvideo/context/context.hpp"

namespace vkvideo {
vkfw::InitHints ContextArgs::init_hint() const {
  return vkfw::InitHints{
      .platform = mode == DisplayMode::Preview ? vkfw::Platform::eAny
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
} // namespace vkvideo
