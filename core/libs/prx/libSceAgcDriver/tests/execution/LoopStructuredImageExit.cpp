#define main ExistingLoopImageMain
#include "LoopImageContinue.cpp"
#undef main
#include "Optimization/ResourceProgram.hpp"
#include "BdaAbi.hpp"
#include <spirv/unified1/spirv.hpp>

namespace {

std::vector<std::uint32_t> BodyCode(bool fragment, bool write, bool implicit, bool phi) {
    std::vector<std::uint32_t> code;
    for (std::uint32_t reg = 1; reg <= 6; ++reg) Vop1(code, 1u, reg, 0x80u);
    code.insert(code.end(), {0xbea80380, 0xbea90310});
    if (phi) {
        Vop1(code, 6u, 12u, 0x100u);
        Vop1(code, 1u, 7u, 0x100u);
        code.push_back(0x340e0e82);
    }
    const auto header = code.size();
    code.insert(code.end(), {0xbf820000, 0xbf0a2928, 0xbf840000});
    const auto exit = code.size() - 1u;
    if (write) code.insert(code.end(), {0x7e060228, 0x7e100c28, 0x061010f2, 0xf0200128, 0x00000801});
    else if (implicit) {
        Literal(code, 1u, 0.0625f);
        Vop1(code, 6u, 2u, 40u);
        Literal(code, 6u, 0.125f);
        code.insert(code.end(), {0x10040d02, 0xf0800f08, 0x00400801});
    } else {
        Vop1(code, 6u, 3u, 40u);
        code.insert(code.end(), {0xf09c0f28, 0x00400801});
    }
    code.push_back(0xbf8c3f70);
    if (phi) code.push_back(0x0618110c);
    code.push_back(0x80288128);
    const auto offset = static_cast<std::int32_t>(header) - static_cast<std::int32_t>(code.size() + 1u);
    code.push_back(0xbf820000u | (static_cast<std::uint32_t>(offset) & 0xffffu));
    code.at(exit) |= static_cast<std::uint32_t>(code.size() - exit - 1u);
    if (fragment) code.insert(code.end(), {0xf800180f, 0x0b0a0908});
    else if (phi) code.insert(code.end(), {0xe0701000, 0x80030c07});
    code.push_back(0xbf810000);
    return code;
}

void CheckBodyShape(const RecompileRequest& request) {
    const auto program = PrepareResourceProgram(request);
    const auto& blocks = program.BlockOrder();
    const auto& infos = program.Metadata().blockInfo;
    const auto find = [&](std::uint32_t id) {
        for (std::size_t i = 0; i < infos.size(); ++i) if (infos[i].id == id) return i;
        throw std::runtime_error("body-exit metadata target missing");
    };
    std::uint32_t matches = 0;
    for (const auto& header : infos) {
        const auto& loop = header.terminator;
        if (!loop.loopHeader || loop.continueBlock == header.id) continue;
        const auto cont = find(loop.continueBlock);
        const auto& end = infos[cont].terminator;
        if (end.kind != TerminatorKind::Branch || end.trueBlock != header.id) continue;
        if (blocks[cont]->Predecessors().size() != 1u) continue;
        const auto* body = blocks[cont]->Predecessors().front();
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i] != body || infos[i].id == header.id) continue;
            const auto& branch = infos[i].terminator;
            if (!branch.loopHeader && branch.kind == TerminatorKind::ConditionalBranch && branch.mergeBlock == InvalidControlFlowId &&
                ((branch.trueBlock == loop.continueBlock && branch.falseBlock == loop.mergeBlock) ||
                 (branch.falseBlock == loop.continueBlock && branch.trueBlock == loop.mergeBlock))) ++matches;
        }
    }
    Require(matches == 1u, "synthetic fixture must retain one own-loop body exit/continue-entry and unconditional latch");
}

void BodyPhi(AgcDriver::VulkanDevice& device, std::uint32_t wave, std::uint32_t limit) {
    constexpr std::uint32_t threads = 64;
    Fill(true);
    PhiOutput.fill(Sentinel);
    const auto code = BodyCode(false, false, false, true);
    auto user = UserData(false);
    const auto output = Buffer(PhiOutput.data(), 0u, sizeof(PhiOutput));
    std::copy(output.begin(), output.end(), user.begin() + 12);
    user.push_back(limit);
    const auto memory = Memory(code);
    const ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {wave, 0u, user, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(wave), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    CheckBodyShape(request);
    const auto result = Recompile(request);
    device.Dispatch(result, 1u, 1u, 1u);
    device.WaitIdle();
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        float expected = static_cast<float>(lane);
        for (std::uint32_t iteration = 0; iteration < limit; ++iteration) expected += static_cast<float>(Value(iteration)[0]) / 255.0f;
        const auto actual = std::bit_cast<float>(PhiOutput[lane]);
        Require(std::fabs(actual - expected) < 1e-4f, "structured body Phi value: wave" + std::to_string(wave) + " limit " + std::to_string(limit) + " lane " + std::to_string(lane) + " actual " + std::to_string(actual));
    }
    for (std::uint32_t word = threads; word < PhiOutput.size(); ++word) Require(PhiOutput[word] == Sentinel, "structured body Phi output sentinel overwritten");
    std::printf("body exit wave%u limit%u: 64 distinct lane values, iterative image sums and sentinels passed\n", wave, limit);
}

void ValidateBody(bool fragment, bool write, bool implicit, std::uint32_t wave, std::uint32_t limit) {
    const auto code = BodyCode(fragment, write, implicit, false);
    const std::array<std::uint32_t, 17> user{0x3000u, 22u << 20u | 3u << 30u, 3u << 14u, implicit ? 0x90000facu : 0xd0000facu, implicit ? 0u : 3u, 0u, 0u, 0u,
        2u | 2u << 3u | 2u << 6u, 0u, 0u, 0u, 0x200000u, 0u, 256u, 0x31016facu, limit};
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
    CheckBodyShape(request);
    const auto result = Recompile(request);
    Require(!result.spirv.Words().empty(), "body-exit stage emitted no module");
    std::printf("body validation %s %s wave%u limit%u passed\n", fragment ? "fragment" : "compute", write ? "write" : implicit ? "implicit" : "sample", wave, limit);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string_view(argv[1]) == "--cpu") {
            const std::string_view stage(argv[2]), kind(argv[3]);
            const auto wave = std::stoul(argv[4]), limit = std::stoul(argv[5]);
            Require((stage == "compute" || stage == "fragment") && (kind == "sample" || kind == "write" || (stage == "fragment" && kind == "implicit")) &&
                    (wave == 32u || wave == 64u) && (limit == 0u || limit == 2u || limit == 4u), "unknown public body-exit case");
            ValidateBody(stage == "fragment", kind == "write", kind == "implicit", wave, limit);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--validation") {
            for (const auto fragment : {false, true}) for (const auto kind : {0u, 1u, 2u}) {
                if (!fragment && kind == 2u) continue;
                for (const auto wave : {32u, 64u}) for (const auto limit : {0u, 2u, 4u}) ValidateBody(fragment, kind == 1u, kind == 2u, wave, limit);
            }
            std::puts("structured image body validation passed: 30 public stage/mode/wave/exit cases");
            return 0;
        }
        const bool single = argc == 4 && std::string_view(argv[1]) == "--case";
        if (argc != 1 && !single) throw std::runtime_error("select --validation, --cpu or --case for public body/Phi cases");
        RegisteredImages images;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::printf("structured image body device %s, native subgroup %u\n", device->DeviceName().c_str(), device->Target().subgroupSize);
        if (single) {
            const auto wave = std::stoul(argv[2]), limit = std::stoul(argv[3]);
            Require((wave == 32u || wave == 64u) && (limit == 0u || limit == 2u || limit == 4u), "unknown native body-exit case");
            BodyPhi(*device, wave, limit);
            return 0;
        }
        for (const auto wave : {32u, 64u}) for (const auto limit : {0u, 2u, 4u}) BodyPhi(*device, wave, limit);
        std::puts("structured image body tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
