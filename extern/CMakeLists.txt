find_package(glfw3 REQUIRED)
add_library(vkfw INTERFACE "${PROJECT_SOURCE_DIR}/extern/vkfw/include/vkfw/vkfw.hpp")
target_include_directories(vkfw INTERFACE "${PROJECT_SOURCE_DIR}/extern/vkfw/include")
target_link_libraries(vkfw INTERFACE glfw)
target_compile_definitions(vkfw INTERFACE VKFW_NO_STRUCT_CONSTRUCTORS)

set(AV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AV_ENABLE_SHARED  OFF CACHE BOOL "" FORCE)
add_subdirectory(avcpp)

add_subdirectory(vk-bootstrap)
