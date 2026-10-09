#include "Recompiler.hpp"
#include "BdaAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <iostream>
#include <string_view>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void CompileCase(std::string_view selected) {
    const bool fragment = selected.starts_with("fragment");
    const bool write = selected.find("write") != std::string_view::npos;
    const bool implicit = selected.find("implicit") != std::string_view::npos;
    const bool loop = selected.ends_with("loop");
    const std::uint32_t wave = selected.find("64") != std::string_view::npos ? 64u : 32u;
    std::vector<std::uint32_t> code{0xbea80380, 0x7e020280, 0x7e040280, 0x7e060280, 0x7e080280, 0x7e0a0280, 0x7e0c0280};
    const auto begin = code.size();
    if (write) code.insert(code.end(), {0x7e060228, 0x7e100c28, 0x061010f2, 0xf0200128, 0x00000801});
    else if (implicit) code.insert(code.end(), {0x7e040c28, 0xf0800f08, 0x00400801});
    else code.insert(code.end(), {0x7e060c28, 0xf09c0f28, 0x00400801});
    if (loop) {
        code.insert(code.end(), {0x80288128, 0xbf0a8428});
        const auto offset = static_cast<std::int32_t>(begin) - static_cast<std::int32_t>(code.size() + 1u);
        code.push_back(0xbf850000u | (static_cast<std::uint32_t>(offset) & 0xffffu));
    }
    if (fragment) code.insert(code.end(), {0xf800180f, 0x0b0a0908});
    else if (!write) code.insert(code.end(), {0xe0700000, 0x80030800});
    code.push_back(0xbf810000);
    const std::array<std::uint32_t, 16> user{0x3000u, 22u << 20u | 3u << 30u, 3u << 14u, implicit ? 0x90000facu : 0xd0000facu, implicit ? 0u : 3u, 0u, 0u, 0u,
        2u | 2u << 3u | 2u << 6u, 0u, 0u, 0u, 0x200000u, 0u, 256u, 0x31016facu};
    constexpr std::array<std::uint32_t, 14> capabilities{spv::CapabilityShader, spv::CapabilityImageGatherExtended,
        spv::CapabilityStorageImageExtendedFormats, spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityStorageImageWriteWithoutFormat,
        spv::CapabilityImageCubeArray, spv::CapabilitySampled1D, spv::CapabilityImage1D, spv::CapabilitySampledBuffer, spv::CapabilityImageBuffer,
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess, spv::CapabilitySampledCubeArray};
    constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    RecompileRequest request{};
    request.shader = {fragment ? ShaderStage::Fragment : ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}};
    request.context.waveSize = wave;
    request.context.userData = user;
    if (fragment) {
        ShaderPixelStageInfo pixel{};
        pixel.wave32 = wave == 32u;
        pixel.targetOutputMode[0] = 9u;
        pixel.targetExportMapping.fill(0xe4u);
        request.context.pixel = pixel;
    } else request.context.compute = ShaderComputeStageInfo{{1u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64u;
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.layout.pushConstantSizeBytes = 128u;
    request.useCache = false;
    const auto result = Recompile(request);
    if (result.spirv.Words().empty()) throw std::runtime_error("synthetic stage emitted no module");
    std::cout << "PASS " << selected << '\n';
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2) CompileCase(argv[1]);
        else if (argc == 1) {
            for (const auto stage : {"compute", "fragment"}) {
                for (const auto kind : {"sample", "write"}) {
                    for (const auto wave : {"32", "64"}) {
                        for (const auto flow : {"outside", "loop"}) CompileCase(std::string(stage) + "-" + kind + "-" + wave + "-" + flow);
                    }
                }
            }
            for (const auto wave : {"32", "64"}) {
                for (const auto flow : {"outside", "loop"}) CompileCase(std::string("fragment-implicit-") + wave + "-" + flow);
            }
            std::puts("loop image continue validation passed: 20 public stage/write/implicit cases");
        } else throw std::runtime_error("select a synthetic stage/write/wave/loop case or run all cases");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
