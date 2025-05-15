set(VKFW_NO_STRUCT_CONSTRUCTORS ON CACHE BOOL "" FORCE)
# disable import vulkan_hpp; to avoid collisions with our Vulkan-Hpp library
set(VKFW_BUILD_WITH_VULKAN_MODULE OFF CACHE BOOL "" FORCE)
set(VKFW_BUILD_CXX_MODULE ON CACHE BOOL "" FORCE)
add_subdirectory(vkfw)

add_library(vma)
target_sources(
    vma
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/VulkanMemoryAllocator-Hpp/src/
        FILES
            ${CMAKE_CURRENT_LIST_DIR}/VulkanMemoryAllocator-Hpp/src/vk_mem_alloc.cppm
)
target_include_directories(
    vma
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/VulkanMemoryAllocator-Hpp/include
        ${CMAKE_CURRENT_LIST_DIR}/VulkanMemoryAllocator-Hpp/VulkanMemoryAllocator/include
)
target_compile_features(vma PUBLIC cxx_std_23)
target_link_libraries(vma PUBLIC vkvideo_VulkanHpp)
target_compile_definitions(vma PRIVATE VMA_ENABLE_VULKAN_HPP_MODULE)
