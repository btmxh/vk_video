add_library(vkvideo)

target_sources(
  vkvideo
  PUBLIC FILE_SET
         CXX_MODULES
         BASE_DIRS
         "${CMAKE_CURRENT_LIST_DIR}"
         FILES
         core/types.cppm
         core/unique_any.cppm
         core/mod.cppm
         third_party/ffmpeg.cppm
         third_party/webp.cppm
         third_party/mod.cppm
         graphics/tlsem.cppm
         graphics/queues.cppm
         graphics/tx.cppm
         graphics/swapchain.cppm
         graphics/vkc.cppm
         graphics/mod.cppm
         medias/stb_image_write.cppm
         medias/video.cppm
         medias/video_frame.cppm
         medias/stream.cppm
         medias/mod.cppm
         context/context.cppm
         context/mod.cppm
         mod.cppm
  PRIVATE medias/video_formats.cpp
          # context/context.cpp graphics/vk.cpp graphics/vma.cpp graphics/tx.cpp
          # graphics/mutex.cpp graphics/swapchain.cpp medias/wrapper.cpp
          # medias/stb_image_write.cpp medias/stream.cpp medias/video.cpp
)

configure_file("${PROJECT_SOURCE_DIR}/include/vkvideo/version.h.in"
               "${PROJECT_BINARY_DIR}/include/vkvideo/version.h")

target_include_directories(vkvideo PUBLIC "${VkVideo_BINARY_DIR}/include")
target_compile_features(vkvideo PUBLIC cxx_std_23)

target_link_libraries(vkvideo PUBLIC vkvideo_VulkanHpp vkfw-module vma
                                     vk-bootstrap FFmpeg::FFmpeg)

if(WebP_FOUND)
  target_link_libraries(vkvideo PUBLIC WebP::webpdemux)
  target_compile_definitions(vkvideo PUBLIC VKVIDEO_HAVE_WEBP)
endif()

target_include_directories(vkvideo PUBLIC "${FFmpeg_INCLUDE_DIRS}")
target_compile_definitions(vkvideo PUBLIC __STDC_CONSTANT_MACROS
                                          VULKAN_HPP_NO_CONSTRUCTORS)

source_group(
  TREE "${PROJECT_SOURCE_DIR}/include"
  PREFIX "Header Files"
  FILES ${HEADER_LIST})
