#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include "ffx_shader_blobs.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <utility>

static bool CheckBindings(uint32_t count, const char** names, const uint32_t* bindings,
                          const uint32_t* counts, const uint32_t* spaces,
                          std::set<std::pair<uint32_t, uint32_t>>& used)
{
    if (count && (!names || !bindings || !counts || !spaces))
        return false;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (!names[index] || !names[index][0] || !counts[index])
            return false;
        if (!used.insert({spaces[index], bindings[index]}).second)
            return false;
    }
    return true;
}

static bool CheckBlob(const FfxShaderBlob& blob)
{
    uint32_t magic = 0;
    if (!blob.data || blob.size < 20 || blob.size % sizeof(uint32_t))
        return false;
    std::memcpy(&magic, blob.data, sizeof(magic));
    if (magic != 0x07230203 || reinterpret_cast<uintptr_t>(blob.data) % alignof(uint32_t))
        return false;
    std::set<std::pair<uint32_t, uint32_t>> used;
    return CheckBindings(blob.cbvCount, blob.boundConstantBufferNames, blob.boundConstantBuffers,
                         blob.boundConstantBufferCounts, blob.boundConstantBufferSpaces, used)
        && CheckBindings(blob.srvTextureCount, blob.boundSRVTextureNames, blob.boundSRVTextures,
                         blob.boundSRVTextureCounts, blob.boundSRVTextureSpaces, used)
        && CheckBindings(blob.uavTextureCount, blob.boundUAVTextureNames, blob.boundUAVTextures,
                         blob.boundUAVTextureCounts, blob.boundUAVTextureSpaces, used)
        && CheckBindings(blob.srvBufferCount, blob.boundSRVBufferNames, blob.boundSRVBuffers,
                         blob.boundSRVBufferCounts, blob.boundSRVBufferSpaces, used)
        && CheckBindings(blob.uavBufferCount, blob.boundUAVBufferNames, blob.boundUAVBuffers,
                         blob.boundUAVBufferCounts, blob.boundUAVBufferSpaces, used)
        && CheckBindings(blob.samplerCount, blob.boundSamplerNames, blob.boundSamplers,
                         blob.boundSamplerCounts, blob.boundSamplerSpaces, used)
        && CheckBindings(blob.rtAccelStructCount, blob.boundRTAccelerationStructureNames,
                         blob.boundRTAccelerationStructures, blob.boundRTAccelerationStructureCounts,
                         blob.boundRTAccelerationStructureSpaces, used);
}

int main()
{
    uint32_t tested = 0;
    uint32_t failed = 0;
    uint32_t binding_count = 0;
    for (uint32_t effect_index = 0; effect_index < 2; ++effect_index)
    {
        const auto effect = effect_index ? FFX_EFFECT_OPTICALFLOW : FFX_EFFECT_FRAMEINTERPOLATION;
        const uint32_t pass_count = effect_index ? FFX_OPTICALFLOW_PASS_COUNT : FFX_FRAMEINTERPOLATION_PASS_COUNT;
        const uint32_t permutation_count = effect_index ? 8 : 32;
        for (uint32_t pass = 0; pass < pass_count; ++pass)
        {
            for (uint32_t permutation = 0; permutation < permutation_count; ++permutation)
            {
                FfxShaderBlob blob{};
                const auto error = ffxGetPermutationBlobByIndex(effect, pass, FFX_BIND_COMPUTE_SHADER_STAGE,
                                                               permutation, &blob);
                ++tested;
                if (error != FFX_OK || !CheckBlob(blob))
                {
                    ++failed;
                    std::cerr << "Invalid blob: effect=" << effect_index << " pass=" << pass
                              << " permutation=" << permutation << " error=" << error << '\n';
                }
                binding_count += blob.cbvCount + blob.srvTextureCount + blob.uavTextureCount
                               + blob.srvBufferCount + blob.uavBufferCount + blob.samplerCount;
            }
        }
    }
    std::cout << "{\"architecture\":\"x86\",\"permutations_tested\":" << tested
              << ",\"failed\":" << failed << ",\"bindings_checked\":" << binding_count
              << ",\"real_spirv\":true,\"aligned_bytecode\":true}" << '\n';
    return failed ? 1 : 0;
}
