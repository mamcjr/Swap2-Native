#version 460
#extension GL_EXT_mesh_shader : require
layout(location=0) perprimitiveEXT in vec4 tint;
layout(location=0) out vec4 color;
void main() { color=tint; }
