cmake_minimum_required(VERSION 3.31 FATAL_ERROR)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
set(CMAKE_CXX_MODULE_STD 1)

project(
    VkVideo
    VERSION 0.1.0
    DESCRIPTION "Vulkan Video decoding/encoding sample"
    LANGUAGES C CXX
)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    include(CTest)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        add_subdirectory(docs)
    else()
        message(STATUS "Doxygen not found, not building docs.")
    endif()
endif()

find_package(Vulkan COMPONENTS glslc REQUIRED)
find_package(WebP CONFIG)
find_package(
    FFmpeg
    COMPONENTS AVCODEC AVUTIL AVFORMAT SWSCALE SWRESAMPLE
    REQUIRED
)
find_package(PortAudioCpp REQUIRED)
add_subdirectory(extern)

add_library(vkvideo_VulkanHpp)
target_sources(
    vkvideo_VulkanHpp
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${Vulkan_INCLUDE_DIR}
        FILES ${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm
)
target_compile_features(vkvideo_VulkanHpp PUBLIC cxx_std_23)
target_link_libraries(vkvideo_VulkanHpp PUBLIC Vulkan::Vulkan)
target_compile_definitions(
    vkvideo_VulkanHpp
    PUBLIC VULKAN_HPP_NO_CONSTRUCTORS VULKAN_HPP_ENABLE_STD_MODULE
)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# source:
# https://thatonegamedev.com/cpp/cmake/how-to-compile-shaders-with-cmake/
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
        list(
            APPEND
            SHADER_COMMANDS
            "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv"
        )

        list(
            APPEND
            SHADER_PRODUCTS
            "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv"
        )
    endforeach()

    add_custom_target(
        ${TARGET_NAME}
        ALL
        ${SHADER_COMMANDS}
        COMMENT "Compiling shaders ${TARGET_NAME}"
        SOURCES ${SHADER_SOURCE_FILES}
        BYPRODUCTS ${SHADER_PRODUCTS}
    )
endfunction()

add_subdirectory(src)
add_subdirectory(app)

if(
    CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME
    OR VK_VIDEO_BUILD_TESTING
    AND BUILD_TESTING
)
    add_subdirectory(tests)
endif()
