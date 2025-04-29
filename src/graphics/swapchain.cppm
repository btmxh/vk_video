export module vkvideo.graphics:swapchain;

import vkvideo.core;
import std;
import vkfw;
import vulkan_hpp;
import :queues;

export namespace vkvideo::graphics {

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
  void init(vk::raii::PhysicalDevice &physical_device, vk::raii::Device &device,
            vkfw::Window window, vk::SurfaceKHR surface, i32 fif_count) {
    this->physical_device = &physical_device;
    this->device = &device;
    this->window = window;
    this->surface = surface;
    this->fif_count = fif_count;

    recreate();

    for (i32 i = 0; i < fif_count; ++i) {
      image_acq_sems.emplace_back(device, vk::SemaphoreCreateInfo{});
    }
  }

  i32 num_images() const { return swapchain_images.size(); }
  i32 num_fifs() const { return fif_count; }

  void set_recreate_callback(std::function<void(std::span<vk::Image>)> cb) {
    recreate_callback = std::move(cb);
  }

  void recreate() {
    auto [width, height] = window.getFramebufferSize();
    recreate(static_cast<i32>(width), static_cast<i32>(height));
  }

  void recreate(i32 width, i32 height) {
    device->waitIdle();

    auto caps = physical_device->getSurfaceCapabilitiesKHR(surface);
    auto image_count = std::min(caps.minImageCount + 1, caps.maxImageCount);

    auto formats = physical_device->getSurfaceFormatsKHR(surface);
    auto format = formats[0];
    if (auto it = std::find_if(formats.begin(), formats.end(),
                               [&](const auto format) {
                                 return format.format ==
                                            vk::Format::eB8G8R8A8Unorm &&
                                        format.colorSpace ==
                                            vk::ColorSpaceKHR::eSrgbNonlinear;
                               });
        it != formats.end())
      format = *it;

    swapchain = vk::raii::SwapchainKHR{
        *device, vk::SwapchainCreateInfoKHR{
                     .surface = surface,
                     .minImageCount = image_count,
                     .imageFormat = swapchain_format = format.format,
                     .imageColorSpace = format.colorSpace,
                     .imageExtent = swapchain_extent =
                         {
                             .width = std::clamp(static_cast<u32>(width),
                                                 caps.minImageExtent.width,
                                                 caps.maxImageExtent.width),
                             .height = std::clamp(static_cast<u32>(height),
                                                  caps.minImageExtent.height,
                                                  caps.maxImageExtent.height),
                         },
                     .imageArrayLayers = 1,
                     .imageUsage = vk::ImageUsageFlagBits::eTransferDst |
                                   vk::ImageUsageFlagBits::eColorAttachment,
                     .imageSharingMode = vk::SharingMode::eExclusive,
                     .preTransform = caps.currentTransform,
                     .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
                     .presentMode = vk::PresentModeKHR::eFifo,
                     .clipped = true,
                     .oldSwapchain = *swapchain,
                 }};

    swapchain_images = swapchain.getImages();
    swapchain_image_views.clear();
    swapchain_image_views.reserve(swapchain_images.size());
    for (const auto &image : swapchain_images) {
      swapchain_image_views.emplace_back(
          *device, vk::ImageViewCreateInfo{
                       .image = image,
                       .viewType = vk::ImageViewType::e2D,
                       .format = swapchain_format,
                       .components =
                           {
                               vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity,
                           },
                       .subresourceRange =
                           {
                               .aspectMask = vk::ImageAspectFlagBits::eColor,
                               .levelCount = 1,
                               .layerCount = 1,
                           },
                   });
    }

    if (recreate_callback)
      recreate_callback(swapchain_images);
  }

  FrameInfo begin_frame() {
    FrameInfo frame{this};
    frame.frame_idx = frame_idx;
    frame.fif_idx = frame_idx % fif_count;
    frame.image_acq_sem = image_acq_sems[frame.fif_idx];

    return frame;
  }

  void end_frame(QueueManager &queues, i32 image_idx,
                 const vk::ArrayProxyNoTemporaries<vk::Semaphore> &wait_sems) {
    auto img_idx = static_cast<u32>(image_idx);
    try {
      auto [_lock, present_queue] = queues.get_graphics_queue();
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

private:
  vk::raii::PhysicalDevice *physical_device = nullptr;
  vk::raii::Device *device = nullptr;
  vkfw::Window window;
  vk::SurfaceKHR surface;
  vk::raii::SwapchainKHR swapchain = nullptr;
  std::vector<vk::Image> swapchain_images;
  std::function<void(std::span<vk::Image>)> recreate_callback;
  vk::Extent2D swapchain_extent;
  vk::Format swapchain_format;

  std::vector<vk::raii::ImageView> swapchain_image_views;
  std::vector<vk::raii::Semaphore> image_acq_sems;

  i32 frame_idx = 0;
  i32 fif_count;

  friend class FrameInfo;
};

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

} // namespace vkvideo::graphics
