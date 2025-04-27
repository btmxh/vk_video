add_library(vkvideo)

target_sources(
  vkvideo
  PUBLIC FILE_SET
         CXX_MODULES
         BASE_DIRS
         "${CMAKE_CURRENT_LIST_DIR}"
         FILES
         core/types.cppm
         core/mod.cppm
         medias/stb_image_write.cppm
         medias/ffmpeg.cppm
         medias/mod.cppm
         mod.cppm
         # context/context.cpp graphics/vk.cpp graphics/vma.cpp graphics/tx.cpp
         # graphics/mutex.cpp graphics/swapchain.cpp medias/wrapper.cpp
         # medias/stb_image_write.cpp medias/stream.cpp medias/video.cpp
)

configure_file("${PROJECT_SOURCE_DIR}/include/vkvideo/version.h.in"
               "${PROJECT_BINARY_DIR}/include/vkvideo/version.h")

target_include_directories(vkvideo PUBLIC "${VkVideo_BINARY_DIR}/include")
target_compile_features(vkvideo PUBLIC cxx_std_23)

target_link_libraries(
  vkvideo PUBLIC vkfw Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator
                 vk-bootstrap mcpp::unique-any FFmpeg::FFmpeg)

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
