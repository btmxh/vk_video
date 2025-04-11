#include "vkvideo/graphics/swapchain.hpp"

#include <vulkan/vulkan.hpp>

namespace vkvideo {

VkSwapchainContext::VkSwapchainContext(vkr::Device &device,
                                       vkb::Device vkb_device,
                                       vkfw::Window window,
                                       vk::SurfaceKHR surface,
                                       vkr::Queue present_queue, i32 fif_count)
    : device{&device}, vkb_device{vkb_device}, window{window}, surface{surface},
      present_queue{present_queue}, fif_count{fif_count} {
  recreate();

  for (i32 i = 0; i < fif_count; ++i) {
    image_acq_sems.emplace_back(device, vk::SemaphoreCreateInfo{});
  }
}

void VkSwapchainContext::recreate() {
  auto [width, height] = window.getFramebufferSize();
  recreate(static_cast<i32>(width), static_cast<i32>(height));
}

void VkSwapchainContext::recreate(i32 width, i32 height) {
  device->waitIdle();

  auto r_swapchain =
      vkb::SwapchainBuilder{vkb_device, surface}
          .set_desired_extent(width, height)
          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
          .set_old_swapchain(*swapchain)
          .set_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
          .set_desired_format(vk::SurfaceFormatKHR{
              .format = vk::Format::eB8G8R8A8Unorm,
              .colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear,
          })
          .build();

  if (!r_swapchain.has_value()) {
    throw std::runtime_error{r_swapchain.error().message()};
  }

  swapchain_extent = r_swapchain.value().extent;
  swapchain_format = static_cast<vk::Format>(r_swapchain.value().image_format);
  swapchain = vkr::SwapchainKHR{*device, r_swapchain.value().swapchain};

  auto r_images = r_swapchain.value().get_images();
  if (!r_images.has_value()) {
    throw std::runtime_error{r_images.error().message()};
  }

  swapchain_images.clear();
  swapchain_images.reserve(r_images.value().size());
  for (const auto &image : r_images.value()) {
    swapchain_images.emplace_back(image);
  }

  auto r_image_views = r_swapchain->get_image_views();
  if (!r_image_views.has_value()) {
    throw std::runtime_error{r_image_views.error().message()};
  }

  swapchain_image_views.clear();
  swapchain_image_views.reserve(r_image_views.value().size());
  for (const auto &view : r_image_views.value()) {
    swapchain_image_views.emplace_back(*device, view);
  }

  if (recreate_callback)
    recreate_callback(swapchain_images);
}

std::optional<
    std::tuple<i32, vk::Image, vk::ImageView, vk::Extent2D, vk::Format>>
FrameInfo::acquire_image(i64 timeout) {
  try {
    auto [result, image_idx] =
        ctx->swapchain.acquireNextImage(timeout, image_acq_sem, nullptr);
    return std::tuple{image_idx, ctx->swapchain_images[image_idx],
                      *ctx->swapchain_image_views[image_idx],
                      ctx->swapchain_extent, ctx->swapchain_format};
  } catch (vk::OutOfDateKHRError) {
    ctx->recreate();
    return acquire_image(timeout);
  }
}

FrameInfo VkSwapchainContext::begin_frame() {
  FrameInfo frame{this};
  frame.frame_idx = frame_idx;
  frame.fif_idx = frame_idx % fif_count;
  frame.image_acq_sem = image_acq_sems[frame.fif_idx];

  return frame;
}

void VkSwapchainContext::end_frame(
    i32 image_idx,
    const vk::ArrayProxyNoTemporaries<vk::Semaphore> &wait_sems) {
  auto img_idx = static_cast<u32>(image_idx);
  try {
    auto result = present_queue.presentKHR(vk::PresentInfoKHR{}
                                               .setWaitSemaphores(wait_sems)
                                               .setImageIndices(img_idx)
                                               .setSwapchains(*swapchain));
    if (result == vk::Result::eErrorOutOfDateKHR) {
      recreate();
    }
  } catch (vk::OutOfDateKHRError) {
    recreate();
  }

  ++frame_idx;
}
} // namespace vkvideo
