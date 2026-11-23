#include "pch.h"

#include "Shader_Common.h"
#include <Config.h>
#include <State.h>

#include <d3dcompiler.h>

#ifdef _DEBUG
#include <imgui/ImGuiNotify.hpp>
#endif

uint32_t GetColorEncodeMode(ShaderColorSpace srcSpace, ShaderColorSpace dstSpace)
{
    if (srcSpace == dstSpace)
        return static_cast<uint32_t>(ShaderColorEncodeMode::Passthrough);

    if (srcSpace == ShaderColorSpace::Linear && dstSpace == ShaderColorSpace::SrgbEncoded)
        return static_cast<uint32_t>(ShaderColorEncodeMode::LinearToSrgb);

    if (srcSpace == ShaderColorSpace::Linear && dstSpace == ShaderColorSpace::PQ)
        return static_cast<uint32_t>(ShaderColorEncodeMode::LinearToPQ);

    if (srcSpace == ShaderColorSpace::SrgbEncoded && dstSpace == ShaderColorSpace::Linear)
        return static_cast<uint32_t>(ShaderColorEncodeMode::SrgbToLinear);

    if (srcSpace == ShaderColorSpace::PQ && dstSpace == ShaderColorSpace::Linear)
        return static_cast<uint32_t>(ShaderColorEncodeMode::PQToLinear);

    if (srcSpace == ShaderColorSpace::SrgbEncoded && dstSpace == ShaderColorSpace::PQ)
        return static_cast<uint32_t>(ShaderColorEncodeMode::SrgbToPQ);

    if (srcSpace == ShaderColorSpace::PQ && dstSpace == ShaderColorSpace::SrgbEncoded)
        return static_cast<uint32_t>(ShaderColorEncodeMode::PQToSrgb);

    return static_cast<uint32_t>(ShaderColorEncodeMode::Passthrough);
}

float GetUiCompareDiffThreshold()
{
    if (const float cutoff = Config::Instance()->FGHudCutoff.value_or_default(); cutoff > 0.0f)
        return cutoff / 10.0f;

    const bool isCyberpunk = State::Instance().gameQuirks[GameQuirk::CyberpunkHudlessState];
    if (isCyberpunk && State::Instance().activeFgInput != FGInput::FSRFG)
        return 0.01f;

    return 0.03f;
}

ID3DBlob* CompileShader(const char* shaderCode, const char* entryPoint, const char* target)
{
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, entryPoint, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr))
    {
        LOG_ERROR("error while compiling shader");

#ifdef _DEBUG
        ImGui::InsertNotification({ ImGuiToastType::Error, 10000, "Error while compiling a shader!" });
#endif

        if (errorBlob)
        {
            LOG_ERROR("error while compiling shader : {0}", (char*) errorBlob->GetBufferPointer());
            errorBlob->Release();
        }

        if (shaderBlob)
            shaderBlob->Release();

        return nullptr;
    }

    if (errorBlob)
        errorBlob->Release();

    return shaderBlob;
}
