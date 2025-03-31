#include "av.h"
#include "averror.h"
#include "avutils.h"
#include "codeccontext.h"
#include "formatcontext.h"
#include "vkvideo/context/context.hpp"
#include "vkvideo/medias/stb_image_write.hpp"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixfmt.h>
#include <sstream>

int main(int argc, char *argv[]) {
  namespace vkv = vkvideo;
  namespace vkr = vkv::vkr;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <input.mkv>" << std::endl;
    return 1;
  }

  av::init();
  av::set_logging_level("info");

  av::FormatContext in_demuxer;
  in_demuxer.openInput(argv[1]);
  in_demuxer.findStreamInfo();

  const AVCodec *codec = nullptr;
  int stream_index = av_find_best_stream(in_demuxer.raw(), AVMEDIA_TYPE_VIDEO,
                                         -1, -1, &codec, 0);
  if (stream_index < 0)
    av::throws_if(av::OptionalErrorCode::null(), stream_index,
                  av::ffmpeg_category());

  vkv::Context context{vkv::ContextArgs{
      .mode = vkv::DisplayMode::Preview,
      .width = 640,
      .height = 360,
      .fps = {30, 1},
      .sample_rate = 44100,
      .ch_layout = av::ChannelLayout{2}, // stereo
      .sample_format = AV_SAMPLE_FMT_S16,
      .render_output = "output.mkv",
  }};

  auto stream = in_demuxer.stream(stream_index);
  av::VideoDecoderContext decoder{stream};

  auto &r_decoder = *decoder.raw();
  context.enable_hardware_acceleration(decoder);

  decoder.open();

  vkv::i32 width = decoder.width();
  vkv::i32 height = decoder.height();

  auto &r_in_demuxer = *in_demuxer.raw();

  av::Packet packet;
  av::VideoFrame frame;

  bool flushed = false;

  // i don't really like the C++ APIs,
  // this is just how i've been doing it in sve2
  // but whatever
  auto next_packet = [&]() mutable -> bool {
    if (flushed) {
      return false;
    }

    int err;
    do {
      av_packet_unref(packet.raw());
      err = av_read_frame(&r_in_demuxer, packet.raw());
      if (err < 0 && err != AVERROR_EOF) {
        av::throws_if(av::OptionalErrorCode::null(), err,
                      av::ffmpeg_category());
      }
    } while (err == 0 && packet.streamIndex() != stream_index);

    if (err == AVERROR_EOF) {
      flushed = true;
    }

    packet = nullptr;
    return true;
  };

  auto next_frame = [&]() -> bool {
    int err;
    do {
      err = avcodec_receive_frame(&r_decoder, frame.raw());
      if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        av::throws_if(av::OptionalErrorCode::null(), err,
                      av::ffmpeg_category());
      }

      if (next_packet()) {
        int err = avcodec_send_packet(&r_decoder, packet.raw());
        if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
          av::throws_if(av::OptionalErrorCode::null(), err,
                        av::ffmpeg_category());
        }
      } else {
        return false;
      }
    } while (err == AVERROR(EAGAIN));

    return err >= 0;
  };

  auto &vk = context.get_vulkan();
  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
      }};

  vkr::CommandBuffers cmdbufs{vk.get_device(),
                              vk::CommandBufferAllocateInfo{
                                  .commandPool = *pool,
                                  .level = vk::CommandBufferLevel::ePrimary,
                                  .commandBufferCount = 1,
                              }};
  vkr::CommandBuffer cmdbuf = std::move(cmdbufs[0]);

  vkr::Queue queue{vk.get_device(), static_cast<vkv::u32>(vk.get_qf_graphics()),
                   0};
  vkr::Fence fence{vk.get_device(), vk::FenceCreateInfo{}};
  auto dst_buffer = vk.get_vma_allocator().createBuffer(
      vk::BufferCreateInfo{
          .size = static_cast<vk::DeviceSize>(width * height),
          .usage = vk::BufferUsageFlagBits::eTransferDst,
      },
      VmaAllocationCreateInfo{
          .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      });

  int num_frames = 0;
  for (int i = 0; i < 10 && next_frame(); ++i) {
    // frame.dump();
    assert(frame.pixelFormat() == AV_PIX_FMT_VULKAN);
    auto &vk_frame = *reinterpret_cast<AVVkFrame *>(frame.data());

    cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });

    vk::ImageMemoryBarrier2 img_barrier{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eBottomOfPipe, // not sure...
        .srcAccessMask = static_cast<vk::AccessFlags2>(
            static_cast<vkv::u32>(vk_frame.access[0])),
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = static_cast<vk::ImageLayout>(vk_frame.layout[0]),
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = vk_frame.img[0],
        .subresourceRange = vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::ePlane0,
            .levelCount = 1,
            .layerCount = 1,
        }};
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{}.setImageMemoryBarriers(img_barrier));

    vk::BufferImageCopy copy_info{
        .imageSubresource =
            vk::ImageSubresourceLayers{
                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
                .layerCount = 1,
            },
        .imageOffset = vk::Offset3D{0, 0, 0},
        .imageExtent = vk::Extent3D{static_cast<vkv::u32>(width),
                                    static_cast<vkv::u32>(height), 1},
    };
    cmdbuf.copyImageToBuffer(vk_frame.img[0],
                             vk::ImageLayout::eTransferSrcOptimal,
                             dst_buffer.get_buffer(), copy_info);

    cmdbuf.end();

    queue.submit(
        vk::SubmitInfo{
            .commandBufferCount = 1,
            .pCommandBuffers = &*cmdbuf,
        },
        fence);
    auto _ = vk.get_device().waitForFences(*fence, true, UINT64_MAX);
    vk.get_device().resetFences(*fence);

    auto memory = dst_buffer.map_memory();
    auto data = memory.data();
    assert(data.size() == width * height);

    std::stringstream ss;
    ss << "output_" << num_frames << ".png";
    vkvideo::write_img_grayscale(ss.str(), width, height, data.data());

    ++num_frames;
  }

  std::cout << "Total frames: " << num_frames << std::endl;

  return 1;
}
