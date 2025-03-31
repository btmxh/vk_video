file(GLOB HEADER_LIST CONFIGURE_DEPENDS "${VkVideo_SOURCE_DIR}/include/vkvideo/*.hpp")

add_library(vkvideo
  context/context.cpp 
  graphics/vk.cpp
  graphics/vma.cpp
  medias/wrapper.cpp
  medias/stb_image_write.cpp
  ${HEADER_LIST}
)

configure_file (
  "${PROJECT_SOURCE_DIR}/include/vkvideo/version.h.in"
  "${PROJECT_BINARY_DIR}/include/vkvideo/version.h"
)


target_include_directories(vkvideo PUBLIC ../include)
target_include_directories(vkvideo PUBLIC "${VkVideo_SOURCE_DIR}/include")
target_include_directories(vkvideo PUBLIC "${VkVideo_BINARY_DIR}/include")
target_compile_features(vkvideo PUBLIC cxx_std_20)

target_link_libraries(vkvideo PUBLIC
  vkfw
  Vulkan::Vulkan
  GPUOpen::VulkanMemoryAllocator
  vk-bootstrap
  avcpp::avcpp
  "${FFmpeg_LIBRARIES}"
)
target_precompile_headers(vkvideo
  PUBLIC <vulkan/vulkan.hpp>
  PUBLIC <vkfw/vkfw.hpp>
)
target_include_directories(vkvideo PUBLIC "${FFmpeg_INCLUDE_DIRS}")
target_compile_definitions(vkvideo PUBLIC __STDC_CONSTANT_MACROS VULKAN_HPP_NO_CONSTRUCTORS)

source_group(
  TREE "${PROJECT_SOURCE_DIR}/include"
  PREFIX "Header Files"
  FILES ${HEADER_LIST}
)
