#pragma once

#include "VkBootstrap.h"
#include "vkvideo/core/types.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <type_traits>

namespace vkvideo {
namespace vkr = vk::raii;

struct SwapchainFrameSemaphores {};

class VkSwapchainContext;

struct FrameInfo {
  vk::Semaphore image_acq_sem;
  i32 frame_idx;
  i32 fif_idx; // frame_idx % num_frames_in_flight, provided for convenience

  std::optional<
      std::tuple<i32, vk::Image, vk::ImageView, vk::Extent2D, vk::Format>>
  acquire_image(i64 timeout);

private:
  friend class VkSwapchainContext;
  VkSwapchainContext *ctx;

  FrameInfo(VkSwapchainContext *ctx) : ctx{ctx} {}
};

class VkSwapchainContext {
public:
  VkSwapchainContext(std::nullptr_t) {};
  VkSwapchainContext(vkr::Device &device, vkb::Device vkb_device,
                     vkfw::Window window, vk::SurfaceKHR surface,
                     vkr::Queue present_queue, i32 fif_count);

  i32 num_images() const { return swapchain_images.size(); }
  i32 num_fifs() const { return fif_count; }

  void set_recreate_callback(std::function<void(std::span<vk::Image>)> cb) {
    recreate_callback = std::move(cb);
  }

  void recreate();
  void recreate(i32 width, i32 height);

  FrameInfo begin_frame();
  void end_frame(i32 image_idx,
                 const vk::ArrayProxyNoTemporaries<vk::Semaphore> &wait_sems);

private:
  vkb::Device vkb_device;
  vkr::Device *device = nullptr;
  vkfw::Window window;
  vk::SurfaceKHR surface;
  vkr::SwapchainKHR swapchain = nullptr;
  vkr::Queue present_queue = nullptr;
  std::vector<vk::Image> swapchain_images;
  std::function<void(std::span<vk::Image>)> recreate_callback;
  vk::Extent2D swapchain_extent;
  vk::Format swapchain_format;

  std::vector<vkr::ImageView> swapchain_image_views;
  std::vector<vkr::Semaphore> image_acq_sems;

  i32 frame_idx = 0;
  i32 fif_count;

  friend class FrameInfo;
};

static_assert(std::is_move_assignable_v<VkSwapchainContext>);

} // namespace vkvideo
