find_package(glfw3 REQUIRED)
add_library(vkfw INTERFACE "${PROJECT_SOURCE_DIR}/extern/vkfw/include/vkfw/vkfw.hpp")
target_include_directories(vkfw INTERFACE "${PROJECT_SOURCE_DIR}/extern/vkfw/include")
target_link_libraries(vkfw INTERFACE glfw)
target_compile_definitions(vkfw INTERFACE VKFW_NO_STRUCT_CONSTRUCTORS)

add_subdirectory(vk-bootstrap)
add_subdirectory(mcpp-unique-any)
