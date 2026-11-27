#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRECOMPILE_DIR="${SCRIPT_DIR}/precompile"
TOOLS_DIR="${SCRIPT_DIR}/../shader_tools"
DXC="${TOOLS_DIR}/dxc.exe"
CREATE_HEADER="${TOOLS_DIR}/create_header.py"

SHADERS=(
    "nl_prep"
    "nl_pyramid"
    "nl_neural_prior"
    "nl_gi_temporal"
    "nl_denoise"
    "nl_composite"
)

cd "${PRECOMPILE_DIR}"

for s in "${SHADERS[@]}"; do
    echo "=== Compiling ${s} (DX12 DXIL cs_6_0) ==="
    WINEDEBUG=-all wine "${DXC}" -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect \
        "${s}.hlsl" -Fo "${s}_Shader.cso"
    python3 "${CREATE_HEADER}" "${s}_Shader.cso" "${s}_Shader.h" "${s}_cso"

    echo "=== Compiling ${s} (Vulkan SPIR-V cs_6_0) ==="
    WINEDEBUG=-all wine "${DXC}" -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE \
        -fspv-target-env=vulkan1.1 -Cc -Vi "${s}.hlsl" -Fo "${s}_Shader_Vk.spv"
    spirv-val --target-env vulkan1.1 "${s}_Shader_Vk.spv"
    python3 "${CREATE_HEADER}" "${s}_Shader_Vk.spv" "${s}_Shader_Vk.h" "${s}_spv"
done

echo "All 6 Neural Look shaders compiled and validated successfully."
