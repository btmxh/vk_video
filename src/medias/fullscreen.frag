#version 450

#extension GL_EXT_debug_printf : enable

layout(location = 0) in vec2 tex_coords;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2DArray tex;
layout(push_constant) uniform FrameInfo {
    vec2 uv_max;
    float frame_index;
};

void main() {
    color = texture(tex, vec3(tex_coords * uv_max, frame_index));
}
