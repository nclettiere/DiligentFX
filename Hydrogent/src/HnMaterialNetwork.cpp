/*
 *  Copyright 2023 Diligent Graphics LLC
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

#include "HnMaterialNetwork.hpp"
#include "HnTokens.hpp"

#include "Errors.hpp"

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hio/glslfx.h"
#include "pxr/usd/sdr/registry.h"
#include "pxr/usd/sdr/shaderProperty.h"
#include "pxr/usd/sdf/valueTypeName.h"

namespace Diligent
{

namespace USD
{

HnMaterialParameter::HnMaterialParameter()
{
}

HnMaterialParameter::~HnMaterialParameter()
{
}

HnMaterialParameter::HnMaterialParameter(
    ParamType                 _Type,
    const pxr::TfToken&       _Name,
    const pxr::VtValue&       _FallbackValue,
    const pxr::TfTokenVector& _SamplerCoords,
    pxr::HdTextureType        _TextureType,
    const std::string&        _Swizzle,
    bool                      _IsPremultiplied,
    size_t                    _ArrayOfTexturesSize) :
    // clang-format off
    Type               {_Type},
    Name               {_Name},
    FallbackValue      {_FallbackValue},
    SamplerCoords      {_SamplerCoords},
    TextureType        {_TextureType},
    Swizzle            {_Swizzle},
    IsPremultiplied    {_IsPremultiplied},
    ArrayOfTexturesSize{_ArrayOfTexturesSize}
{}

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    HnMaterialPrivateTokens,
    (opacity)
    (opacityThreshold)
    (isPtex)
    (st)
    (uv)
    (fieldname)
    (diffuseColor)
    (a)

    (HwUvTexture_1)
    (textureMemory)
    (sourceColorSpace)
    (in)

    (mtlx)
);
// clang-format on

HnMaterialNetwork::HnMaterialNetwork()
{
}

HnMaterialNetwork::~HnMaterialNetwork()
{
}

namespace
{

const pxr::HdMaterialNode2* GetTerminalNode(const pxr::HdMaterialNetwork2& Network,
                                            const pxr::TfToken&            TerminalToken,
                                            pxr::SdfPath&                  TerminalPath)
{
    // Get the Surface or Volume Terminal
    const auto terminal_it = Network.terminals.find(TerminalToken);
    if (terminal_it == Network.terminals.end())
        return nullptr;

    TerminalPath       = terminal_it->second.upstreamNode;
    const auto node_it = Network.nodes.find(TerminalPath);
    return &node_it->second;
}

using HioGlslfxSharedPtr = std::shared_ptr<pxr::HioGlslfx>;

HioGlslfxSharedPtr GetGlslfxForTerminal(const pxr::TfToken& NodeTypeId)
{
    // If there is a URI, we will use that, otherwise we will try to use
    // the source code.
    pxr::SdrRegistry&          ShaderReg = pxr::SdrRegistry::GetInstance();
    pxr::SdrShaderNodeConstPtr SdrNode   = ShaderReg.GetShaderNodeByIdentifierAndType(NodeTypeId, pxr::HioGlslfxTokens->glslfx);

    if (!SdrNode)
        return nullptr;

    const std::string& GlslfxFilePath = SdrNode->GetResolvedImplementationURI();
    if (!GlslfxFilePath.empty())
    {
        // TODO: cache the glslfx file
        return std::make_shared<pxr::HioGlslfx>(GlslfxFilePath);
    }
    else
    {
        const std::string& SourceCode = SdrNode->GetSourceCode();
        if (!SourceCode.empty())
        {
            // Do not use the registry for the source code to avoid
            // the cost of hashing the entire source code.
            return std::make_shared<pxr::HioGlslfx>(std::istringstream{SourceCode});
        }
    }

    return nullptr;
}

pxr::TfToken GetMaterialTag(const pxr::VtDictionary& Metadata, const pxr::HdMaterialNode2& Terminal)
{
    // Strongest materialTag opinion is a hardcoded tag in glslfx meta data.
    // This can be used for masked, additive, translucent or volume materials.
    // See HdMaterialTagTokens.
    pxr::VtValue vtMetaTag = TfMapLookupByValue(Metadata, pxr::HdShaderTokens->materialTag, pxr::VtValue{});
    if (vtMetaTag.IsHolding<std::string>())
    {
        return TfToken{vtMetaTag.UncheckedGet<std::string>()};
    }

    // Next check for authored terminal.opacityThreshold value > 0
    for (const auto param_it : Terminal.parameters)
    {
        if (param_it.first != HnMaterialPrivateTokens->opacityThreshold)
            continue;

        const pxr::VtValue& vtOpacityThreshold = param_it.second;
        if (vtOpacityThreshold.Get<float>() > 0.0f)
        {
            return HnMaterialTagTokens->masked;
        }
    }

    // Next strongest opinion is a connection to 'terminal.opacity'
    auto IsTranslucent = (Terminal.inputConnections.find(HnMaterialPrivateTokens->opacity) != Terminal.inputConnections.end());

    // Weakest opinion is an authored terminal.opacity value.
    if (!IsTranslucent)
    {
        for (const auto param_it : Terminal.parameters)
        {
            if (param_it.first != HnMaterialPrivateTokens->opacity)
                continue;

            const pxr::VtValue& vtOpacity = param_it.second;
            IsTranslucent                 = vtOpacity.Get<float>() < 1.0f;
            break;
        }
    }

    return IsTranslucent ? HnMaterialTagTokens->translucent : HnMaterialTagTokens->defaultTag;
}

// Get the fallback value for material node, first consulting Sdr to find
// whether the node has an input for the fallback value and then checking
// whether the output named outputName is known to Sdr and using either
// the default value specified by the SdrShaderProperty or using a
// default constructed value of the type specified by SdrShaderProperty.
static pxr::VtValue GetNodeFallbackValue(const pxr::HdMaterialNode2& Node,
                                         const pxr::TfToken&         OutputName)
{
    pxr::SdrRegistry& ShaderReg = pxr::SdrRegistry::GetInstance();

    // Find the corresponding Sdr node.
    const pxr::SdrShaderNodeConstPtr SdrNode =
        ShaderReg.GetShaderNodeByIdentifierAndType(Node.nodeTypeId, pxr::HioGlslfxTokens->glslfx);
    if (!SdrNode)
    {
        return pxr::VtValue{};
    }


    // HACK: Incorrect usage of GetDefaultInput to
    // determine what the fallback value is.
    // GetDefaultInput is meant to be used for 'disabled'
    // node where the 'default input' becomes the value
    // pass-through in the network. But there is no other
    // mechanism currently to deal with fallback values.
    if (const pxr::SdrShaderPropertyConstPtr& DefaultInput = SdrNode->GetDefaultInput())
    {
        const pxr::TfToken& DefInputName = DefaultInput->GetName();
        auto const&         def_param_it = Node.parameters.find(DefInputName);
        if (def_param_it != Node.parameters.end())
        {
            return def_param_it->second;
        }
    }

    // Sdr supports specifying default values for outputs so if we
    // did not use the GetDefaultInput hack above, we fallback to
    // using this DefaultOutput value.
    if (const pxr::SdrShaderPropertyConstPtr& Output = SdrNode->GetShaderOutput(OutputName))
    {
        const pxr::VtValue Out = Output->GetDefaultValue();
        if (!Out.IsEmpty())
        {
            return Out;
        }

        // If no default value was registered with Sdr for
        // the output, fallback to the type's default.
        return Output->GetTypeAsSdfType().first.GetDefaultValue();
    }

    return pxr::VtValue{};
}


pxr::VtValue GetParamFallbackValue(const pxr::HdMaterialNetwork2& Network,
                                   const pxr::HdMaterialNode2&    Node,
                                   const pxr::TfToken&            ParamName)
{
    // The 'fallback value' will be the value of the material param if nothing
    // is connected or what is connected is mis-configured. For example a
    // missing texture file.

    // Check if there are any connections to the terminal input.
    {
        const auto conn_it = Node.inputConnections.find(ParamName);
        if (conn_it != Node.inputConnections.end())
        {
            if (!conn_it->second.empty())
            {
                const pxr::HdMaterialConnection2& con          = conn_it->second.front();
                const auto&                       pn_it        = Network.nodes.find(con.upstreamNode);
                const pxr::HdMaterialNode2&       UpstreamNode = pn_it->second;

                const pxr::VtValue FallbackValue =
                    GetNodeFallbackValue(UpstreamNode, con.upstreamOutputName);
                if (!FallbackValue.IsEmpty())
                {
                    return FallbackValue;
                }
            }
        }
    }

    // If there are no connections, there may be an authored value.
    {
        const auto param_it = Node.parameters.find(ParamName);
        if (param_it != Node.parameters.end())
        {
            return param_it->second;
        }
    }

    // If we had nothing connected, but we do have an Sdr node, we can use the
    // DefaultValue for the input as specified in the Sdr schema.
    // E.g. PreviewSurface is a terminal with an Sdr schema.
    {
        pxr::SdrRegistry& ShaderReg = pxr::SdrRegistry::GetInstance();

        pxr::SdrShaderNodeConstPtr TerminalSdr =
            ShaderReg.GetShaderNodeByIdentifierAndType(Node.nodeTypeId, pxr::HioGlslfxTokens->glslfx);

        if (TerminalSdr)
        {
            if (const pxr::SdrShaderPropertyConstPtr& Input = TerminalSdr->GetShaderInput(ParamName))
            {
                pxr::VtValue Out = Input->GetDefaultValue();
                // If no default value was registered with Sdr for
                // the output, fallback to the type's default.
                if (Out.IsEmpty())
                {
                    Out = Input->GetTypeAsSdfType().first.GetDefaultValue();
                }

                if (!Out.IsEmpty())
                {
                    return Out;
                }
            }
        }
    }

    // Returning an empty value will likely result in a shader compile error,
    // because the buffer source will not be able to determine the HdTupleType.
    // Hope for the best and return a vec3.
    LOG_WARNING_MESSAGE("Couldn't determine default value for: ", ParamName.GetText(), " on nodeType: ", Node.nodeTypeId.GetText());

    return pxr::VtValue{pxr::GfVec3f{0}};
}

} // namespace

HnMaterialNetwork::HnMaterialNetwork(const pxr::SdfPath&              SdfPath,
                                     const pxr::HdMaterialNetworkMap& hdNetworkMap) noexcept(false)
{
    // The fragment source comes from the 'surface' network or the
    // 'volume' network.
    bool                    IsVolume      = false;
    pxr::HdMaterialNetwork2 Network2      = HdConvertToHdMaterialNetwork2(hdNetworkMap, &IsVolume);
    const pxr::TfToken&     TerminalToken = IsVolume ? pxr::HdMaterialTerminalTokens->volume : pxr::HdMaterialTerminalTokens->surface;

    pxr::SdfPath                TerminalPath;
    const pxr::HdMaterialNode2* TerminalNode = GetTerminalNode(Network2, TerminalToken, TerminalPath);
    if (TerminalNode == nullptr)
        return;

    // Extract the glslfx and metadata for surface/volume.
    auto Glslfx = GetGlslfxForTerminal(TerminalNode->nodeTypeId);
    if (!Glslfx || !Glslfx->IsValid())
        return;

    m_Metadata = Glslfx->GetMetadata();
    m_Tag      = GetMaterialTag(m_Metadata, *TerminalNode);

    LoadMaterialParams(Network2, *TerminalNode);
}

void HnMaterialNetwork::LoadMaterialParams(const pxr::HdMaterialNetwork2& Network,
                                           const pxr::HdMaterialNode2&    Node)
{
    // Hydrogent currently supports two material configurations.
    // A custom glslfx file or a PreviewSurface material network.
    // Either configuration consists of a terminal (Shader or PreviewSurface)
    // with its input values authored or connected to a primvar, texture or
    // volume node. The texture may have a primvar connected to provide UVs.
    //
    // The following code is made to process one of these two material configs
    // exclusively. It cannot convert arbitrary material networks to Storm by
    // generating the appropriate glsl code.

    pxr::SdrRegistry& ShaderReg = pxr::SdrRegistry::GetInstance();

    const pxr::SdrShaderNodeConstPtr SdrNode =
        ShaderReg.GetShaderNodeByIdentifierAndType(Node.nodeTypeId, pxr::HioGlslfxTokens->glslfx);

    if (SdrNode)
    {
        pxr::SdfPathSet VisitedNodes;
        for (const TfToken& InputName : SdrNode->GetInputNames())
        {
            //_MakeParamsForInputParameter(
            //    network, node, inputName, &visitedNodes,
            //    params, textureDescriptors, materialTag);
        }
    }
    else
    {
        LOG_WARNING_MESSAGE("Unrecognized node: ", Node.nodeTypeId.GetText());
    }

    // Set fallback values for the inputs on the terminal (excepting
    // referenced sampler coords).
    for (auto& Param : m_Parameters)
    {
        if (Param.Type != HnMaterialParameter::ParamType::AdditionalPrimvar &&
            Param.FallbackValue.IsEmpty())
        {
            Param.FallbackValue = GetParamFallbackValue(Network, Node, Param.Name);
        }
    }

    if (SdrNode)
    {
        // Create HnMaterialParameter for each primvar the terminal says it
        // needs.
        // Primvars come from 'attributes' in the glslfx and are seperate from
        // the input 'parameters'. We need to create a material param for them
        // so that these primvars survive 'primvar filtering' that discards any
        // unused primvars on the mesh.
        // If the network lists additional primvars, we add those too.
        pxr::NdrTokenVec Primvars = SdrNode->GetPrimvars();
        Primvars.insert(Primvars.end(), Network.primvars.begin(), Network.primvars.end());
        std::sort(Primvars.begin(), Primvars.end());
        Primvars.erase(std::unique(Primvars.begin(), Primvars.end()), Primvars.end());

        for (const TfToken& PrimvarName : Primvars)
        {
            //_MakeMaterialParamsForAdditionalPrimvar(primvarName, params);
        }
    }
}

} // namespace USD

} // namespace Diligent
