// based on nv_ray_tracing_basic from https://github.com/SaschaWillems/Vulkan

#version 460
#extension GL_NV_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) rayPayloadInNV vec3 hitValue;
hitAttributeNV vec2 baryCoord;

void main()
{
    // due to the Y flipping madness we have barycentric coordinates like this:
    //            *  (0, 0)
    //           * *
    //          *   *
    // (1, 0)  * * * *  (0, 1)
    // which gives a different result compared to the nv_ray_tracing_basic sample but this is expected.

    hitValue = vec3(1.0f - baryCoord.x - baryCoord.y, baryCoord.x, baryCoord.y);
}
