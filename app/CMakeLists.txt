add_shaders(vkvideo_app_shaders shaders/fullscreen.vert shaders/fullscreen.frag)
add_executable(vkvideo_app main.cpp)
target_link_libraries(vkvideo_app PRIVATE vkvideo)
add_dependencies(vkvideo_app vkvideo_app_shaders)
