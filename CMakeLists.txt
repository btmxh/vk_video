cmake_minimum_required(VERSION 3.19...4.0)

project(VkVideo VERSION 0.1.0
                DESCRIPTION "Vulkan Video decoding/encoding sample"
                LANGUAGES C CXX)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)

  include(CTest)
  find_package(Doxygen)
  if (DOXYGEN_FOUND)
    add_subdirectory(docs)
  else()
    message(STATUS "Doxygen not found, not building docs.")
  endif()
endif()

find_package(Vulkan COMPONENTS glslc REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(WebP CONFIG)
add_subdirectory(extern)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

add_subdirectory(src)
add_subdirectory(app)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR VK_VIDEO_BUILD_TESTING AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

