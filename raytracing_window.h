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

#ifndef RAYTRACINGWINDOW_H
#define RAYTRACINGWINDOW_H

#include "window.h"

class RaytracingWindow : public Window
{
public:
    RaytracingWindow();
    ~RaytracingWindow();

    void customInit() override;
    void customRender() override;

private:
    VkPhysicalDeviceRayTracingPropertiesNV m_raytracingProps;
    PFN_vkCreateAccelerationStructureNV createAccelerationStructure;
    PFN_vkDestroyAccelerationStructureNV destroyAccelerationStructure;
    PFN_vkBindAccelerationStructureMemoryNV bindAccelerationStructureMemory;
    PFN_vkGetAccelerationStructureHandleNV getAccelerationStructureHandle;
    PFN_vkGetAccelerationStructureMemoryRequirementsNV getAccelerationStructureMemoryRequirements;
    PFN_vkCmdBuildAccelerationStructureNV cmdBuildAccelerationStructure;
    PFN_vkCreateRayTracingPipelinesNV createRayTracingPipelines;
    PFN_vkGetRayTracingShaderGroupHandlesNV getRayTracingShaderGroupHandles;
    PFN_vkCmdTraceRaysNV cmdTraceRays;

    std::unique_ptr<QRhiBuffer> m_quadVbuf;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    bool m_vbufReady;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_quadSampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_quadSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_quadPs;

    VkDescriptorPool m_rayDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_rayDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_rayPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_rayPipeline = VK_NULL_HANDLE;
    VkDescriptorSet m_rayDescSet[2] = {};
    VkAccelerationStructureNV m_blas = VK_NULL_HANDLE;
    VkDeviceMemory m_blasMem = VK_NULL_HANDLE;
    uint64_t m_blasHandle;
    VkAccelerationStructureNV m_tlas = VK_NULL_HANDLE;
    VkDeviceMemory m_tlasMem = VK_NULL_HANDLE;
    uint64_t m_tlasHandle;
#if 0
    VkBuffer m_geometryTransformBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_geometryTransformBufMem = VK_NULL_HANDLE;
#endif
    VkBuffer m_scratchBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_scratchBufMem = VK_NULL_HANDLE;
    bool m_needsRayBuild;
    VkGeometryNV m_geometry;
    VkBuffer m_instanceBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_instanceBufMem = VK_NULL_HANDLE;
    VkBuffer m_sbtBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_sbtBufMem = VK_NULL_HANDLE;

    QVector<VkImageView> m_imageViews;
    VkImage m_lastImage = VK_NULL_HANDLE;
};

#endif
