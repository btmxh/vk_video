cmake_minimum_required(VERSION 3.31 FATAL_ERROR)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
set(CMAKE_CXX_MODULE_STD 1)

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
find_package(FFmpeg COMPONENTS AVCODEC AVUTIL AVFORMAT SWSCALE SWRESAMPLE REQUIRED)
add_subdirectory(extern)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# source: https://thatonegamedev.com/cpp/cmake/how-to-compile-shaders-with-cmake/
function(add_shaders TARGET_NAME)
  set(SHADER_SOURCE_FILES ${ARGN})
  list(LENGTH SHADER_SOURCE_FILES SHADER_COUNT)

  if(SHADER_COUNT EQUAL 0)
    message(FATAL_ERROR "No shader source files provided")
  endif()

  set(SHADER_COMMANDS)
  set(SHADER_PRODUCTS)

  foreach(SHADER_SOURCE_FILE ${SHADER_SOURCE_FILES})
    cmake_path(ABSOLUTE_PATH SHADER_SOURCE_FILE NORMALIZE)
    cmake_path(GET SHADER_SOURCE_FILE FILENAME SHADER_NAME)

    list(APPEND SHADER_COMMANDS COMMAND)
    list(APPEND SHADER_COMMANDS Vulkan::glslc)
    list(APPEND SHADER_COMMANDS "${SHADER_SOURCE_FILE}")
    list(APPEND SHADER_COMMANDS "-o")
    list(APPEND SHADER_COMMANDS "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")

    list(APPEND SHADER_PRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
  endforeach()


  add_custom_target(
    ${TARGET_NAME} ALL
    ${SHADER_COMMANDS}
    COMMENT "Compiling shaders ${TARGET_NAME}"
    SOURCES ${SHADER_SOURCE_FILES}
    BYPRODUCTS ${SHADER_PRODUCTS}
  )
endfunction()

add_subdirectory(src)
add_subdirectory(app)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR VK_VIDEO_BUILD_TESTING AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

