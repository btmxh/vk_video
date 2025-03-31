#include "vkvideo/medias/stb_image_write.hpp"
#include "codec.h"
#include "codeccontext.h"
#include "formatcontext.h"
#include "videorescaler.h"
#include <libavcodec/avcodec.h>

namespace vkvideo {
void write_img_grayscale(std::string_view filename, i32 width, i32 height,
                         const void *data) {
  auto format = av::guessOutputFormat("", std::string{filename});

  av::FormatContext muxer;
  auto codec_id = format.defaultVideoCodecId();
  auto codec = av::findEncodingCodec(codec_id);

  AVPixelFormat *pix_fmts;
  avcodec_get_supported_config(
      nullptr, codec.raw(), AV_CODEC_CONFIG_PIX_FORMAT, 0,
      const_cast<const void **>(reinterpret_cast<void **>(&pix_fmts)), nullptr);
  auto pix_fmt = avcodec_find_best_pix_fmt_of_list(pix_fmts, AV_PIX_FMT_GRAY8,
                                                   false, nullptr);

  av::VideoEncoderContext encoder{codec};
  encoder.setWidth(width);
  encoder.setHeight(height);
  encoder.setTimeBase({1, 25});
  encoder.setPixelFormat(pix_fmt);
  encoder.open();

  auto stream = muxer.addStream(encoder);

  muxer.openOutput(std::string{filename});
  muxer.writeHeader();

  av::VideoFrame frame{static_cast<const u8 *>(data),
                       static_cast<std::size_t>(width * height),
                       AV_PIX_FMT_GRAY8, width, height};
  av::VideoRescaler rescaler{width, height, encoder.pixelFormat(),
                             width, height, AV_PIX_FMT_GRAY8};
  auto rescale_frame = rescaler.rescale(frame);
  auto packet = encoder.encode(rescale_frame);
  if (packet)
    muxer.writePacket(packet);
  packet = encoder.encode();
  if (packet)
    muxer.writePacket(packet);

  muxer.writeTrailer();
}
} // namespace vkvideo
