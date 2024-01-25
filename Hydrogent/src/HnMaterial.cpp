/*
 *  Copyright 2023-2024 Diligent Graphics LLC
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

#include "HnMaterial.hpp"

#include <vector>

#include "HnRenderDelegate.hpp"
#include "HnTokens.hpp"
#include "HnTypeConversions.hpp"
#include "HnRenderPass.hpp"
#include "GfTypeConversions.hpp"
#include "DynamicTextureAtlas.h"
#include "GLTFResourceManager.hpp"
#include "GLTFBuilder.hpp"
#include "DataBlobImpl.hpp"

#include "pxr/imaging/hd/sceneDelegate.h"

#include "USD_Renderer.hpp"
#include "DebugUtilities.hpp"
#include "Image.h"

namespace Diligent
{

namespace USD
{

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    HnMaterialPrivateTokens,
    (whiteRgba8)
    (blackRgba8)
    (whiteR8)
);
// clang-format on


HnMaterial* HnMaterial::Create(const pxr::SdfPath& id)
{
    return new HnMaterial{id};
}

HnMaterial* HnMaterial::CreateFallback(HnTextureRegistry& TexRegistry, const USD_Renderer& UsdRenderer)
{
    return new HnMaterial{TexRegistry, UsdRenderer};
}

HnMaterial::HnMaterial(const pxr::SdfPath& id) :
    pxr::HdMaterial{id}
{
    m_MaterialData.Attribs.BaseColorFactor = float4{1, 1, 1, 1};
    m_MaterialData.Attribs.SpecularFactor  = float4{1, 1, 1, 1};
    m_MaterialData.Attribs.MetallicFactor  = 1;
    m_MaterialData.Attribs.RoughnessFactor = 1;
    m_MaterialData.Attribs.OcclusionFactor = 1;

    m_MaterialData.Attribs.Workflow = PBR_Renderer::PBR_WORKFLOW_METALL_ROUGH;
}


// Default material
HnMaterial::HnMaterial(HnTextureRegistry& TexRegistry, const USD_Renderer& UsdRenderer) :
    HnMaterial{pxr::SdfPath{}}
{
    // Sync() is never called for the default material, so we need to initialize texture attributes now.
    InitTextureAttribs(TexRegistry, UsdRenderer, {});
}

HnMaterial::~HnMaterial()
{
}

void HnMaterial::Sync(pxr::HdSceneDelegate* SceneDelegate,
                      pxr::HdRenderParam*   RenderParam,
                      pxr::HdDirtyBits*     DirtyBits)
{
    if (*DirtyBits == pxr::HdMaterial::Clean)
        return;

    HnRenderDelegate*   RenderDelegate = static_cast<HnRenderDelegate*>(SceneDelegate->GetRenderIndex().GetRenderDelegate());
    HnTextureRegistry&  TexRegistry    = RenderDelegate->GetTextureRegistry();
    const USD_Renderer& UsdRenderer    = *RenderDelegate->GetUSDRenderer();

    // A mapping from the texture name to the texture coordinate set index (e.g. "diffuseColor" -> 0)
    TexNameToCoordSetMapType TexNameToCoordSetMap;

    pxr::VtValue vtMat = SceneDelegate->GetMaterialResource(GetId());
    if (vtMat.IsHolding<pxr::HdMaterialNetworkMap>())
    {
        const pxr::HdMaterialNetworkMap& hdNetworkMap = vtMat.UncheckedGet<pxr::HdMaterialNetworkMap>();
        if (!hdNetworkMap.terminals.empty() && !hdNetworkMap.map.empty())
        {
            try
            {
                m_Network = HnMaterialNetwork{GetId(), hdNetworkMap}; // May throw

                TexNameToCoordSetMap = AllocateTextures(TexRegistry);
                ProcessMaterialNetwork();
            }
            catch (const std::runtime_error& err)
            {
                LOG_ERROR_MESSAGE("Failed to create material network for material ", GetId(), ": ", err.what());
                m_Network = {};
            }
            catch (...)
            {
                LOG_ERROR_MESSAGE("Failed to create material network for material ", GetId(), ": unknown error");
                m_Network = {};
            }
        }
    }

    // It is important to initialize texture attributes with default values even if there is no material network.
    InitTextureAttribs(TexRegistry, UsdRenderer, TexNameToCoordSetMap);

    *DirtyBits = HdMaterial::Clean;
}

static bool ReadFallbackValue(const HnMaterialNetwork& Network, const pxr::TfToken& Name, float4& Value)
{
    if (const HnMaterialParameter* Param = Network.GetParameter(HnMaterialParameter::ParamType::Fallback, Name))
    {
        Value = float4{ToFloat3(Param->FallbackValue.Get<pxr::GfVec3f>()), 1};
        return true;
    }

    return false;
}

static bool ReadFallbackValue(const HnMaterialNetwork& Network, const pxr::TfToken& Name, float& Value)
{
    if (const HnMaterialParameter* Param = Network.GetParameter(HnMaterialParameter::ParamType::Fallback, Name))
    {
        Value = Param->FallbackValue.Get<float>();
        return true;
    }

    return false;
}

template <typename T>
static void ApplyTextureInputScale(const HnMaterialNetwork& Network, const pxr::TfToken& Name, T& Value)
{
    if (const HnMaterialParameter* TexParam = Network.GetParameter(HnMaterialParameter::ParamType::Texture, Name))
    {
        for (size_t i = 0; i < Value.GetComponentCount(); ++i)
            Value[i] *= TexParam->InputScale[i];
    }
}

static void ApplyTextureInputScale(const HnMaterialNetwork& Network, const pxr::TfToken& Name, float& Value)
{
    if (const HnMaterialParameter* TexParam = Network.GetParameter(HnMaterialParameter::ParamType::Texture, Name))
    {
        Value *= TexParam->InputScale[0];
    }
}

void HnMaterial::ProcessMaterialNetwork()
{
    ReadFallbackValue(m_Network, HnTokens->diffuseColor, m_MaterialData.Attribs.BaseColorFactor);
    ReadFallbackValue(m_Network, HnTokens->metallic, m_MaterialData.Attribs.MetallicFactor);
    ReadFallbackValue(m_Network, HnTokens->roughness, m_MaterialData.Attribs.RoughnessFactor);
    ReadFallbackValue(m_Network, HnTokens->occlusion, m_MaterialData.Attribs.OcclusionFactor);
    if (!ReadFallbackValue(m_Network, HnTokens->emissiveColor, m_MaterialData.Attribs.EmissiveFactor))
    {
        m_MaterialData.Attribs.EmissiveFactor = m_Textures.find(HnTokens->emissiveColor) != m_Textures.end() ? float4{1} : float4{0};
    }

    ApplyTextureInputScale(m_Network, HnTokens->diffuseColor, m_MaterialData.Attribs.BaseColorFactor);
    ApplyTextureInputScale(m_Network, HnTokens->metallic, m_MaterialData.Attribs.MetallicFactor);
    ApplyTextureInputScale(m_Network, HnTokens->roughness, m_MaterialData.Attribs.RoughnessFactor);
    ApplyTextureInputScale(m_Network, HnTokens->occlusion, m_MaterialData.Attribs.OcclusionFactor);
    ApplyTextureInputScale(m_Network, HnTokens->emissiveColor, m_MaterialData.Attribs.EmissiveFactor);

    if (const HnMaterialParameter* Param = m_Network.GetParameter(HnMaterialParameter::ParamType::Fallback, HnTokens->clearcoat))
    {
        m_MaterialData.Attribs.ClearcoatFactor = Param->FallbackValue.Get<float>();
        if (m_MaterialData.Attribs.ClearcoatFactor > 0)
        {
            m_MaterialData.HasClearcoat = true;

            if (const HnMaterialParameter* RoughnessParam = m_Network.GetParameter(HnMaterialParameter::ParamType::Fallback, HnTokens->clearcoatRoughness))
            {
                m_MaterialData.Attribs.ClearcoatRoughnessFactor = RoughnessParam->FallbackValue.Get<float>();
            }
        }
    }


    m_MaterialData.Attribs.AlphaMode = MaterialTagToPbrAlphaMode(m_Network.GetTag());

    m_MaterialData.Attribs.AlphaCutoff       = m_Network.GetOpacityThreshold();
    m_MaterialData.Attribs.BaseColorFactor.a = m_Network.GetOpacity();
}

void HnMaterial::InitTextureAttribs(HnTextureRegistry& TexRegistry, const USD_Renderer& UsdRenderer, const TexNameToCoordSetMapType& TexNameToCoordSetMap)
{
    GLTF::MaterialBuilder MatBuilder{m_MaterialData};

    auto SetTextureParams = [&](const pxr::TfToken& Name, Uint32 Idx) {
        GLTF::Material::TextureShaderAttribs& TexAttribs = MatBuilder.GetTextureAttrib(Idx);

        auto coord_it         = TexNameToCoordSetMap.find(Name);
        TexAttribs.UVSelector = coord_it != TexNameToCoordSetMap.end() ?
            static_cast<float>(coord_it->second) :
            0;

        TexAttribs.UBias              = 0;
        TexAttribs.VBias              = 0;
        TexAttribs.UVScaleAndRotation = float2x2::Identity();

        auto tex_it = m_Textures.find(Name);
        if (tex_it != m_Textures.end())
        {
            if (const HnMaterialParameter* Param = m_Network.GetParameter(HnMaterialParameter::ParamType::Transform2d, Name))
            {
                float2x2 UVScaleAndRotation = float2x2::Scale(Param->Transform2d.Scale[0], Param->Transform2d.Scale[1]);
                float    Rotation           = Param->Transform2d.Rotation;
                if (Rotation != 0)
                {
                    UVScaleAndRotation *= float2x2::Rotation(DegToRad(Rotation));
                }

                TexAttribs.UBias = Param->Transform2d.Translation[0];
                TexAttribs.VBias = Param->Transform2d.Translation[1];

                TexAttribs.UVScaleAndRotation = UVScaleAndRotation;
            }
        }
        else
        {
            tex_it = m_Textures.emplace(Name, GetDefaultTexture(TexRegistry, Name)).first;
        }

        if (ITextureAtlasSuballocation* pAtlasSuballocation = tex_it->second->pAtlasSuballocation)
        {
            TexAttribs.TextureSlice        = static_cast<float>(pAtlasSuballocation->GetSlice());
            TexAttribs.AtlasUVScaleAndBias = pAtlasSuballocation->GetUVScaleBias();

            m_UsesAtlas = true;
        }
        else
        {
            TexAttribs.TextureSlice        = 0;
            TexAttribs.AtlasUVScaleAndBias = float4{1, 1, 0, 0};
        }
    };

    const auto& TexAttribIndices = UsdRenderer.GetSettings().TextureAttribIndices;
    // clang-format off
    SetTextureParams(HnTokens->diffuseColor,  TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_BASE_COLOR]);
    SetTextureParams(HnTokens->normal,        TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_NORMAL]);
    SetTextureParams(HnTokens->metallic,      TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_METALLIC]);
    SetTextureParams(HnTokens->roughness,     TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_ROUGHNESS]);
    SetTextureParams(HnTokens->occlusion,     TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_OCCLUSION]);
    SetTextureParams(HnTokens->emissiveColor, TexAttribIndices[PBR_Renderer::TEXTURE_ATTRIB_ID_EMISSIVE]);
    // clang-format on

    MatBuilder.Finalize();
}

static RefCntAutoPtr<Image> CreateDefaultImage(const pxr::TfToken& Name, Uint32 Dimension = 64)
{
    ImageDesc ImgDesc;
    ImgDesc.Width         = Dimension;
    ImgDesc.Height        = Dimension;
    ImgDesc.ComponentType = VT_UINT8;
    RefCntAutoPtr<IDataBlob> pData;

    auto InitData = [&](Uint32 NumComponents, int Value) {
        ImgDesc.NumComponents = NumComponents;
        ImgDesc.RowStride     = ImgDesc.Width * ImgDesc.NumComponents;
        pData                 = DataBlobImpl::Create(size_t{ImgDesc.RowStride} * size_t{ImgDesc.Height});
        if (Value >= 0)
        {
            memset(pData->GetDataPtr(), Value, pData->GetSize());
        }
    };

    if (Name == HnMaterialPrivateTokens->whiteRgba8)
    {
        InitData(4, 255);
    }
    else if (Name == HnMaterialPrivateTokens->blackRgba8)
    {
        InitData(4, 0);
    }
    else if (Name == HnMaterialPrivateTokens->whiteR8)
    {
        InitData(1, 255);
    }
    else if (Name == HnTokens->normal)
    {
        InitData(4, -1);

        Uint8* pDst = reinterpret_cast<Uint8*>(pData->GetDataPtr());
        for (size_t i = 0; i < pData->GetSize(); i += 4)
        {
            pDst[i + 0] = 128;
            pDst[i + 1] = 128;
            pDst[i + 2] = 255;
            pDst[i + 3] = 0;
        }
    }
    else
    {
        UNEXPECTED("Unknown texture name '", Name, "'");
        InitData(4, 0);
    }

    RefCntAutoPtr<Image> pImage;
    Image::CreateFromMemory(ImgDesc, pData, &pImage);
    VERIFY_EXPR(pImage);
    return pImage;
}

static pxr::TfToken GetDefaultTexturePath(const pxr::TfToken& Name)
{
    return pxr::TfToken{std::string{"$Default-"} + Name.GetString()};
}

HnTextureRegistry::TextureHandleSharedPtr HnMaterial::GetDefaultTexture(HnTextureRegistry& TexRegistry, const pxr::TfToken& Name)
{
    pxr::TfToken DefaultTexName;
    if (Name == HnTokens->diffuseColor ||
        Name == HnTokens->emissiveColor)
    {
        DefaultTexName = HnMaterialPrivateTokens->whiteRgba8;
    }
    else if (Name == HnTokens->normal)
    {
        DefaultTexName = HnTokens->normal;
    }
    else if (Name == HnTokens->metallic ||
             Name == HnTokens->roughness ||
             Name == HnTokens->occlusion)
    {
        DefaultTexName = HnMaterialPrivateTokens->whiteR8;
    }
    else
    {
        UNEXPECTED("Unknown texture name '", Name, "'");
        DefaultTexName = HnMaterialPrivateTokens->blackRgba8;
    }

    const pxr::TfToken DefaultTexPath = GetDefaultTexturePath(DefaultTexName);

    pxr::HdSamplerParameters SamplerParams;
    SamplerParams.wrapS     = pxr::HdWrapRepeat;
    SamplerParams.wrapT     = pxr::HdWrapRepeat;
    SamplerParams.wrapR     = pxr::HdWrapRepeat;
    SamplerParams.minFilter = pxr::HdMinFilterLinearMipmapLinear;
    SamplerParams.magFilter = pxr::HdMagFilterLinear;
    return TexRegistry.Allocate(DefaultTexPath, TextureComponentMapping::Identity(), SamplerParams,
                                [&]() {
                                    RefCntAutoPtr<Image> pImage = CreateDefaultImage(DefaultTexName);

                                    TextureLoadInfo               LoadInfo{Name.GetText()};
                                    RefCntAutoPtr<ITextureLoader> pLoader;
                                    CreateTextureLoaderFromImage(pImage, LoadInfo, &pLoader);
                                    VERIFY_EXPR(pLoader);
                                    return pLoader;
                                });
}

static TEXTURE_FORMAT GetMaterialTextureFormat(const pxr::TfToken& Name)
{
    if (Name == HnTokens->diffuseColor ||
        Name == HnTokens->emissiveColor ||
        Name == HnTokens->normal)
    {
        return TEX_FORMAT_RGBA8_UNORM;
    }
    else if (Name == HnTokens->metallic ||
             Name == HnTokens->roughness ||
             Name == HnTokens->occlusion)
    {
        return TEX_FORMAT_R8_UNORM;
    }
    else
    {
        return TEX_FORMAT_UNKNOWN;
    }
}

HnMaterial::TexNameToCoordSetMapType HnMaterial::AllocateTextures(HnTextureRegistry& TexRegistry)
{
    // Texture name to texture coordinate set index (e.g. "diffuseColor" -> 0)
    TexNameToCoordSetMapType TexNameToCoordSetMap;

    // Texture coordinate primvar name to texture coordinate set index (e.g. "st" -> 0)
    std::unordered_map<pxr::TfToken, size_t, pxr::TfToken::HashFunctor> TexCoordPrimvarMapping;
    for (const HnMaterialNetwork::TextureDescriptor& TexDescriptor : m_Network.GetTextures())
    {
        TEXTURE_FORMAT Format = GetMaterialTextureFormat(TexDescriptor.Name);
        if (Format == TEX_FORMAT_UNKNOWN)
        {
            LOG_INFO_MESSAGE("Skipping unknown texture '", TexDescriptor.Name, "' in material '", GetId(), "'");
            continue;
        }

        if (TexDescriptor.TextureId.FilePath.IsEmpty())
        {
            LOG_ERROR_MESSAGE("Texture '", TexDescriptor.Name, "' in material '", GetId(), "' has no file path");
            continue;
        }

        if (auto pTex = TexRegistry.Allocate(TexDescriptor.TextureId, Format, TexDescriptor.SamplerParams))
        {
            m_Textures[TexDescriptor.Name] = pTex;
            // Find texture coordinate
            size_t TexCoordIdx = ~size_t{0};
            if (const HnMaterialParameter* Param = m_Network.GetParameter(HnMaterialParameter::ParamType::Texture, TexDescriptor.Name))
            {
                if (!Param->SamplerCoords.empty())
                {
                    if (Param->SamplerCoords.size() > 1)
                        LOG_WARNING_MESSAGE("Texture '", TexDescriptor.Name, "' has ", Param->SamplerCoords.size(), " texture coordinates. Only the first set will be used");
                    const pxr::TfToken& TexCoordName = Param->SamplerCoords[0];

                    // Check if the texture coordinate set primvar (e.g. "st0") has already been allocated
                    auto it_inserted = TexCoordPrimvarMapping.emplace(TexCoordName, m_TexCoords.size());
                    TexCoordIdx      = it_inserted.first->second;
                    if (it_inserted.second)
                    {
                        // Add new texture coordinate set
                        VERIFY_EXPR(TexCoordIdx == m_TexCoords.size());
                        m_TexCoords.resize(TexCoordIdx + 1);
                        m_TexCoords[TexCoordIdx] = {TexCoordName};
                    }

                    TexNameToCoordSetMap[TexDescriptor.Name] = TexCoordIdx;
                }
                else
                {
                    LOG_ERROR_MESSAGE("Texture '", TexDescriptor.Name, "' in material '", GetId(), "' has no texture coordinates");
                }
            }

            if (TexCoordIdx == ~size_t{0})
            {
                LOG_ERROR_MESSAGE("Failed to find texture coordinates for texture '", TexDescriptor.Name, "' in material '", GetId(), "'");
            }
        }
    }

    return TexNameToCoordSetMap;
}


pxr::HdDirtyBits HnMaterial::GetInitialDirtyBitsMask() const
{
    return pxr::HdMaterial::AllDirty;
}

// {AFEC3E3E-021D-4BA6-9464-CB7E356DE15D}
static const INTERFACE_ID IID_HnMaterialSRBCache =
    {0xafec3e3e, 0x21d, 0x4ba6, {0x94, 0x64, 0xcb, 0x7e, 0x35, 0x6d, 0xe1, 0x5d}};

class HnMaterialSRBCache : public ObjectBase<IObject>
{
public:
    using StaticShaderTextureIdsArrayType = PBR_Renderer::StaticShaderTextureIdsArrayType;
    using ShaderTextureIndexingIdType     = HnMaterial::ShaderTextureIndexingIdType;

    HnMaterialSRBCache(IReferenceCounters* pRefCounters) :
        ObjectBase<IObject>{pRefCounters}
    {}

    static RefCntAutoPtr<IObject> Create()
    {
        return RefCntAutoPtr<IObject>{MakeNewRCObj<HnMaterialSRBCache>()()};
    }

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_HnMaterialSRBCache, ObjectBase<IObject>)

    /// SRB cache key
    ///
    /// The key is the combination of unique IDs of the texture objects used by the SRB.
    struct ResourceKey
    {
        std::vector<Int32> UniqueIDs;

        bool operator==(const ResourceKey& rhs) const
        {
            return UniqueIDs == rhs.UniqueIDs;
        }

        struct Hasher
        {
            size_t operator()(const ResourceKey& Key) const
            {
                return ComputeHashRaw(Key.UniqueIDs.data(), Key.UniqueIDs.size() * sizeof(Key.UniqueIDs[0]));
            }
        };
    };

    template <class CreateSRBFuncType>
    RefCntAutoPtr<IShaderResourceBinding> GetSRB(const ResourceKey& Key, CreateSRBFuncType&& CreateSRB)
    {
        return m_Cache.Get(Key, CreateSRB);
    }

    /// Adds shader texture indexing to the cache and returns its identifier, for example:
    ///     {0, 0, 0, 1, 1, 2} -> 0
    ///     {0, 1, 0, 1, 2, 2} -> 1
    ShaderTextureIndexingIdType AddShaderTextureIndexing(const StaticShaderTextureIdsArrayType& TextureIds)
    {
        std::lock_guard<std::mutex> Lock{m_ShaderTextureIndexingCacheMtx};

        auto it = m_ShaderTextureIndexingCache.find(TextureIds);
        if (it != m_ShaderTextureIndexingCache.end())
            return it->second;

        auto Id = static_cast<ShaderTextureIndexingIdType>(m_ShaderTextureIndexingCache.size());
        it      = m_ShaderTextureIndexingCache.emplace(TextureIds, Id).first;

        m_IdToIndexing.emplace(Id, it->first);

        return Id;
    }

    /// Returns the shader texture indexing by its identifier, for example:
    ///     0 -> {0, 0, 0, 1, 1, 2}
    ///     1 -> {0, 1, 0, 1, 2, 2}
    const StaticShaderTextureIdsArrayType& GetShaderTextureIndexing(Uint32 Id) const
    {
        auto it = m_IdToIndexing.find(Id);
        VERIFY_EXPR(it != m_IdToIndexing.end());
        return it->second;
    }

private:
    ObjectsRegistry<ResourceKey, RefCntAutoPtr<IShaderResourceBinding>, ResourceKey::Hasher> m_Cache;

    struct ShaderTextureIndexingTypeHasher
    {
        size_t operator()(const PBR_Renderer::StaticShaderTextureIdsArrayType& TexIds) const
        {
            size_t Hash = 0;
            for (const auto& Idx : TexIds)
            {
                HashCombine(Hash, Idx);
            }
            return Hash;
        }
    };

    std::mutex m_ShaderTextureIndexingCacheMtx;
    std::unordered_map<StaticShaderTextureIdsArrayType,
                       ShaderTextureIndexingIdType,
                       ShaderTextureIndexingTypeHasher>
        m_ShaderTextureIndexingCache;

    std::unordered_map<ShaderTextureIndexingIdType, const StaticShaderTextureIdsArrayType&> m_IdToIndexing;
};

const PBR_Renderer::StaticShaderTextureIdsArrayType& HnMaterial::GetStaticShaderTextureIds(IObject* SRBCache, ShaderTextureIndexingIdType Id)
{
    return ClassPtrCast<HnMaterialSRBCache>(SRBCache)->GetShaderTextureIndexing(Id);
}

RefCntAutoPtr<IObject> HnMaterial::CreateSRBCache()
{
    return HnMaterialSRBCache::Create();
}

void HnMaterial::UpdateSRB(HnRenderDelegate& RendererDelegate)
{
    RefCntAutoPtr<HnMaterialSRBCache> SRBCache{RendererDelegate.GetMaterialSRBCache(), IID_HnMaterialSRBCache};
    VERIFY_EXPR(SRBCache);

    const Uint32 AtlasVersion = RendererDelegate.GetTextureRegistry().GetAtlasVersion();
    if (m_UsesAtlas && AtlasVersion != m_AtlasVersion)
    {
        m_SRB.Release();
        m_PrimitiveAttribsVar = nullptr;
    }

    if (m_SRB)
        return;

    USD_Renderer& UsdRenderer       = *RendererDelegate.GetUSDRenderer();
    const Uint32  TexturesArraySize = UsdRenderer.GetSettings().MaterialTexturesArraySize;

    // Texture atlas format to atlas id, for example:
    //     RGBA8_UNORM      -> 0
    //     R8_UNORM         -> 1
    //     RGBA8_UNORM_SRGB -> 2
    std::unordered_map<TEXTURE_FORMAT, Uint32> AtlasFormatIds;
    if (m_UsesAtlas)
    {
        for (TEXTURE_FORMAT AtlasFmt : RendererDelegate.GetResourceManager().GetAllocatedAtlasFormats())
        {
            AtlasFormatIds.emplace(AtlasFmt, static_cast<Uint32>(AtlasFormatIds.size()));
        }
    }

    HnMaterialSRBCache::ResourceKey SRBKey;

    bool                   AllTexturesInAtlases = true;
    std::vector<ITexture*> Textures(TexturesArraySize);

    // Texture name to texture object mapping, for example:
    //     "diffuseColor" -> pDiffuseColorTex
    //     "normal"       -> pNormalTex
    std::unordered_map<pxr::TfToken, ITexture*, pxr::TfToken::HashFunctor> TexNameToTexture;

    PBR_Renderer::StaticShaderTextureIdsArrayType StaticShaderTexIds;
    StaticShaderTexIds.fill(PBR_Renderer::InvalidMaterialTextureId);

    for (Uint32 id = 0; id < PBR_Renderer::TEXTURE_ATTRIB_ID_COUNT; ++id)
    {
        const pxr::TfToken& TexName = PBRTextureAttribIdToPxrName(static_cast<PBR_Renderer::TEXTURE_ATTRIB_ID>(id));
        if (TexName.IsEmpty())
            continue;

        auto tex_it = m_Textures.find(TexName);
        if (tex_it == m_Textures.end())
        {
            UNEXPECTED("Texture '", TexName, "' is not found. This is unexpected as at least the default texture must always be set.");
            continue;
        }

        ITexture* pTexture = nullptr;

        const HnTextureRegistry::TextureHandleSharedPtr& pTexHandle = tex_it->second;
        if (pTexHandle->pTexture)
        {
            const auto& TexDesc = pTexHandle->pTexture->GetDesc();
            VERIFY(TexDesc.Type == RESOURCE_DIM_TEX_2D_ARRAY, "2D textures should be loaded as single-slice 2D array textures");
            pTexture = pTexHandle->pTexture;

            AllTexturesInAtlases = false;
        }
        else if (pTexHandle->pAtlasSuballocation)
        {
            pTexture = pTexHandle->pAtlasSuballocation->GetAtlas()->GetTexture();

            const TEXTURE_FORMAT AtlasFmt = pTexture->GetDesc().Format;

            auto it = AtlasFormatIds.find(AtlasFmt);
            if (it != AtlasFormatIds.end())
            {
                // StaticShaderTexIds[TEXTURE_ATTRIB_ID_BASE_COLOR] -> Atlas 0
                // StaticShaderTexIds[TEXTURE_ATTRIB_ID_METALLIC]   -> Atlas 1
                StaticShaderTexIds[id] = it->second;
            }
            else
            {
                UNEXPECTED("Texture atlas '", TexName, "' was not found in AtlasFormatIds. This looks to be a bug.");
            }
        }
        else
        {
            UNEXPECTED("Texture '", TexName, "' is not initialized. This likely indicates that HnRenderDelegate::CommitResources() was not called.");
            continue;
        }

        TexNameToTexture.emplace(TexName, pTexture);

        if (!m_UsesAtlas)
        {
            SRBKey.UniqueIDs.push_back(pTexture ? pTexture->GetUniqueID() : 0);
        }
    }

    HnTextureRegistry::TextureHandleSharedPtr WhiteTex;
    if (m_UsesAtlas)
    {
        if (AllTexturesInAtlases)
        {
            // Set texture atlases according to their indices in AtlasFormatIds, for example
            // Textures[0] -> Atlas 0 (RGBA8_UNORM)
            // Textures[1] -> Atlas 1 (R8_UNORM)
            // Textures[2] -> Atlas 2 (RGBA8_UNORM_SRGB)
            for (auto it : AtlasFormatIds)
            {
                auto* pTexture = RendererDelegate.GetResourceManager().GetTexture(it.first);
                VERIFY_EXPR(pTexture != nullptr);
                Textures[it.second] = pTexture;
            }

            // Set unused textures to white texture
            for (auto& Tex : Textures)
            {
                if (!Tex)
                {
                    if (!WhiteTex)
                    {
                        WhiteTex = GetDefaultTexture(RendererDelegate.GetTextureRegistry(), HnTokens->diffuseColor);
                        VERIFY_EXPR(WhiteTex->pAtlasSuballocation);
                    }
                    Tex = WhiteTex->pAtlasSuballocation->GetAtlas()->GetTexture();
                }
            }
        }
        else
        {
            UNEXPECTED("TODO");
        }

        // Construct SRB key from texture atlas object ids
        for (auto& Tex : Textures)
        {
            VERIFY_EXPR(Tex);
            SRBKey.UniqueIDs.push_back(Tex ? Tex->GetUniqueID() : 0);
        }

        m_ShaderTextureIndexingId = SRBCache->AddShaderTextureIndexing(StaticShaderTexIds);
    }

    m_SRB = SRBCache->GetSRB(SRBKey, [&]() {
        RefCntAutoPtr<IShaderResourceBinding> pSRB;

        UsdRenderer.CreateResourceBinding(&pSRB);
        VERIFY_EXPR(pSRB);

        if (IShaderResourceVariable* pVar = pSRB->GetVariableByName(SHADER_TYPE_PIXEL, "cbPrimitiveAttribs"))
        {
            // Primitive attribs buffer is a large buffer that fits multiple primitives.
            // In the render loop, we write multiple primitive attribs into this buffer
            // and use the SetBufferOffset function to select the attribs for the current primitive.
            pVar->SetBufferRange(UsdRenderer.GetPBRPrimitiveAttribsCB(), 0, UsdRenderer.GetPBRPrimitiveAttribsSize(HnRenderPass::GetMaterialPSOFlags(*this)));
        }
        else
        {
            UNEXPECTED("Failed to find 'cbPrimitiveAttribs' variable in the shader resource binding");
        }

        UsdRenderer.InitCommonSRBVars(pSRB, RendererDelegate.GetFrameAttribsCB());

        if (m_UsesAtlas)
        {
            if (IShaderResourceVariable* pVar = pSRB->GetVariableByName(SHADER_TYPE_PIXEL, "g_MaterialTextures"))
            {
                std::vector<IDeviceObject*> TextureViews(TexturesArraySize);
                for (Uint32 i = 0; i < TexturesArraySize; ++i)
                {
                    VERIFY_EXPR(Textures[i]);
                    TextureViews[i] = Textures[i] ? Textures[i]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
                }
                pVar->SetArray(TextureViews.data(), 0, TexturesArraySize);
            }
        }
        else
        {
            for (Uint32 id = 0; id < PBR_Renderer::TEXTURE_ATTRIB_ID_COUNT; ++id)
            {
                const PBR_Renderer::TEXTURE_ATTRIB_ID ID = static_cast<PBR_Renderer::TEXTURE_ATTRIB_ID>(id);

                const pxr::TfToken& TexName = PBRTextureAttribIdToPxrName(ID);
                if (TexName.IsEmpty())
                    continue;

                auto tex_it = TexNameToTexture.find(TexName);
                if (tex_it == TexNameToTexture.end())
                {
                    UNEXPECTED("Texture '", TexName, "' is not found. This is unexpected as at least the default texture must always be set.");
                    continue;
                }
                VERIFY_EXPR(tex_it->second);
                UsdRenderer.SetMaterialTexture(pSRB, tex_it->second->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), ID);
            }
        }

        return pSRB;
    });

    if (m_SRB)
    {
        m_PrimitiveAttribsVar = m_SRB->GetVariableByName(SHADER_TYPE_PIXEL, "cbPrimitiveAttribs");
        VERIFY_EXPR(m_PrimitiveAttribsVar != nullptr);
    }
    else
    {
        UNEXPECTED("Failed to create shader resource binding for material ", GetId());
    }

    m_AtlasVersion = AtlasVersion;
}

} // namespace USD

} // namespace Diligent
