export module vkvideo.graphics:swapchain;

import vkvideo.core;
import std;
import vkfw;
import vulkan_hpp;
import vk_mem_alloc_hpp;
import :queues;
import :vku;
import :temppools;

export namespace vkvideo::graphics {

// swapchain abstraction
class RenderTargetImageProvider {
public:
  virtual ~RenderTargetImageProvider() = default;
  using OnChangeCallback =
      std::function<void(std::span<vk::Image>, vk::Format, vk::Extent2D)>;

  virtual vk::Format get_image_format() = 0;
  virtual vk::Extent2D get_image_extent() = 0;
  virtual std::vector<vk::Image> get_images() = 0;
  virtual i32 num_fifs() = 0;
  virtual std::optional<vk::ImageLayout> present_layout() {
    return std::nullopt;
  }

  virtual std::optional<std::pair<i32, vk::Semaphore>>
  acquire_next_image(i32 fif_idx, i64 timeout) = 0;
  virtual void end_frame(QueueManager &queues, i32 image_idx,
                         std::span<vk::Semaphore> wait_sems,
                         vk::Semaphore image_acq_sem) = 0;

  void set_on_change_cb(OnChangeCallback cb) {
    on_change_cb = std::move(cb);
    on_change();
  }

protected:
  void on_change() {
    auto images = get_images();
    if (on_change_cb)
      on_change_cb(images, get_image_format(), get_image_extent());
  }

private:
  OnChangeCallback on_change_cb;
};

class SwapchainRenderTargetImageProvider : public RenderTargetImageProvider {
public:
  SwapchainRenderTargetImageProvider(vk::raii::PhysicalDevice &physical_device,
                                     vk::raii::Device &device,
                                     vkfw::Window window,
                                     vk::SurfaceKHR surface, i32 fif_count)
      : physical_device{physical_device}, device{device}, window{window},
        surface{surface}, fif_count{fif_count} {
    image_acquire_sems.reserve(fif_count);
    for (i32 i = 0; i < fif_count; ++i) {
      auto img_acq_sem =
          *image_acquire_sems.emplace_back(device, vk::SemaphoreCreateInfo{});
      auto name = std::format("image_acquire_sems[{}]", i);
      set_debug_label(device, img_acq_sem, name.c_str());
    }
    window.callbacks()->on_framebuffer_resize =
        [this](vkfw::Window w, std::size_t width, std::size_t height) {
          if (width > 0 && height > 0)
            recreate(static_cast<i32>(width), static_cast<i32>(height));
        };
    recreate();
  }
  ~SwapchainRenderTargetImageProvider() override = default;

  vk::Format get_image_format() override { return image_format; }
  vk::Extent2D get_image_extent() override { return image_extent; }
  std::vector<vk::Image> get_images() override { return images; }
  i32 num_fifs() override { return fif_count; }
  std::optional<vk::ImageLayout> present_layout() override {
    return vk::ImageLayout::ePresentSrcKHR;
  }

  std::optional<std::pair<i32, vk::Semaphore>>
  acquire_next_image(i32 fif_idx, i64 timeout) override {
    if (timeout < 0)
      return std::nullopt;

    SteadyClock clock{};
    auto deadline = timeout == std::numeric_limits<i64>::max()
                        ? timeout
                        : clock.get_time() + timeout;

    try {
      auto sem = *image_acquire_sems[fif_idx];
      auto [result, image_idx] =
          swapchain.acquireNextImage(timeout, sem, nullptr);
      return std::pair{image_idx, sem};
    } catch (vk::OutOfDateKHRError) {
      recreate();
      return acquire_next_image(fif_idx, deadline - clock.get_time());
    }
  }

  void end_frame(QueueManager &queues, i32 image_idx,
                 std::span<vk::Semaphore> wait_sems,
                 vk::Semaphore image_acq_sem) override {
    try {
      auto img_idx = static_cast<u32>(image_idx);
      auto [lock, present_queue] = queues.get_graphics_queue();
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
  }

  void recreate() {
    auto [width, height] = window.getFramebufferSize();
    recreate(static_cast<i32>(width), static_cast<i32>(height));
  }

  void recreate(i32 width, i32 height) {
    device.waitIdle();

    auto caps = physical_device.getSurfaceCapabilitiesKHR(surface);
    auto image_count = std::min(caps.minImageCount + 1, caps.maxImageCount);

    auto formats = physical_device.getSurfaceFormatsKHR(surface);
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
        device, vk::SwapchainCreateInfoKHR{
                    .surface = surface,
                    .minImageCount = image_count,
                    .imageFormat = image_format = format.format,
                    .imageColorSpace = format.colorSpace,
                    .imageExtent = image_extent =
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

    images = swapchain.getImages();
    on_change();
  }

private:
  vk::raii::PhysicalDevice &physical_device;
  vk::raii::Device &device;
  vkfw::Window window;
  vk::SurfaceKHR surface;
  vk::raii::SwapchainKHR swapchain = nullptr;
  vk::Format image_format;
  vk::Extent2D image_extent;
  i32 fif_count;
  std::vector<vk::Image> images;
  std::vector<vk::raii::Semaphore> image_acquire_sems;
};

class HeadlessRenderTargetImageProvider : public RenderTargetImageProvider {
public:
  using EndFrameCallback =
      std::function<void(i32, std::span<vk::Semaphore>, vk::Semaphore)>;
  HeadlessRenderTargetImageProvider(QueueManager &queues,
                                    TempCommandPools &tx_pool,
                                    vma::Allocator &allocator,
                                    vk::raii::Device &device, i32 num_images,
                                    vk::Format format, vk::Extent2D size,
                                    vk::ImageUsageFlags usage,
                                    EndFrameCallback callback)
      : format{format}, size{size}, callback{std::move(callback)} {
    images.reserve(num_images);
    image_acquire_sems.reserve(num_images);
    std::vector<vk::SemaphoreSubmitInfo> signal_sems;
    for (i32 i = 0; i < num_images; ++i) {
      auto [uniq_image, uniq_alloc] = allocator.createImageUnique(
          {
              .imageType = vk::ImageType::e2D,
              .format = format,
              .extent = vk::Extent3D{size.width, size.height, 1},
              .mipLevels = 1,
              // TODO: pack everything in one image???
              .arrayLayers = 1,
              .samples = vk::SampleCountFlagBits::e1,
              .tiling = vk::ImageTiling::eOptimal,
              .usage = usage,
              .sharingMode = vk::SharingMode::eExclusive,
              .initialLayout = vk::ImageLayout::eUndefined,
          },
          {
              .requiredFlags = vk::MemoryPropertyFlagBits::eDeviceLocal,
          });
      vk::raii::Image image{device, uniq_image.release()};
      auto name = std::format("render_target_image[{}]", i);
      set_debug_label(device, *image, name.c_str());
      images.emplace_back(std::move(image), std::move(uniq_alloc));
      auto img_acq_sem =
          *image_acquire_sems.emplace_back(device, vk::SemaphoreCreateInfo{});
      name = std::format("image_acquire_sems[{}]", i);
      set_debug_label(device, img_acq_sem, name.c_str());
      signal_sems.push_back(vk::SemaphoreSubmitInfo{
          .semaphore = img_acq_sem,
      });
    }

    // HACK: this is to signal all image_acquire_sems
    auto cmd = tx_pool.begin(0);
    cmd.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    cmd.end();
    tx_pool.end2(std::move(cmd), 0, {}, {}, signal_sems);
  }

  vk::Format get_image_format() override { return format; }
  vk::Extent2D get_image_extent() override { return size; }
  i32 num_fifs() override { return images.size(); }
  std::vector<vk::Image> get_images() override {
    return images |
           std::ranges::views::transform([](auto &i) { return *i.first; }) |
           std::ranges::to<std::vector>();
  }

  std::optional<std::pair<i32, vk::Semaphore>>
  acquire_next_image(i32 fif_idx, i64 timeout) override {
    return std::pair{fif_idx, *image_acquire_sems[fif_idx]};
  };

  void end_frame(QueueManager &queues, i32 image_idx,
                 std::span<vk::Semaphore> wait_sems,
                 vk::Semaphore image_acq_sem) override {
    callback(image_idx, wait_sems, image_acq_sem);
  };

private:
  std::vector<std::pair<vk::raii::Image, vma::UniqueAllocation>> images;
  vk::Format format;
  vk::Extent2D size;
  std::vector<vk::raii::Semaphore> image_acquire_sems;
  EndFrameCallback callback;
};

class RenderTarget;

struct AcquiredFrameInfo;

struct FrameInfo {
  i32 frame_idx;
  i32 fif_idx; // frame_idx % num_frames_in_flight, provided for convenience

  std::optional<AcquiredFrameInfo> acquire_image(i64 timeout);

private:
  friend class RenderTarget;
  RenderTarget *ctx;

  FrameInfo(RenderTarget *ctx) : ctx{ctx} {}
};

struct AcquiredFrameInfo : FrameInfo {
  i32 image_idx;
  vk::Image image;
  vk::ImageView view;
  vk::Format format;
  vk::Extent2D extent;
  vk::Semaphore image_acquire_sem;
  vk::Semaphore image_present_sem;

  AcquiredFrameInfo(FrameInfo info) : FrameInfo{std::move(info)} {}
};

class RenderTarget {
public:
  RenderTarget(vk::raii::Device &device,
               std::unique_ptr<RenderTargetImageProvider> img_provider)
      : device{device}, img_provider(std::move(img_provider)) {
    this->img_provider->set_on_change_cb(
        [this](std::span<vk::Image> images, vk::Format format,
               vk::Extent2D size) { on_recreate(images, format, size); });
  }

  i32 num_fifs() { return img_provider->num_fifs(); }
  std::optional<vk::ImageLayout> present_layout() {
    return img_provider->present_layout();
  }

  FrameInfo begin_frame() {
    FrameInfo frame{this};
    frame.frame_idx = frame_idx;
    frame.fif_idx = frame_idx % img_provider->num_fifs();
    return frame;
  }

  void end_frame(QueueManager &queues, const AcquiredFrameInfo &frame,
                 const vk::ArrayProxy<vk::Semaphore> &wait_sems) {
    std::vector<vk::Semaphore> wait_sem_vec{wait_sems.begin(), wait_sems.end()};
    wait_sem_vec.push_back(image_present_sems[frame.image_idx]);
    img_provider->end_frame(queues, frame.image_idx, wait_sem_vec,
                            frame.image_acquire_sem);
    ++frame_idx;
  }

private:
  vk::raii::Device &device;
  std::unique_ptr<RenderTargetImageProvider> img_provider;
  std::vector<vk::raii::ImageView> image_views;
  std::vector<vk::raii::Semaphore> image_present_sems;
  i32 frame_idx = 0;

  friend class FrameInfo;

  void on_recreate(std::span<vk::Image> images, vk::Format format,
                   vk::Extent2D size) {
    image_views.clear();
    image_views.reserve(images.size());

    image_present_sems.clear();
    image_present_sems.reserve(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
      auto image = images[i];
      auto name = std::format("swapchain_images[{}]", i);
      set_debug_label(device, image, name.c_str());
      auto &image_view = image_views.emplace_back(
          device, vk::ImageViewCreateInfo{
                      .image = image,
                      .viewType = vk::ImageViewType::e2D,
                      .format = format,
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
      name = std::format("swapchain_image_views[{}]", i);
      set_debug_label(device, *image_view, name.c_str());

      image_present_sems.emplace_back(device, vk::SemaphoreCreateInfo{});
    }
  }
};

std::optional<AcquiredFrameInfo> FrameInfo::acquire_image(i64 timeout) {
  auto img_provider = ctx->img_provider.get();
  auto opt = img_provider->acquire_next_image(fif_idx, timeout);
  if (!opt.has_value())
    return std::nullopt;

  auto [image_idx, sem] = opt.value();
  AcquiredFrameInfo info = *this;
  info.image_idx = image_idx;
  info.image = img_provider->get_images()[image_idx];
  info.view = *ctx->image_views[image_idx];
  info.format = img_provider->get_image_format();
  info.extent = img_provider->get_image_extent();
  info.image_acquire_sem = sem;
  info.image_present_sem = ctx->image_present_sems[image_idx];
  return info;
}
} // namespace vkvideo::graphics
