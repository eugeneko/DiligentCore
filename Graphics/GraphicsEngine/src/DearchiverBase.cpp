/*
 *  Copyright 2019-2022 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "DearchiverBase.hpp"
#include "ArchiveFileImpl.hpp"
#include "ArchiveMemoryImpl.hpp"
#include "PipelineStateBase.hpp"
#include "PSOSerializer.hpp"

namespace Diligent
{

#define CHECK_UNPACK_PARAMATER(Expr, ...)   \
    do                                      \
    {                                       \
        DEV_CHECK_ERR(Expr, ##__VA_ARGS__); \
        if (!(Expr))                        \
            return false;                   \
    } while (false)

namespace
{

bool VerifyPipelineStateUnpackInfo(const PipelineStateUnpackInfo& DeArchiveInfo, IPipelineState** ppPSO)
{
#define CHECK_UNPACK_PSO_PARAM(Expr, ...) CHECK_UNPACK_PARAMATER("Invalid PSO unpack parameter: ", ##__VA_ARGS__)
    CHECK_UNPACK_PSO_PARAM(ppPSO != nullptr, "ppPSO must not be null");
    CHECK_UNPACK_PSO_PARAM(DeArchiveInfo.pArchive != nullptr, "pArchive must not be null");
    CHECK_UNPACK_PSO_PARAM(DeArchiveInfo.Name != nullptr, "Name must not be null");
    CHECK_UNPACK_PSO_PARAM(DeArchiveInfo.pDevice != nullptr, "pDevice must not be null");
    CHECK_UNPACK_PSO_PARAM(DeArchiveInfo.PipelineType <= PIPELINE_TYPE_LAST, "PipelineType must be valid");
#undef CHECK_UNPACK_PSO_PARAM

    return true;
}

bool VerifyResourceSignatureUnpackInfo(const ResourceSignatureUnpackInfo& DeArchiveInfo, IPipelineResourceSignature** ppSignature)
{
#define CHECK_UNPACK_SIGN_PARAM(Expr, ...) CHECK_UNPACK_PARAMATER("Invalid signature unpack parameter: ", ##__VA_ARGS__)
    CHECK_UNPACK_SIGN_PARAM(ppSignature != nullptr, "ppSignature must not be null");
    CHECK_UNPACK_SIGN_PARAM(DeArchiveInfo.pArchive != nullptr, "pArchive must not be null");
    CHECK_UNPACK_SIGN_PARAM(DeArchiveInfo.Name != nullptr, "Name must not be null");
    CHECK_UNPACK_SIGN_PARAM(DeArchiveInfo.pDevice != nullptr, "pDevice must not be null");
#undef CHECK_UNPACK_SIGN_PARAM

    return true;
}

bool VerifyRenderPassUnpackInfo(const RenderPassUnpackInfo& DeArchiveInfo, IRenderPass** ppRP)
{
#define CHECK_UNPACK_RENDER_PASS_PARAM(Expr, ...) CHECK_UNPACK_PARAMATER("Invalid signature unpack parameter: ", ##__VA_ARGS__)
    CHECK_UNPACK_RENDER_PASS_PARAM(ppRP != nullptr, "ppRP must not be null");
    CHECK_UNPACK_RENDER_PASS_PARAM(DeArchiveInfo.pArchive != nullptr, "pArchive must not be null");
    CHECK_UNPACK_RENDER_PASS_PARAM(DeArchiveInfo.Name != nullptr, "Name must not be null");
    CHECK_UNPACK_RENDER_PASS_PARAM(DeArchiveInfo.pDevice != nullptr, "pDevice must not be null");
#undef CHECK_UNPACK_RENDER_PASS_PARAM

    return true;
}

} // namespace


DearchiverBase::DeviceType DearchiverBase::GetArchiveDeviceType(const IRenderDevice* pDevice)
{
    VERIFY_EXPR(pDevice != nullptr);
    const auto Type = pDevice->GetDeviceInfo().Type;
    return DeviceObjectArchiveBase::RenderDeviceTypeToArchiveDeviceType(Type);
}

template <typename CreateInfoType>
struct DearchiverBase::PSOData
{
    DynamicLinearAllocator Allocator;
    const PSODataHeader*   pHeader = nullptr;
    CreateInfoType         CreateInfo{};
    PSOCreateInternalInfo  InternalCI;
    SerializedPSOAuxData   AuxData;
    TPRSNames              PRSNames{};
    const char*            RenderPassName = nullptr;

    // Strong references to pipeline resource signatures, render pass, etc.
    std::vector<RefCntAutoPtr<IDeviceObject>> Objects;
    std::vector<RefCntAutoPtr<IShader>>       Shaders;

    static const ChunkType ExpectedChunkType;

    explicit PSOData(IMemoryAllocator& Allocator, Uint32 BlockSize = 2 << 10) :
        Allocator{Allocator, BlockSize}
    {}

    bool Deserialize(const char* Name, Serializer<SerializerMode::Read>& Ser);
    void AssignShaders();
    void CreatePipeline(IRenderDevice* pDevice, IPipelineState** ppPSO);

private:
    void DeserializeInternal(Serializer<SerializerMode::Read>& Ser);
};


struct DearchiverBase::RPData
{
    DynamicLinearAllocator Allocator;
    const RPDataHeader*    pHeader = nullptr;
    RenderPassDesc         Desc{};

    static constexpr ChunkType ExpectedChunkType = ChunkType::RenderPass;

    explicit RPData(IMemoryAllocator& Allocator, Uint32 BlockSize = 1 << 10) :
        Allocator{Allocator, BlockSize}
    {}

    bool Deserialize(const char* Name, Serializer<SerializerMode::Read>& Ser);
};


template <typename ResType>
bool DearchiverBase::NamedResourceCache<ResType>::Get(const char* Name, ResType** ppResource)
{
    VERIFY_EXPR(Name != nullptr);
    VERIFY_EXPR(ppResource != nullptr && *ppResource == nullptr);
    *ppResource = nullptr;

    std::unique_lock<std::mutex> Lock{m_Mtx};

    auto it = m_Map.find(Name);
    if (it == m_Map.end())
        return false;

    auto Ptr = it->second.Lock();
    if (!Ptr)
        return false;

    *ppResource = Ptr.Detach();
    return true;
}

template <typename ResType>
void DearchiverBase::NamedResourceCache<ResType>::Set(const char* Name, ResType* pResource)
{
    VERIFY_EXPR(Name != nullptr && Name[0] != '\0');
    VERIFY_EXPR(pResource != nullptr);

    std::unique_lock<std::mutex> Lock{m_Mtx};
    m_Map.emplace(HashMapStringKey{Name, true}, pResource);
}

// Instantiation is required by UnpackResourceSignatureImpl
template class DearchiverBase::NamedResourceCache<IPipelineResourceSignature>;


// Instantiation is required by UnpackResourceSignatureImpl
template bool DeviceObjectArchiveBase::LoadResourceData<DearchiverBase::PRSData>(
    const NameToArchiveRegionMap& NameToRegion,
    const char*                   ResourceName,
    DearchiverBase::PRSData&      ResData) const;


bool DearchiverBase::PRSData::Deserialize(const char* Name, Serializer<SerializerMode::Read>& Ser)
{
    Desc.Name = Name;
    PRSSerializer<SerializerMode::Read>::SerializeDesc(Ser, Desc, &Allocator);
    return true;
}

bool DearchiverBase::RPData::Deserialize(const char* Name, Serializer<SerializerMode::Read>& Ser)
{
    Desc.Name = Name;
    RPSerializer<SerializerMode::Read>::SerializeDesc(Ser, Desc, &Allocator);
    return true;
}

template <typename CreateInfoType>
void DearchiverBase::PSOData<CreateInfoType>::DeserializeInternal(Serializer<SerializerMode::Read>& Ser)
{
    PSOSerializer<SerializerMode::Read>::SerializeCreateInfo(Ser, CreateInfo, PRSNames, &Allocator);
}

template <>
void DearchiverBase::PSOData<GraphicsPipelineStateCreateInfo>::DeserializeInternal(Serializer<SerializerMode::Read>& Ser)
{
    PSOSerializer<SerializerMode::Read>::SerializeCreateInfo(Ser, CreateInfo, PRSNames, &Allocator, RenderPassName);
}

template <>
void DearchiverBase::PSOData<RayTracingPipelineStateCreateInfo>::DeserializeInternal(Serializer<SerializerMode::Read>& Ser)
{
    auto RemapShaders = [](Uint32& InIndex, IShader*& outShader) {
        outShader = BitCast<IShader*>(size_t{InIndex});
    };
    PSOSerializer<SerializerMode::Read>::SerializeCreateInfo(Ser, CreateInfo, PRSNames, &Allocator, RemapShaders);
}

template <typename CreateInfoType>
bool DearchiverBase::PSOData<CreateInfoType>::Deserialize(const char* Name, Serializer<SerializerMode::Read>& Ser)
{
    CreateInfo.PSODesc.Name = Name;

    DeserializeInternal(Ser);
    PSOSerializer<SerializerMode::Read>::SerializeAuxData(Ser, AuxData, &Allocator);

    CreateInfo.Flags |= PSO_CREATE_FLAG_DONT_REMAP_SHADER_RESOURCES;
    if (AuxData.NoShaderReflection)
        InternalCI.Flags |= PSO_CREATE_INTERNAL_FLAG_NO_SHADER_REFLECTION;

    CreateInfo.pInternalData = &InternalCI;

    if (CreateInfo.ResourceSignaturesCount == 0)
    {
        CreateInfo.ResourceSignaturesCount = 1;
        InternalCI.Flags |= PSO_CREATE_INTERNAL_FLAG_IMPLICIT_SIGNATURE0;
    }

    return true;
}

template <>
const DearchiverBase::ChunkType DearchiverBase::PSOData<GraphicsPipelineStateCreateInfo>::ExpectedChunkType = DearchiverBase::ChunkType::GraphicsPipelineStates;
template <>
const DearchiverBase::ChunkType DearchiverBase::PSOData<ComputePipelineStateCreateInfo>::ExpectedChunkType = DearchiverBase::ChunkType::ComputePipelineStates;
template <>
const DearchiverBase::ChunkType DearchiverBase::PSOData<TilePipelineStateCreateInfo>::ExpectedChunkType = DearchiverBase::ChunkType::TilePipelineStates;
template <>
const DearchiverBase::ChunkType DearchiverBase::PSOData<RayTracingPipelineStateCreateInfo>::ExpectedChunkType = DearchiverBase::ChunkType::RayTracingPipelineStates;


template <>
bool DearchiverBase::UnpackPSORenderPass<GraphicsPipelineStateCreateInfo>(PSOData<GraphicsPipelineStateCreateInfo>& PSO, IRenderDevice* pRenderDevice)
{
    VERIFY_EXPR(pRenderDevice != nullptr);
    if (PSO.RenderPassName == nullptr || *PSO.RenderPassName == 0)
        return true;

    RefCntAutoPtr<IRenderPass> pRenderPass;
    UnpackRenderPass(RenderPassUnpackInfo{pRenderDevice, PSO.RenderPassName}, &pRenderPass);
    if (!pRenderPass)
        return false;

    PSO.CreateInfo.GraphicsPipeline.pRenderPass = pRenderPass;
    PSO.Objects.emplace_back(std::move(pRenderPass));
    return true;
}

template <typename CreateInfoType>
bool DearchiverBase::UnpackPSOSignatures(PSOData<CreateInfoType>& PSO, IRenderDevice* pRenderDevice)
{
    const auto ResourceSignaturesCount = PSO.CreateInfo.ResourceSignaturesCount;
    if (ResourceSignaturesCount == 0)
    {
        UNEXPECTED("PSO must have at least one resource signature (including PSOs that use implicit signature)");
        return true;
    }
    auto* const ppResourceSignatures = PSO.Allocator.template Allocate<IPipelineResourceSignature*>(ResourceSignaturesCount);

    PSO.CreateInfo.ppResourceSignatures = ppResourceSignatures;
    for (Uint32 i = 0; i < ResourceSignaturesCount; ++i)
    {
        ResourceSignatureUnpackInfo UnpackInfo{pRenderDevice, PSO.PRSNames[i]};
        UnpackInfo.SRBAllocationGranularity = PSO.CreateInfo.PSODesc.SRBAllocationGranularity;

        auto pSignature = UnpackResourceSignature(UnpackInfo, (PSO.InternalCI.Flags & PSO_CREATE_INTERNAL_FLAG_IMPLICIT_SIGNATURE0) != 0);
        if (!pSignature)
            return false;

        ppResourceSignatures[i] = pSignature;
        PSO.Objects.emplace_back(std::move(pSignature));
    }
    return true;
}

RefCntAutoPtr<IShader> DearchiverBase::UnpackShader(const ShaderCreateInfo& ShaderCI,
                                                    IRenderDevice*          pDevice)
{
    RefCntAutoPtr<IShader> pShader;
    pDevice->CreateShader(ShaderCI, &pShader);
    return pShader;
}

inline void AssignShader(IShader*& pDstShader, IShader* pSrcShader, SHADER_TYPE ExpectedType)
{
    VERIFY_EXPR(pSrcShader != nullptr);
    VERIFY_EXPR(pSrcShader->GetDesc().ShaderType == ExpectedType);

    VERIFY(pDstShader == nullptr, "Non-null ", GetShaderTypeLiteralName(pDstShader->GetDesc().ShaderType), " has already been assigned. This might be a bug.");
    pDstShader = pSrcShader;
}

template <>
void DearchiverBase::PSOData<GraphicsPipelineStateCreateInfo>::AssignShaders()
{
    for (auto& Shader : Shaders)
    {
        const auto ShaderType = Shader->GetDesc().ShaderType;
        switch (ShaderType)
        {
            // clang-format off
            case SHADER_TYPE_VERTEX:        AssignShader(CreateInfo.pVS, Shader, ShaderType); break;
            case SHADER_TYPE_PIXEL:         AssignShader(CreateInfo.pPS, Shader, ShaderType); break;
            case SHADER_TYPE_GEOMETRY:      AssignShader(CreateInfo.pGS, Shader, ShaderType); break;
            case SHADER_TYPE_HULL:          AssignShader(CreateInfo.pHS, Shader, ShaderType); break; 
            case SHADER_TYPE_DOMAIN:        AssignShader(CreateInfo.pDS, Shader, ShaderType); break;
            case SHADER_TYPE_AMPLIFICATION: AssignShader(CreateInfo.pAS, Shader, ShaderType); break;
            case SHADER_TYPE_MESH:          AssignShader(CreateInfo.pMS, Shader, ShaderType); break;
            // clang-format on
            default:
                LOG_ERROR_MESSAGE("Unsupported shader type for graphics pipeline");
                return;
        }
    }
}

template <>
void DearchiverBase::PSOData<ComputePipelineStateCreateInfo>::AssignShaders()
{
    VERIFY(Shaders.size() == 1, "Compute pipline must have one shader");
    AssignShader(CreateInfo.pCS, Shaders[0], SHADER_TYPE_COMPUTE);
}

template <>
void DearchiverBase::PSOData<TilePipelineStateCreateInfo>::AssignShaders()
{
    VERIFY(Shaders.size() == 1, "Tile pipline must have one shader");
    AssignShader(CreateInfo.pTS, Shaders[0], SHADER_TYPE_TILE);
}

template <>
void DearchiverBase::PSOData<RayTracingPipelineStateCreateInfo>::AssignShaders()
{
    auto RemapShader = [this](IShader* const& inoutShader) //
    {
        auto ShaderIndex = BitCast<size_t>(inoutShader);
        if (ShaderIndex < Shaders.size())
            const_cast<IShader*&>(inoutShader) = Shaders[ShaderIndex];
        else
        {
            VERIFY(ShaderIndex == ~0u, "Failed to remap shader");
            const_cast<IShader*&>(inoutShader) = nullptr;
        }
    };

    for (Uint32 i = 0; i < CreateInfo.GeneralShaderCount; ++i)
    {
        RemapShader(CreateInfo.pGeneralShaders[i].pShader);
    }
    for (Uint32 i = 0; i < CreateInfo.TriangleHitShaderCount; ++i)
    {
        RemapShader(CreateInfo.pTriangleHitShaders[i].pClosestHitShader);
        RemapShader(CreateInfo.pTriangleHitShaders[i].pAnyHitShader);
    }
    for (Uint32 i = 0; i < CreateInfo.ProceduralHitShaderCount; ++i)
    {
        RemapShader(CreateInfo.pProceduralHitShaders[i].pIntersectionShader);
        RemapShader(CreateInfo.pProceduralHitShaders[i].pClosestHitShader);
        RemapShader(CreateInfo.pProceduralHitShaders[i].pAnyHitShader);
    }
}

template <>
void DearchiverBase::PSOData<GraphicsPipelineStateCreateInfo>::CreatePipeline(IRenderDevice* pDevice, IPipelineState** ppPSO)
{
    pDevice->CreateGraphicsPipelineState(CreateInfo, ppPSO);
}

template <>
void DearchiverBase::PSOData<ComputePipelineStateCreateInfo>::CreatePipeline(IRenderDevice* pDevice, IPipelineState** ppPSO)
{
    pDevice->CreateComputePipelineState(CreateInfo, ppPSO);
}

template <>
void DearchiverBase::PSOData<TilePipelineStateCreateInfo>::CreatePipeline(IRenderDevice* pDevice, IPipelineState** ppPSO)
{
    pDevice->CreateTilePipelineState(CreateInfo, ppPSO);
}

template <>
void DearchiverBase::PSOData<RayTracingPipelineStateCreateInfo>::CreatePipeline(IRenderDevice* pDevice, IPipelineState** ppPSO)
{
    pDevice->CreateRayTracingPipelineState(CreateInfo, ppPSO);
}

template <typename CreateInfoType>
bool DearchiverBase::UnpackPSOShaders(PSOData<CreateInfoType>& PSO,
                                      IRenderDevice*           pDevice)
{
    const auto DevType    = GetArchiveDeviceType(pDevice);
    const auto ShaderData = m_pArchive->GetDeviceSpecificData(DevType, *PSO.pHeader, PSO.Allocator, PSO.ExpectedChunkType);
    if (!ShaderData)
        return false;

    const Uint64 BaseOffset = m_pArchive->m_ArchiveIndex.BaseOffsets[static_cast<size_t>(DeviceObjectArchiveBase::GetBlockOffsetType(DevType))];
    if (BaseOffset > m_pArchive->m_pArchive->GetSize())
    {
        LOG_ERROR_MESSAGE("Required block does not exist in archive");
        return false;
    }

    DynamicLinearAllocator Allocator{GetRawAllocator()};

    DeviceObjectArchiveBase::ShaderIndexArray ShaderIndices;
    {
        Serializer<SerializerMode::Read> Ser{ShaderData};
        PSOSerializer<SerializerMode::Read>::SerializeShaderIndices(Ser, ShaderIndices, &Allocator);
        VERIFY_EXPR(Ser.IsEnded());
    }

    auto& ShaderInfo = m_pArchive->GetShaderDeviceInfo(DevType, Allocator);

    PSO.Shaders.resize(ShaderIndices.Count);
    for (Uint32 i = 0; i < ShaderIndices.Count; ++i)
    {
        auto& pShader{PSO.Shaders[i]};

        const Uint32 Idx = ShaderIndices.pIndices[i];

        ArchiveRegion OffsetAndSize;
        {
            std::unique_lock<std::mutex> ReadLock{ShaderInfo.Mtx};

            if (Idx >= ShaderInfo.Regions.size())
                return false;

            if (ShaderInfo.Cache.empty())
                ShaderInfo.Cache.resize(ShaderInfo.Regions.size());
            else
                VERIFY_EXPR(ShaderInfo.Regions.size() == ShaderInfo.Cache.size());
            // Try to get cached shader
            pShader = ShaderInfo.Cache[Idx];
            if (pShader)
                continue;

            OffsetAndSize = ShaderInfo.Regions[Idx];
        }

        void* pData = Allocator.Allocate(OffsetAndSize.Size, DeviceObjectArchiveBase::DataPtrAlign);

        if (!m_pArchive->m_pArchive->Read(BaseOffset + OffsetAndSize.Offset, OffsetAndSize.Size, pData))
            return false;

        {
            ShaderCreateInfo ShaderCI;
            {
                Serializer<SerializerMode::Read> ShaderSer{SerializedData{pData, OffsetAndSize.Size}};
                ShaderSerializer<SerializerMode::Read>::SerializeCI(ShaderSer, ShaderCI);
                VERIFY_EXPR(ShaderSer.IsEnded());
            }

            if ((PSO.InternalCI.Flags & PSO_CREATE_INTERNAL_FLAG_NO_SHADER_REFLECTION) != 0)
                ShaderCI.CompileFlags |= SHADER_COMPILE_FLAG_SKIP_REFLECTION;

            pShader = UnpackShader(ShaderCI, pDevice);
            if (!pShader)
                return false;
        }

        // Add to the cache
        {
            std::unique_lock<std::mutex> WriteLock{ShaderInfo.Mtx};
            ShaderInfo.Cache[Idx] = pShader;
        }
    }

    return true;
}

template <typename PSOCreateInfoType>
static bool ModifyPipelineStateCreateInfo(PSOCreateInfoType&             CreateInfo,
                                          const PipelineStateUnpackInfo& UnpackInfo)
{
    if (UnpackInfo.ModifyPipelineStateCreateInfo == nullptr)
        return true;

    const auto PipelineType = CreateInfo.PSODesc.PipelineType;

    auto ResourceLayout = CreateInfo.PSODesc.ResourceLayout;

    std::unordered_set<std::string> Strings;

    std::vector<ShaderResourceVariableDesc> Variables{ResourceLayout.Variables, ResourceLayout.Variables + ResourceLayout.NumVariables};
    for (auto& Var : Variables)
        Var.Name = Strings.emplace(Var.Name).first->c_str();

    std::vector<ImmutableSamplerDesc> ImmutableSamplers{ResourceLayout.ImmutableSamplers, ResourceLayout.ImmutableSamplers + ResourceLayout.NumImmutableSamplers};
    for (auto& Sam : ImmutableSamplers)
        Sam.SamplerOrTextureName = Strings.emplace(Sam.SamplerOrTextureName).first->c_str();

    ResourceLayout.Variables         = Variables.data();
    ResourceLayout.ImmutableSamplers = ImmutableSamplers.data();

    std::vector<IPipelineResourceSignature*> pSignatures{CreateInfo.ppResourceSignatures, CreateInfo.ppResourceSignatures + CreateInfo.ResourceSignaturesCount};

    UnpackInfo.ModifyPipelineStateCreateInfo(CreateInfo, UnpackInfo.pUserData);

    if (PipelineType != CreateInfo.PSODesc.PipelineType)
    {
        LOG_ERROR_MESSAGE("Modifying pipeline type is not allowed");
        return false;
    }

    if (!PipelineResourceLayoutDesc::IsEqual(ResourceLayout, CreateInfo.PSODesc.ResourceLayout, /*IgnoreVariables = */ false, /*IgnoreSamplers = */ true))
    {
        LOG_ERROR_MESSAGE("Only immutable sampler descriptions in the pipeline resource layout can be modified");
        return false;
    }

    for (size_t i = 0; i < ResourceLayout.NumImmutableSamplers; ++i)
    {
        // Immutable sampler descriptions can be modified, but shader stages must be the same
        if (ResourceLayout.ImmutableSamplers[i].ShaderStages != CreateInfo.PSODesc.ResourceLayout.ImmutableSamplers[i].ShaderStages)
        {
            LOG_ERROR_MESSAGE("Modifiying immutable sampler shader stages in the resource layout is not allowed");
            return false;
        }
    }

    if (pSignatures.size() != CreateInfo.ResourceSignaturesCount)
    {
        LOG_ERROR_MESSAGE("Changing the number of resource signatures is not allowed");
        return false;
    }

    for (size_t sign = 0; sign < CreateInfo.ResourceSignaturesCount; ++sign)
    {
        const auto* pOrigSign = pSignatures[sign];
        const auto* pNewSign  = CreateInfo.ppResourceSignatures[sign];
        if (pOrigSign == pNewSign)
            continue;
        if ((pOrigSign == nullptr) != (pNewSign == nullptr))
        {
            LOG_ERROR_MESSAGE("Changing non-null resource signature to null and vice versa is not allowed");
            return false;
        }
        if ((pOrigSign == nullptr) || (pNewSign == nullptr))
        {
            // This may never happen, but let's make static analyzers happy
            continue;
        }

        const auto& OrigDesc = pOrigSign->GetDesc();
        const auto& NewDesc  = pNewSign->GetDesc();
        if (!PipelineResourceSignaturesCompatible(OrigDesc, NewDesc, /*IgnoreSamplerDescriptions =*/true))
        {
            LOG_ERROR_MESSAGE("When changing pipeline resource singatures, only immutable sampler descriptions in new signatures are allowed to differ from original");
            return false;
        }
    }

    return true;
}

template <typename CreateInfoType>
void DearchiverBase::UnpackPipelineStateImpl(const PipelineStateUnpackInfo&      UnpackInfo,
                                             IPipelineState**                    ppPSO,
                                             const NameToArchiveRegionMap&       PSONameToRegion,
                                             NamedResourceCache<IPipelineState>& PSOCache)
{
    VERIFY_EXPR(UnpackInfo.pDevice != nullptr);

    if (UnpackInfo.ModifyPipelineStateCreateInfo == nullptr && PSOCache.Get(UnpackInfo.Name, ppPSO))
        return;

    PSOData<CreateInfoType> PSO{GetRawAllocator()};
    if (!m_pArchive->LoadResourceData(PSONameToRegion, UnpackInfo.Name, PSO))
        return;

#ifdef DILIGENT_DEVELOPMENT
    if (UnpackInfo.pDevice->GetDeviceInfo().IsD3DDevice())
    {
        // We always have reflection information in Direct3D shaders, so always
        // load it in development build to allow the engine verify bindings.
        PSO.InternalCI.Flags &= ~PSO_CREATE_INTERNAL_FLAG_NO_SHADER_REFLECTION;
    }
#endif

    if (!UnpackPSORenderPass(PSO, UnpackInfo.pDevice))
        return;

    if (!UnpackPSOSignatures(PSO, UnpackInfo.pDevice))
        return;

    if (!UnpackPSOShaders(PSO, UnpackInfo.pDevice))
        return;

    PSO.AssignShaders();

    PSO.CreateInfo.PSODesc.SRBAllocationGranularity = UnpackInfo.SRBAllocationGranularity;
    PSO.CreateInfo.PSODesc.ImmediateContextMask     = UnpackInfo.ImmediateContextMask;
    PSO.CreateInfo.pPSOCache                        = UnpackInfo.pCache;

    if (!ModifyPipelineStateCreateInfo(PSO.CreateInfo, UnpackInfo))
        return;

    PSO.CreatePipeline(UnpackInfo.pDevice, ppPSO);

    if (UnpackInfo.ModifyPipelineStateCreateInfo == nullptr)
        PSOCache.Set(UnpackInfo.Name, *ppPSO);
}

bool DearchiverBase::LoadArchive(IArchive* pArchive)
{
    if (pArchive == nullptr)
        return false;

    try
    {
        m_pArchive = NEW_RC_OBJ(GetRawAllocator(), "Device object archive instance", DeviceObjectArchiveBase)(pArchive);
        return true;
    }
    catch (...)
    {
        LOG_ERROR("Failed to create the device object archive");
        return false;
    }
}

void DearchiverBase::UnpackPipelineState(const PipelineStateUnpackInfo& UnpackInfo, IPipelineState** ppPSO)
{
    if (!VerifyPipelineStateUnpackInfo(UnpackInfo, ppPSO))
        return;

    *ppPSO = nullptr;

    switch (UnpackInfo.PipelineType)
    {
        case PIPELINE_TYPE_GRAPHICS:
        case PIPELINE_TYPE_MESH:
            UnpackPipelineStateImpl<GraphicsPipelineStateCreateInfo>(UnpackInfo, ppPSO, m_pArchive->m_ArchiveIndex.GraphPSO, m_Cache.GraphPSO);
            break;

        case PIPELINE_TYPE_COMPUTE:
            UnpackPipelineStateImpl<ComputePipelineStateCreateInfo>(UnpackInfo, ppPSO, m_pArchive->m_ArchiveIndex.CompPSO, m_Cache.CompPSO);
            break;

        case PIPELINE_TYPE_RAY_TRACING:
            UnpackPipelineStateImpl<TilePipelineStateCreateInfo>(UnpackInfo, ppPSO, m_pArchive->m_ArchiveIndex.TilePSO, m_Cache.TilePSO);
            break;

        case PIPELINE_TYPE_TILE:
            UnpackPipelineStateImpl<RayTracingPipelineStateCreateInfo>(UnpackInfo, ppPSO, m_pArchive->m_ArchiveIndex.RayTrPSO, m_Cache.RayTrPSO);
            break;

        case PIPELINE_TYPE_INVALID:
        default:
            LOG_ERROR_MESSAGE("Unsupported pipeline type");
            return;
    }
}

void DearchiverBase::UnpackResourceSignature(const ResourceSignatureUnpackInfo& DeArchiveInfo,
                                             IPipelineResourceSignature**       ppSignature)
{
    if (!VerifyResourceSignatureUnpackInfo(DeArchiveInfo, ppSignature))
        return;

    *ppSignature = nullptr;

    auto pSignature = UnpackResourceSignature(DeArchiveInfo, false /*IsImplicit*/);
    *ppSignature    = pSignature.Detach();
}

void DearchiverBase::UnpackRenderPass(const RenderPassUnpackInfo& UnpackInfo, IRenderPass** ppRP)
{
    if (!VerifyRenderPassUnpackInfo(UnpackInfo, ppRP))
        return;

    *ppRP = nullptr;

    VERIFY_EXPR(UnpackInfo.pDevice != nullptr);

    if (UnpackInfo.ModifyRenderPassDesc == nullptr && m_Cache.RenderPass.Get(UnpackInfo.Name, ppRP))
        return;

    RPData RP{GetRawAllocator()};
    if (!m_pArchive->LoadResourceData(m_pArchive->m_ArchiveIndex.RenderPass, UnpackInfo.Name, RP))
        return;

    if (UnpackInfo.ModifyRenderPassDesc != nullptr)
        UnpackInfo.ModifyRenderPassDesc(RP.Desc, UnpackInfo.pUserData);

    UnpackInfo.pDevice->CreateRenderPass(RP.Desc, ppRP);

    if (UnpackInfo.ModifyRenderPassDesc == nullptr)
        m_Cache.RenderPass.Set(UnpackInfo.Name, *ppRP);
}

/// Implementation of IDearchiver::Reset().
void DearchiverBase::Reset()
{
    m_pArchive.Release();
    m_Cache.Sign.Clear();
    m_Cache.RenderPass.Clear();
    m_Cache.GraphPSO.Clear();
    m_Cache.CompPSO.Clear();
    m_Cache.TilePSO.Clear();
    m_Cache.RayTrPSO.Clear();
}

} // namespace Diligent
