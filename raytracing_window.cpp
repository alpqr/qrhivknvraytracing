/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

// Example of integrating VK_NV_ray_tracing into a QRhi-based application.

// Much of the raytracing pipeline setup is based on the nv_ray_tracing_basic
// sample from https://github.com/SaschaWillems/Vulkan

#include "raytracing_window.h"
#include <QVulkanFunctions>
#include <QFile>
#include <QtGui/private/qshader_p.h>

QShader getShader(const QString &name)
{
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());

    return QShader();
}

QByteArray getSpirv(const QString &name)
{
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return f.readAll();

    return QByteArray();
}

// All our vertex data both for raytracing and graphics follows OpenGL as is
// the Qt convention (so Y up, front face CCW). For graphics we correct for it
// in the fragment shader and via QRhi's clipSpaceCorrMatrix(), while for
// raytracing the flipping to Vulkan Y down is handled by the instance
// transform (and not the geometry transform, to avoid changing the winding
// order - the default front=CW based (?) culling will just just work due to
// winding order being determined in Y down object space against our Y up
// front=CCW data... or whatever)

static float vertexData[] = {
     0.0f,   1.0f,  0.0f,
    -1.0f,  -1.0f,  0.0f,
     1.0f,  -1.0f,  0.0f
};

static float quadVertexAndCoordData[] = {
  -1.0f,   1.0f,   0.0f, 0.0f,
  -1.0f,  -1.0f,   0.0f, 1.0f,
   1.0f,  -1.0f,   1.0f, 1.0f,
  -1.0f,   1.0f,   0.0f, 0.0f,
   1.0f,  -1.0f,   1.0f, 1.0f,
   1.0f,   1.0f,   1.0f, 0.0f
};

RaytracingWindow::RaytracingWindow()
{
    // ### ugh! this really needs a better solution [QRhi TODO]
    qputenv("QT_VULKAN_DEVICE_EXTENSIONS", "VK_KHR_get_memory_requirements2;VK_NV_ray_tracing");
}

RaytracingWindow::~RaytracingWindow()
{
    const QRhiVulkanNativeHandles *h = static_cast<const QRhiVulkanNativeHandles *>(m_rhi->nativeHandles());
    QVulkanDeviceFunctions *df = vulkanInstance()->deviceFunctions(h->dev);

    df->vkDestroyDescriptorPool(h->dev, m_rayDescPool, nullptr);
    df->vkDestroyDescriptorSetLayout(h->dev, m_rayDescSetLayout, nullptr);
    df->vkDestroyPipelineLayout(h->dev, m_rayPipelineLayout, nullptr);
    df->vkDestroyPipeline(h->dev, m_rayPipeline, nullptr);

    destroyAccelerationStructure(h->dev, m_blas, nullptr);
    df->vkFreeMemory(h->dev, m_blasMem, nullptr);
    destroyAccelerationStructure(h->dev, m_tlas, nullptr);
    df->vkFreeMemory(h->dev, m_tlasMem, nullptr);

#if 0
    df->vkFreeMemory(h->dev, m_geometryTransformBufMem, nullptr);
    df->vkDestroyBuffer(h->dev, m_geometryTransformBuf, nullptr);
#endif
    df->vkFreeMemory(h->dev, m_scratchBufMem, nullptr);
    df->vkDestroyBuffer(h->dev, m_scratchBuf, nullptr);
    df->vkFreeMemory(h->dev, m_instanceBufMem, nullptr);
    df->vkDestroyBuffer(h->dev, m_instanceBuf, nullptr);
    df->vkFreeMemory(h->dev, m_sbtBufMem, nullptr);
    df->vkDestroyBuffer(h->dev, m_sbtBuf, nullptr);

    for (VkImageView v : m_imageViews)
        df->vkDestroyImageView(h->dev, v, nullptr);
}

struct GeometryInstance // as per spec
{
    float transform[12]; // 4x3 row major
    uint32_t instanceCustomIndex  : 24;
    uint32_t mask : 8;
    uint32_t instanceOffset : 24;
    uint32_t flags : 8;
    uint64_t accelerationStructureHandle;
};

void RaytracingWindow::customInit()
{
    Q_ASSERT(m_rhi->resourceLimit(QRhi::FramesInFlight) == 2); // not prepared to handle other values

    const QRhiVulkanNativeHandles *h = static_cast<const QRhiVulkanNativeHandles *>(m_rhi->nativeHandles());
    QVulkanInstance *inst = vulkanInstance();
    QVulkanFunctions *f = inst->functions();
    QVulkanDeviceFunctions *df = inst->deviceFunctions(h->dev);

    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                inst->getInstanceProcAddr("vkGetPhysicalDeviceProperties2"));

    m_raytracingProps = {};
    m_raytracingProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PROPERTIES_NV;
    VkPhysicalDeviceProperties2 deviceProps2 = {};
    deviceProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProps2.pNext = &m_raytracingProps;
    getPhysicalDeviceProperties2(h->physDev, &deviceProps2);

    qDebug("shaderGroupHandleSize: %u\nmaxRecursionDepth: %u\nmaxShaderGroupStride: %u\nshaderGroupBaseAlignment: %u\n"
           "maxGeometryCount: %llu\nmaxInstanceCount: %llu\nmaxTriangleCount: %llu\nmaxDescriptorSetAccelerationStructures: %u",
           m_raytracingProps.shaderGroupHandleSize,
           m_raytracingProps.maxRecursionDepth,
           m_raytracingProps.maxShaderGroupStride,
           m_raytracingProps.shaderGroupBaseAlignment,
           m_raytracingProps.maxGeometryCount,
           m_raytracingProps.maxInstanceCount,
           m_raytracingProps.maxTriangleCount,
           m_raytracingProps.maxDescriptorSetAccelerationStructures);

    createAccelerationStructure = reinterpret_cast<PFN_vkCreateAccelerationStructureNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkCreateAccelerationStructureNV"));
    destroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkDestroyAccelerationStructureNV"));
    bindAccelerationStructureMemory = reinterpret_cast<PFN_vkBindAccelerationStructureMemoryNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkBindAccelerationStructureMemoryNV"));
    getAccelerationStructureHandle = reinterpret_cast<PFN_vkGetAccelerationStructureHandleNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkGetAccelerationStructureHandleNV"));
    getAccelerationStructureMemoryRequirements = reinterpret_cast<PFN_vkGetAccelerationStructureMemoryRequirementsNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkGetAccelerationStructureMemoryRequirementsNV"));
    cmdBuildAccelerationStructure = reinterpret_cast<PFN_vkCmdBuildAccelerationStructureNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkCmdBuildAccelerationStructureNV"));
    createRayTracingPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkCreateRayTracingPipelinesNV"));
    getRayTracingShaderGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkGetRayTracingShaderGroupHandlesNV"));
    cmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysNV>(
                f->vkGetDeviceProcAddr(h->dev, "vkCmdTraceRaysNV"));

    m_vbufReady = false;

    m_tex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, size() * devicePixelRatio(), 1, QRhiTexture::UsedWithLoadStore));
    m_tex->build();

    m_quadVbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quadVertexAndCoordData)));
    m_quadVbuf->build();

    m_quadSampler.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_quadSampler->build();

    m_quadSrb.reset(m_rhi->newShaderResourceBindings());
    m_quadSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, m_tex.get(), m_quadSampler.get())
    });
    m_quadSrb->build();

    m_quadPs.reset(m_rhi->newGraphicsPipeline());
    m_quadPs->setShaderStages({
        { QRhiShaderStage::Vertex, getShader(QLatin1String(":/fsquad.vert.qsb")) },
        { QRhiShaderStage::Fragment, getShader(QLatin1String(":/fsquad.frag.qsb")) }
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        { 4 * sizeof(float) }
    });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) }
    });
    m_quadPs->setVertexInputLayout(inputLayout);
    m_quadPs->setShaderResourceBindings(m_quadSrb.get());
    m_quadPs->setRenderPassDescriptor(m_rp.get());
    m_quadPs->build();

    // Now onto the raytracing resources

    // Say no to boilerplate; will use QRhi and dig out the VkBuffers afterwards (same goes for the image)
    m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(vertexData)));
    m_vbuf->build();

    VkBuffer vbuf = *reinterpret_cast<const VkBuffer *>(m_vbuf->nativeBuffer().objects[0]);

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64 * 2));
    m_ubuf->build();

    // but sadly, no help from QRhi from this point on. so much for no boilerplate..

    VkPhysicalDeviceMemoryProperties memProps;
    f->vkGetPhysicalDeviceMemoryProperties(h->physDev, &memProps);

    const VkDescriptorPoolSize descPoolSizes[] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 }
    };
    VkDescriptorPoolCreateInfo descPoolInfo = {};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.maxSets = 2;
    descPoolInfo.poolSizeCount = sizeof(descPoolSizes) / sizeof(descPoolSizes[0]);
    descPoolInfo.pPoolSizes = descPoolSizes;
    df->vkCreateDescriptorPool(h->dev, &descPoolInfo, nullptr, &m_rayDescPool);

    VkDescriptorSetLayoutBinding accelerationStructureLayoutBinding = {};
    accelerationStructureLayoutBinding.binding = 0;
    accelerationStructureLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;
    accelerationStructureLayoutBinding.descriptorCount = 1;
    accelerationStructureLayoutBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

    VkDescriptorSetLayoutBinding resultImageLayoutBinding = {};
    resultImageLayoutBinding.binding = 1;
    resultImageLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    resultImageLayoutBinding.descriptorCount = 1;
    resultImageLayoutBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

    VkDescriptorSetLayoutBinding uniformBufferBinding = {};
    uniformBufferBinding.binding = 2;
    uniformBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformBufferBinding.descriptorCount = 1;
    uniformBufferBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

    const VkDescriptorSetLayoutBinding bindings[] = {
        accelerationStructureLayoutBinding,
        resultImageLayoutBinding,
        uniformBufferBinding
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
    layoutInfo.pBindings = bindings;
    df->vkCreateDescriptorSetLayout(h->dev, &layoutInfo, nullptr, &m_rayDescSetLayout);

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &m_rayDescSetLayout;
    df->vkCreatePipelineLayout(h->dev, &pipelineLayoutCreateInfo, nullptr, &m_rayPipelineLayout);

    VkShaderModule shaderModules[3];
    QByteArray spirv = getSpirv(QLatin1String(":/raygen.spv"));
    VkShaderModuleCreateInfo shaderInfo = {};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = size_t(spirv.size());
    shaderInfo.pCode = reinterpret_cast<const quint32 *>(spirv.constData());
    df->vkCreateShaderModule(h->dev, &shaderInfo, nullptr, &shaderModules[0]);
    spirv = getSpirv(QLatin1String(":/miss.spv"));
    shaderInfo.codeSize = size_t(spirv.size());
    shaderInfo.pCode = reinterpret_cast<const quint32 *>(spirv.constData());
    df->vkCreateShaderModule(h->dev, &shaderInfo, nullptr, &shaderModules[1]);
    spirv = getSpirv(QLatin1String(":/closesthit.spv"));
    shaderInfo.codeSize = size_t(spirv.size());
    shaderInfo.pCode = reinterpret_cast<const quint32 *>(spirv.constData());
    df->vkCreateShaderModule(h->dev, &shaderInfo, nullptr, &shaderModules[2]);

    VkPipelineShaderStageCreateInfo shaderStages[3] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_NV;
    shaderStages[0].module = shaderModules[0];
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_MISS_BIT_NV;
    shaderStages[1].module = shaderModules[1];
    shaderStages[1].pName = "main";
    shaderStages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV;
    shaderStages[2].module = shaderModules[2];
    shaderStages[2].pName = "main";

    VkRayTracingShaderGroupCreateInfoNV shaderGroupInfo[3] = {};
    for (int i = 0; i < 3; ++i) {
        shaderGroupInfo[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV;
        shaderGroupInfo[i].generalShader = VK_SHADER_UNUSED_NV;
        shaderGroupInfo[i].closestHitShader = VK_SHADER_UNUSED_NV;
        shaderGroupInfo[i].anyHitShader = VK_SHADER_UNUSED_NV;
        shaderGroupInfo[i].intersectionShader = VK_SHADER_UNUSED_NV;
    }
    shaderGroupInfo[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
    shaderGroupInfo[0].generalShader = 0; // raygen
    shaderGroupInfo[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
    shaderGroupInfo[1].generalShader = 1; // miss
    shaderGroupInfo[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_NV;
    shaderGroupInfo[2].closestHitShader = 2; // closesthit

    VkRayTracingPipelineCreateInfoNV rayPipelineInfo = {};
    rayPipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_NV;
    rayPipelineInfo.stageCount = 3;
    rayPipelineInfo.pStages = shaderStages;
    rayPipelineInfo.groupCount = 3;
    rayPipelineInfo.pGroups = shaderGroupInfo;
    rayPipelineInfo.maxRecursionDepth = 1;
    rayPipelineInfo.layout = m_rayPipelineLayout;
    VkResult err = createRayTracingPipelines(h->dev, VK_NULL_HANDLE, 1, &rayPipelineInfo, nullptr, &m_rayPipeline);
    if (err != VK_SUCCESS)
        qFatal("Failed to create raytracing pipeline: %d", err);

    for (int i = 0; i < 3; ++i)
        df->vkDestroyShaderModule(h->dev, shaderModules[i], nullptr);

    // common stuff for buffers
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.usage = VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
    VkMemoryRequirements bufMemReq;
    VkMemoryAllocateInfo bufMemAllocInfo = {};
    bufMemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    quint8 *p;
    auto findMemTypeIndex = [&bufMemReq, &memProps](uint32_t wantedBits) {
        uint32_t memTypeIndex = 0;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (bufMemReq.memoryTypeBits & (1 << i)) {
                if ((memProps.memoryTypes[i].propertyFlags & wantedBits) == wantedBits) {
                    memTypeIndex = i;
                    break;
                }
            }
        }
        return memTypeIndex;
    };

#if 0
    // geometry transform buffer
    bufInfo.size = 12 * sizeof(float);
    df->vkCreateBuffer(h->dev, &bufInfo, nullptr, &m_geometryTransformBuf);
    df->vkGetBufferMemoryRequirements(h->dev, m_geometryTransformBuf, &bufMemReq);
    bufMemAllocInfo.allocationSize = bufMemReq.size;
    bufMemAllocInfo.memoryTypeIndex = findMemTypeIndex(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    err = df->vkAllocateMemory(h->dev, &bufMemAllocInfo, nullptr, &m_geometryTransformBufMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate geometry transform buffer memory: %d", err);
    df->vkBindBufferMemory(h->dev, m_geometryTransformBuf, m_geometryTransformBufMem, 0);

    static const float flip4x3RowMajor[12] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };
    df->vkMapMemory(h->dev, m_geometryTransformBufMem, 0, 12 * sizeof(float), 0, reinterpret_cast<void **>(&p));
    memcpy(p, flip4x3RowMajor, 12 * sizeof(float));
    df->vkUnmapMemory(h->dev, m_geometryTransformBufMem);
#endif

    // the geometry
    m_geometry = {};
    m_geometry.sType = VK_STRUCTURE_TYPE_GEOMETRY_NV;
    m_geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_NV;
    m_geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV;
    m_geometry.geometry.triangles.vertexData = vbuf;
    m_geometry.geometry.triangles.vertexOffset = 0;
    m_geometry.geometry.triangles.vertexCount = 3;
    m_geometry.geometry.triangles.vertexStride = 3 * sizeof(float);
    m_geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    m_geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_NV;
#if 0
    m_geometry.geometry.triangles.transformData = m_geometryTransformBuf;
#endif
    m_geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV;
    m_geometry.flags = VK_GEOMETRY_OPAQUE_BIT_NV;

    // bottom level acceleration structure
    VkAccelerationStructureInfoNV accelInfo = {};
    accelInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
    accelInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
    accelInfo.instanceCount = 0;
    accelInfo.geometryCount = 1;
    accelInfo.pGeometries = &m_geometry;

    VkAccelerationStructureCreateInfoNV accelCreateInfo = {};
    accelCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
    accelCreateInfo.info = accelInfo;
    err = createAccelerationStructure(h->dev, &accelCreateInfo, nullptr, &m_blas);
    if (err != VK_SUCCESS)
        qFatal("Failed to create bottom level acceleration structure: %d", err);

    VkAccelerationStructureMemoryRequirementsInfoNV memReqInfo = {};
    memReqInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
    memReqInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV;
    memReqInfo.accelerationStructure = m_blas;
    VkMemoryRequirements2 memReq = {};
    getAccelerationStructureMemoryRequirements(h->dev, &memReqInfo, &memReq);
    qDebug("blas memory needed: %llu", memReq.memoryRequirements.size);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemTypeIndex(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    err = df->vkAllocateMemory(h->dev, &allocInfo, nullptr, &m_blasMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate memory for bottom level acceleration structure: %d", err);

    VkBindAccelerationStructureMemoryInfoNV accelMemInfo = {};
    accelMemInfo.sType = VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
    accelMemInfo.accelerationStructure = m_blas;
    accelMemInfo.memory = m_blasMem;
    bindAccelerationStructureMemory(h->dev, 1, &accelMemInfo);

    getAccelerationStructureHandle(h->dev, m_blas, sizeof(uint64_t), &m_blasHandle);

    // top level acceleration structure
    accelInfo = {};
    accelInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
    accelInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
    accelInfo.instanceCount = 1;
    accelInfo.geometryCount = 0;

    accelCreateInfo = {};
    accelCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
    accelCreateInfo.info = accelInfo;
    err = createAccelerationStructure(h->dev, &accelCreateInfo, nullptr, &m_tlas);
    if (err != VK_SUCCESS)
        qFatal("Failed to create top level acceleration structure: %d", err);

    memReqInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV;
    memReqInfo.accelerationStructure = m_tlas;
    getAccelerationStructureMemoryRequirements(h->dev, &memReqInfo, &memReq);
    qDebug("tlas memory needed: %llu", memReq.memoryRequirements.size);

    allocInfo.allocationSize = memReq.memoryRequirements.size;
    err = df->vkAllocateMemory(h->dev, &allocInfo, nullptr, &m_tlasMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate memory for top level acceleration structure: %d", err);

    accelMemInfo.accelerationStructure = m_tlas;
    accelMemInfo.memory = m_tlasMem;
    bindAccelerationStructureMemory(h->dev, 1, &accelMemInfo);

    getAccelerationStructureHandle(h->dev, m_tlas, sizeof(uint64_t), &m_tlasHandle);

    // scratch buffer for vkCmdBuildAccelerationStructureNV
    VkDeviceSize scratchSize = 0;
    memReqInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV;
    memReqInfo.accelerationStructure = m_blas;
    getAccelerationStructureMemoryRequirements(h->dev, &memReqInfo, &memReq);
    qDebug("blas scratch buffer size: %llu", memReq.memoryRequirements.size);
    scratchSize = memReq.memoryRequirements.size;
    memReqInfo.accelerationStructure = m_tlas;
    getAccelerationStructureMemoryRequirements(h->dev, &memReqInfo, &memReq);
    qDebug("tlas scratch buffer size: %llu", memReq.memoryRequirements.size);
    scratchSize = qMax(scratchSize, memReq.memoryRequirements.size);

    bufInfo.size = scratchSize;
    df->vkCreateBuffer(h->dev, &bufInfo, nullptr, &m_scratchBuf);
    df->vkGetBufferMemoryRequirements(h->dev, m_scratchBuf, &bufMemReq);
    bufMemAllocInfo.allocationSize = bufMemReq.size;
    bufMemAllocInfo.memoryTypeIndex = findMemTypeIndex(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    err = df->vkAllocateMemory(h->dev, &bufMemAllocInfo, nullptr, &m_scratchBufMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate scratch buffer memory: %d", err);
    df->vkBindBufferMemory(h->dev, m_scratchBuf, m_scratchBufMem, 0);

    static const float modelMatrix4x3RowMajor[12] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    // instance buffer (the TLAS has only 1 instance in this example)
    Q_ASSERT(sizeof(GeometryInstance) == 64);
    GeometryInstance instance = {};
    memcpy(instance.transform, modelMatrix4x3RowMajor, 12 * sizeof(float));
    instance.mask = 0xFF;
    //instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_CULL_DISABLE_BIT_NV;
    //instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_NV;
    instance.accelerationStructureHandle = m_blasHandle;

    bufInfo.size = sizeof(GeometryInstance);
    df->vkCreateBuffer(h->dev, &bufInfo, nullptr, &m_instanceBuf);
    df->vkGetBufferMemoryRequirements(h->dev, m_instanceBuf, &bufMemReq);
    bufMemAllocInfo.allocationSize = bufMemReq.size;
    bufMemAllocInfo.memoryTypeIndex = findMemTypeIndex(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    err = df->vkAllocateMemory(h->dev, &bufMemAllocInfo, nullptr, &m_instanceBufMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate instance buffer memory: %d", err);
    df->vkBindBufferMemory(h->dev, m_instanceBuf, m_instanceBufMem, 0);

    df->vkMapMemory(h->dev, m_instanceBufMem, 0, sizeof(GeometryInstance), 0, reinterpret_cast<void **>(&p));
    memcpy(p, &instance, sizeof(GeometryInstance));
    df->vkUnmapMemory(h->dev, m_instanceBufMem);

    // buffer for shader binding table
    const uint32_t sghSize = m_raytracingProps.shaderGroupHandleSize;
    const uint32_t sbtSize = sghSize * 3;
    bufInfo.size = sbtSize;
    df->vkCreateBuffer(h->dev, &bufInfo, nullptr, &m_sbtBuf);
    df->vkGetBufferMemoryRequirements(h->dev, m_sbtBuf, &bufMemReq);
    bufMemAllocInfo.allocationSize = bufMemReq.size;
    bufMemAllocInfo.memoryTypeIndex = findMemTypeIndex(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    err = df->vkAllocateMemory(h->dev, &bufMemAllocInfo, nullptr, &m_sbtBufMem);
    if (err != VK_SUCCESS)
        qFatal("Failed to allocate shader binding table buffer memory: %d", err);
    df->vkBindBufferMemory(h->dev, m_sbtBuf, m_sbtBufMem, 0);

    QVector<quint8> shaderHandles;
    shaderHandles.resize(sbtSize);
    getRayTracingShaderGroupHandles(h->dev, m_rayPipeline, 0, 3, sbtSize, shaderHandles.data());

    df->vkMapMemory(h->dev, m_sbtBufMem, 0, sbtSize, 0, reinterpret_cast<void **>(&p));
    memcpy(p, shaderHandles.constData(), sbtSize);
    df->vkUnmapMemory(h->dev, m_sbtBufMem);

    // descriptor sets (have to deal with double buffering)
    VkDescriptorSetAllocateInfo descSetAllocInfo = {};
    descSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descSetAllocInfo.descriptorPool = m_rayDescPool;
    descSetAllocInfo.descriptorSetCount = 2;
    VkDescriptorSetLayout descSetLayouts[2] = { m_rayDescSetLayout, m_rayDescSetLayout };
    descSetAllocInfo.pSetLayouts = descSetLayouts;
    df->vkAllocateDescriptorSets(h->dev, &descSetAllocInfo, m_rayDescSet);

    m_needsRayBuild = true;
}

void RaytracingWindow::customRender()
{
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    if (!m_vbufReady) {
        m_vbufReady = true;
        u->uploadStaticBuffer(m_quadVbuf.get(), quadVertexAndCoordData);
        u->uploadStaticBuffer(m_vbuf.get(), vertexData);
    }
    if (m_matricesChanged) {
        u->updateDynamicBuffer(m_ubuf.get(), 0, 64, m_rayViewInverse.constData());
        u->updateDynamicBuffer(m_ubuf.get(), 64, 64, m_rayProjInverse.constData());
    }

    QRhiCommandBuffer *cb = m_sc->currentFrameCommandBuffer();
    cb->resourceUpdate(u);
    u = nullptr;

    const QSize outputSizeInPixels = m_sc->currentPixelSize();
    if (m_tex->pixelSize() != outputSizeInPixels) {
        m_tex->setPixelSize(outputSizeInPixels);
        m_tex->build();
    }

    const QRhiVulkanNativeHandles *h = static_cast<const QRhiVulkanNativeHandles *>(m_rhi->nativeHandles());
    QVulkanDeviceFunctions *df = vulkanInstance()->deviceFunctions(h->dev);

    if (m_needsRayBuild) {
        m_needsRayBuild = false;

        cb->beginExternal();
        VkCommandBuffer commandBuffer = static_cast<const QRhiVulkanCommandBufferNativeHandles *>(cb->nativeHandles())->commandBuffer;

        // build bottom level acceleration structure
        VkAccelerationStructureInfoNV buildInfo = {};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &m_geometry;
        cmdBuildAccelerationStructure(commandBuffer, &buildInfo, VK_NULL_HANDLE, 0, VK_FALSE, m_blas, VK_NULL_HANDLE, m_scratchBuf, 0);

        // because scratch is reused
        VkMemoryBarrier memoryBarrier = {};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
        memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
        df->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV,
                                 0, 1, &memoryBarrier, 0, 0, 0, 0);

        // build top level acceleration structure
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
        buildInfo.instanceCount = 1;
        buildInfo.geometryCount = 0;
        buildInfo.pGeometries = nullptr;
        cmdBuildAccelerationStructure(commandBuffer, &buildInfo, m_instanceBuf, 0, VK_FALSE, m_tlas, VK_NULL_HANDLE, m_scratchBuf, 0);

        df->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV,
                                 0, 1, &memoryBarrier, 0, 0, 0, 0);

        cb->endExternal();
    }

    VkImage image = *reinterpret_cast<const VkImage *>(m_tex->nativeTexture().object);
    // ### the image view should be retrievable from the QRhiTexture, the one we use for graphics would be suitable as-is [QRhi TODO]
    if (image != m_lastImage) {
        m_lastImage = image;
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView v;
        df->vkCreateImageView(h->dev, &viewInfo, nullptr, &v);
        m_imageViews.append(v);
    }
    VkImageView imageView = m_imageViews.last();

    const int currentFrameSlot = m_rhi->currentFrameSlot();

    // Dynamic QRhiBuffers are backed by multiple native buffers, pick the current one
    // ### this relies on the fact the nativeBuffer() executes pending host writes [QRhi TODO]
    VkBuffer ubuf = *reinterpret_cast<const VkBuffer *>(m_ubuf->nativeBuffer().objects[currentFrameSlot]);

    {
        VkWriteDescriptorSet writeDescSet[3] = {};

        VkWriteDescriptorSetAccelerationStructureNV accelWriteDescSet = {};
        accelWriteDescSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV;
        accelWriteDescSet.accelerationStructureCount = 1;
        accelWriteDescSet.pAccelerationStructures = &m_tlas;
        writeDescSet[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescSet[0].pNext = &accelWriteDescSet;
        writeDescSet[0].dstSet = m_rayDescSet[currentFrameSlot];
        writeDescSet[0].dstBinding = 0;
        writeDescSet[0].descriptorCount = 1;
        writeDescSet[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;

        VkDescriptorImageInfo descImageInfo = {};
        descImageInfo.imageView = imageView;
        descImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writeDescSet[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescSet[1].dstSet = m_rayDescSet[currentFrameSlot];
        writeDescSet[1].dstBinding = 1;
        writeDescSet[1].descriptorCount = 1;
        writeDescSet[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeDescSet[1].pImageInfo = &descImageInfo;

        VkDescriptorBufferInfo descBufferInfo = {};
        descBufferInfo.buffer = ubuf;
        descBufferInfo.range = m_ubuf->size();

        writeDescSet[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescSet[2].dstSet = m_rayDescSet[currentFrameSlot];
        writeDescSet[2].dstBinding = 2;
        writeDescSet[2].descriptorCount = 1;
        writeDescSet[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writeDescSet[2].pBufferInfo = &descBufferInfo;

        df->vkUpdateDescriptorSets(h->dev, 3, writeDescSet, 0, nullptr);
    }

    {
        // Raytracing pass: writes to m_tex
        cb->beginExternal();
        VkCommandBuffer commandBuffer = static_cast<const QRhiVulkanCommandBufferNativeHandles *>(cb->nativeHandles())->commandBuffer;

        VkImageMemoryBarrier imageBarrier = {};
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarrier.subresourceRange.baseMipLevel = 0;
        imageBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        imageBarrier.subresourceRange.baseArrayLayer = 0;
        imageBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        imageBarrier.image = image;

        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.srcAccessMask = 0;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        df->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &imageBarrier);

        df->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, m_rayPipeline);
        df->vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, m_rayPipelineLayout, 0, 1, &m_rayDescSet[currentFrameSlot], 0, nullptr);

        const uint32_t sghSize = m_raytracingProps.shaderGroupHandleSize;
        VkDeviceSize bindingOffsetRayGenShader = 0;
        VkDeviceSize bindingOffsetMissShader = sghSize;
        VkDeviceSize bindingOffsetHitShader = 2 * sghSize;

        cmdTraceRays(commandBuffer,
                     m_sbtBuf, bindingOffsetRayGenShader,
                     m_sbtBuf, bindingOffsetMissShader, sghSize,
                     m_sbtBuf, bindingOffsetHitShader, sghSize,
                     VK_NULL_HANDLE, 0, 0,
                     m_tex->pixelSize().width(), m_tex->pixelSize().height(), 1);

        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        df->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &imageBarrier);

        cb->endExternal();
    }

    // Render pass: draw a quad textured with m_tex
    cb->beginPass(m_sc->currentFrameRenderTarget(), Qt::white, { 1.0f, 0 });
    cb->setGraphicsPipeline(m_quadPs.get());
    cb->setShaderResources();
    cb->setViewport({ 0, 0, float(outputSizeInPixels.width()), float(outputSizeInPixels.height()) });
    const QRhiCommandBuffer::VertexInput vbufBinding(m_quadVbuf.get(), 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(6);
    cb->endPass();
}
