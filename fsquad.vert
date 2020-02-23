#version 440

layout(location = 0) in vec4 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_texcoord;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    // input is Y up, we are Vulkan-only so correct for it always
    v_texcoord = vec2(texcoord.x, 1.0 - texcoord.y);
    gl_Position = position;
}
