#version 450

#extension GL_EXT_debug_printf : enable

const vec3 positions[3] = vec3[3](
        vec3(-1.0, -1.0, 0.0),
        vec3(3.0, -1.0, 0.0),
        vec3(-1.0, 3.0, 0.0)
    );

layout(location = 0) out vec2 tex_coords;

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 1.0);
    tex_coords = positions[gl_VertexIndex].xy * 0.5 + 0.5;
}
