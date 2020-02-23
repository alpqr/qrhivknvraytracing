// based on nv_ray_tracing_basic from https://github.com/SaschaWillems/Vulkan

#version 460
#extension GL_NV_ray_tracing : require

layout(location = 0) rayPayloadInNV vec3 hitValue;

void main()
{
    hitValue = vec3(0.0, 0.0, 0.2);
}
