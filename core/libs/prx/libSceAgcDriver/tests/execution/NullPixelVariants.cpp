#define main PreparedShadersReferenceEntryPoint
#include "PreparedShaders.cpp"
#undef main
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <bit>
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" std::uint32_t* APS5_VABI sceAgcDcbDrawIndexAuto(CommandBuffer*, std::uint32_t, std::uint64_t);
extern "C" int APS5_VABI sceAgcDriverSubmitDcb(const Packet*);

namespace {

using namespace ShaderRecompiler;
using namespace AgcDriver::DriverDetail;

RecompileRequest NullRequest(AgcDriver::VulkanDevice& device, const ShaderSnapshot& snapshot, std::uint32_t wave, std::span<const std::uint32_t> users, std::uint32_t offset) {
    auto pixel = AgcDriver::Graphics::DecodePixelStageInfo({}, {}, true);
    pixel.wave32 = wave == 32u;
    return {{ShaderStage::Fragment, snapshot.codeAddress, snapshot.code, 0, {}},
        {wave, 0, users, {}, pixel, {}, {}}, device.Target(), {0, 0, offset, 128u - offset}};
}

std::shared_ptr<ShaderSnapshot> NullSnapshot() {
    auto snapshot = std::make_shared<ShaderSnapshot>();
    snapshot->codeAddress = NullPixelProgramAddress();
    snapshot->type = 1;
    snapshot->code.resize(64);
    snapshot->code.front() = 0xbf810000u;
    snapshot->header.resize(sizeof(Shader));
    return snapshot;
}

void Variants(AgcDriver::VulkanDevice& device, std::uint32_t wave) {
    for (const bool sourceFirst : {false, true}) {
        auto snapshot = NullSnapshot();
        const auto registered = NullRequest(device, *snapshot, 64, {}, 0);
        snapshot->prepared->entries.push_back({0, PrepareShader(registered)});
        const auto original = snapshot->prepared->entries.front().handle;
        std::array<std::uint32_t, 4> users{1, 2, 3, 4};
        auto request = NullRequest(device, *snapshot, wave, users, 20);
        if (sourceFirst) static_cast<void>(SourceHandleFor(*snapshot, 0, request));
        auto invocation = InvocationFor(*snapshot, 0, request);
        Require(snapshot->prepared->entries.size() == 2 && snapshot->prepared->entries.front().handle == original, "null variant overwrote the registered artifact");
        const auto handle = snapshot->prepared->entries.back().handle;
        const auto& artifact = GetPreparedArtifact(*handle);
        Require(artifact.layout.pushConstantOffsetBytes == 20, "null variant lost the shared push offset");
        for (const bool useCache : {false, true}) {
            request.useCache = useCache;
            Require(SourceHandleFor(*snapshot, 0, request) == handle && snapshot->prepared->entries.size() == 2, "null source cache was not reused");
            auto repeated = InvocationFor(*snapshot, 0, request);
            SrtRuntime runtime{};
            runtime.userData = users;
            const auto capture = repeated.Capture(runtime);
            const auto result = repeated.Materialize(*capture);
            Require(result->variantId == artifact.variantId && result->spirv.data() == artifact.spirv.data(), "null invocation replaced its compiled artifact");
        }
        const auto ownedCode = GetPreparedCode(*handle);
        Require(!ownedCode.empty() && ownedCode.front() == 0xbf810000u, "null artifact lost its owned program");
        snapshot.reset();
        users.fill(9);
        SrtRuntime runtime{};
        runtime.userData = users;
        const auto capture = invocation.Capture(runtime);
        Require(!invocation.Materialize(*capture)->spirv.empty(), "null invocation did not retain the snapshot lifetime");
        std::printf("null variant wave%u %s-first push20/cap108 cache/lifetime passed\n", wave, sourceFirst ? "source" : "invocation");
    }
}

void Guards(AgcDriver::VulkanDevice& device) {
    auto snapshot = NullSnapshot();
    auto fragment = NullRequest(device, *snapshot, 64, {}, 20);
    snapshot->codeAddress += 256u;
    fragment.shader.codeAddress = snapshot->codeAddress;
    ExpectFailure([&] { static_cast<void>(SourceHandleFor(*snapshot, 0, fragment)); }, "artifact is missing");
    ExpectFailure([&] { static_cast<void>(InvocationFor(*snapshot, 0, fragment)); }, "artifact is missing");
    Require(snapshot->prepared->entries.empty(), "non-null registered rejection published an artifact");
    RecompileRequest compute{{ShaderStage::Compute, snapshot->codeAddress, snapshot->code, 0, {}},
        {32, 0, {}, ShaderComputeStageInfo{{1, 1, 1}, 0, {false, false, false}, false, 1, {}}, {}, {}, {}},
        device.ComputeTarget(32), {0, 0, 0, 128}};
    snapshot->codeAddress = NullPixelProgramAddress();
    compute.shader.codeAddress = snapshot->codeAddress;
    ExpectFailure([&] { static_cast<void>(SourceHandleFor(*snapshot, 0, compute)); }, "artifact is missing");
    ExpectFailure([&] { static_cast<void>(InvocationFor(*snapshot, 0, compute)); }, "artifact is missing");
    snapshot->codeAddress += 256u;
    compute.shader.codeAddress = snapshot->codeAddress;
    snapshot->prepared->deferred = true;
    const auto deferred = SourceHandleFor(*snapshot, 0, compute);
    auto invocation = InvocationFor(*snapshot, 0, compute);
    SrtRuntime runtime{};
    auto capture = invocation.Capture(runtime);
    device.Dispatch(*invocation.Materialize(*capture), 1, 1, 1);
    device.WaitIdle();
    Require(snapshot->prepared->entries.size() == 1 && snapshot->prepared->entries.front().handle == deferred, "registered deferral did not preserve its cache");
    for (const bool deferredFlag : {false, true}) {
        for (const bool sourceFirst : {false, true}) {
            auto unregistered = NullSnapshot();
            unregistered->codeAddress += 256u;
            unregistered->header.clear();
            unregistered->type = 0;
            unregistered->prepared->deferred = deferredFlag;
            auto invalidStage = NullRequest(device, *unregistered, 64, {}, 20);
            ExpectFailure([&] {
                if (sourceFirst) static_cast<void>(SourceHandleFor(*unregistered, 0, invalidStage));
                else static_cast<void>(InvocationFor(*unregistered, 0, invalidStage));
            }, "unregistered program is not a compute shader");
            Require(unregistered->prepared->entries.empty(), "unregistered-stage rejection published an artifact");
        }
    }
    auto transactional = NullSnapshot();
    const auto entry = transactional->prepared->entries;
    {
        ShaderPreparationTransaction transaction;
        transaction.Edit(*transactional).entries.push_back({0, deferred});
    }
    Require(transactional->prepared->entries.size() == entry.size(), "aborted null preparation published staged entries");
    {
        ShaderPreparationTransaction transaction;
        transaction.Edit(*transactional).entries.push_back({0, deferred});
        transaction.Commit();
    }
    Require(transactional->prepared->entries.size() == 1, "committed null preparation did not publish its entry");
    auto malformed = NullSnapshot();
    auto invalid = NullRequest(device, *malformed, 64, {}, 20);
    invalid.layout.pushConstantOffsetBytes = 127;
    invalid.layout.pushConstantSizeBytes = 1;
    ExpectFailure([&] { static_cast<void>(InvocationFor(*malformed, 0, invalid)); }, "push");
    Require(malformed->prepared->entries.empty(), "invalid null preparation published an artifact");
    std::puts("null-only/stage/non-null/deferred rejection and transaction controls passed");
}

class DrawMemory {
public:
    DrawMemory() {
#ifdef _WIN32
        data = static_cast<std::byte*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        data = static_cast<std::byte*>(std::aligned_alloc(65536, 65536));
#endif
        Require(data != nullptr, "null draw allocation failed");
        std::memset(data, 0x40, 65536);
        GuestAllocations::Mutation().Add(data, 65536, true, true);
    }
    ~DrawMemory() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        VirtualFree(data, 0, MEM_RELEASE);
#else
        std::free(data);
#endif
    }
    std::byte* data = nullptr;
};

alignas(256) constexpr std::array<std::uint32_t, 6> DrawVertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 4> DrawWhiteCode{0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 4> DrawBlackCode{0x7e0e0280u, 0xf800180fu, 0x07070707u, 0xbf810000u};
alignas(256) std::array<std::array<float, 4>, 3> DrawTriangle{{{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}}};

struct DrawHeader {
    Shader shader{};
    std::array<ShaderRegister, 7> registers{};
    std::array<ShaderRegister, 5> context{};
    void Initialize(std::span<const std::uint32_t> code, std::uint8_t type, std::uint32_t wave) {
        shader.file_header = 0x34333231u;
        shader.version = 0x18u;
        shader.code = code.data();
        shader.header_size = sizeof(*this);
        shader.shader_size = code.size_bytes();
        shader.type = type;
        shader.sh_registers = registers.data();
        shader.num_sh_registers = type == 2 ? 7 : 3;
        shader.cx_registers = context.data();
        shader.num_cx_registers = context.size();
        const auto address = reinterpret_cast<std::uintptr_t>(code.data());
        const auto program = type == 2 ? 0xc8u : 8u;
        const auto resource = type == 2 ? 0x8bu : 0xbu;
        registers[0] = {program, static_cast<std::uint32_t>(address >> 8u)};
        registers[1] = {program + 1u, static_cast<std::uint32_t>(address >> 40u)};
        registers[2] = {resource, type == 2 ? 8u : 0u};
        const auto vertices = reinterpret_cast<std::uintptr_t>(DrawTriangle.data());
        const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>((vertices >> 32u) & 0xffffu) | (16u << 16u), 3, 0x01016facu};
        for (std::size_t i = 0; i < descriptor.size(); ++i) registers[i + 3] = {static_cast<std::uint32_t>(0x8cu + i), descriptor[i]};
        context = {{{0x1b6, wave == 32 ? 0x8000u : 0u}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, type == 1 ? 9u : 0u}}};
    }
};

void SubmitDraw(const AgcDriver::QueueState& state) {
    std::array<std::uint32_t, 1024> words{};
    CommandBuffer buffer{words.data(), words.data() + words.size(), words.data(), words.data() + words.size(), nullptr, nullptr, 0};
    const auto write = [&](const AgcDriver::Registers& registers, std::uint32_t opcode) {
        for (const auto& [offset, value] : registers) {
            *buffer.cursor_up++ = 0xc0010000u | (opcode << 8u);
            *buffer.cursor_up++ = offset;
            *buffer.cursor_up++ = value;
        }
    };
    write(state.shader, 0x76);
    write(state.context, 0x69);
    write(state.userConfig, 0x79);
    sceAgcDcbDrawIndexAuto(&buffer, 3, 0);
    alignas(64) volatile std::uint32_t completed = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(&completed);
    const std::array<std::uint32_t, 15> completion{0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0, 0,
        0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0xffffffffu, 0x19u};
    std::copy(completion.begin(), completion.end(), buffer.cursor_up);
    buffer.cursor_up += completion.size();
    Packet packet{words.data(), static_cast<std::uint32_t>(buffer.cursor_up - words.data()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "null draw DCB submit failed");
    AgcDriverWaitIdle_nid_postfix();
    const auto start = std::chrono::steady_clock::now();
    while (completed != 1) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "null draw completion timed out");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void GuestDraw(std::uint32_t wave) {
    DrawMemory colors, depths;
    DrawHeader vertex, white, black;
    vertex.Initialize(DrawVertexCode, 2, wave);
    white.Initialize(DrawWhiteCode, 1, wave);
    black.Initialize(DrawBlackCode, 1, wave);
    AgcDriverRegisterShader_nid_postfix(&vertex.shader);
    AgcDriverRegisterShader_nid_postfix(&white.shader);
    AgcDriverRegisterShader_nid_postfix(&black.shader);
    AgcDriver::QueueState state{};
    state.userConfig[0x242] = 4;
    state.userConfig[0x24a] = 0;
    state.userConfig[0x24b] = 0;
    state.context = {{0x2d5, 0x2000u | (wave == 32 ? 0x00400000u : 0u)}, {0x1b6, wave == 32 ? 0x8000u : 0u}, {0x207, 0}, {0x200, 0x26}, {0x203, 0},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0}, {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff}, {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010}, {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028}, {0x31b, 0}, {0x31d, 0},
        {0x3b0, (63u << 14u) | 31u}, {0x3b8, 0x9000000}, {0x1e0, 0}, {0xc, 0}, {0xd, 0x200040}, {0x81, 0x80000000}, {0x82, 0x200040},
        {0x90, 0x80000000}, {0x91, 0x200040}, {0x94, 0x80000000}, {0x95, 0x200040},
        {0x000, 0}, {0x002, 0}, {0x007, 63u | (31u << 16u)}, {0x010, 3}, {0x011, 0}, {0x00b, std::bit_cast<std::uint32_t>(1.0f)}, {0x00a, 0}};
    const auto color = reinterpret_cast<std::uintptr_t>(colors.data);
    const auto depth = reinterpret_cast<std::uintptr_t>(depths.data);
    state.context[0x318] = static_cast<std::uint32_t>(color >> 8u);
    state.context[0x390] = static_cast<std::uint32_t>(color >> 40u);
    for (const auto offset : {0x012u, 0x014u}) state.context[offset] = static_cast<std::uint32_t>(depth >> 8u);
    for (const auto offset : {0x01au, 0x01cu}) state.context[offset] = static_cast<std::uint32_t>(depth >> 40u);
    state.context[0x10f] = std::bit_cast<std::uint32_t>(32.0f);
    state.context[0x110] = std::bit_cast<std::uint32_t>(32.0f);
    state.context[0x111] = std::bit_cast<std::uint32_t>(-16.0f);
    state.context[0x112] = std::bit_cast<std::uint32_t>(16.0f);
    state.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    state.context[0x114] = 0;
    state.context[0xb4] = 0;
    state.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    for (const auto reg : vertex.registers) state.shader[reg.offset] = reg.value;
    const auto bind = [&](const DrawHeader& fragment) {
        for (std::size_t i = 0; i < 3; ++i) state.shader[fragment.registers[i].offset] = fragment.registers[i].value;
        std::vector<ShaderRegister> context;
        for (const auto& [offset, value] : state.context) context.push_back({offset, value});
        const std::array<const Shader*, 2> stages{&vertex.shader, &fragment.shader};
        const std::array<ShaderRegister, 1> primitive{{{0x242, 4}}};
        AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, context, primitive);
    };
    const auto check = [&](std::byte expected) {
        std::array<std::byte, 64u * 32u * 4u> result{};
        AgcDriver::GuestMemory::Read(color, result);
        for (std::size_t i = 0; i < result.size(); ++i) Require(result[i] == expected, ("null draw color readback differs at byte " + std::to_string(i)).c_str());
        for (std::size_t i = result.size(); i < 65536; ++i) Require(colors.data[i] == std::byte{0x40}, "null draw overwrote the color allocation guard");
    };
    bind(white);
    SubmitDraw(state);
    check(std::byte{0x40});
    state.shader[8] = 0;
    state.shader[9] = 0;
    state.shader[0xb] = 8;
    state.context[0x8e] = 0;
    state.context[0x8f] = 0;
    state.context[0x1c5] = 0;
    state.context[0x200] = 0x76;
    SubmitDraw(state);
    SubmitDraw(state);
    check(std::byte{0x40});
    state.context[0x8e] = 15;
    state.context[0x8f] = 15;
    state.context[0x1c5] = 9;
    state.context[0x200] = 0x22;
    bind(white);
    SubmitDraw(state);
    check(std::byte{255});
    for (auto& position : DrawTriangle) position[2] = 0.75f;
    state.context[0x200] = 0x12;
    bind(black);
    SubmitDraw(state);
    check(std::byte{255});
    for (auto& position : DrawTriangle) position[2] = 0.5f;
    std::printf("guest null pixel wave%u: depth0.5 equality witness, depth0.75 rejection, repeated null draw,8192 color bytes and57344 allocation sentinels passed\n", wave);
    AgcDriverShutdown_nid_postfix();
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::string_view mode = argc > 1 ? argv[1] : "--guards";
        const auto wave = mode.find("64") != std::string_view::npos ? 64u : 32u;
        std::printf("null-pixel device %s native subgroup%u\n", device->DeviceName().c_str(), device->Target().subgroupSize);
        if (mode.starts_with("--guest")) {
            if (device->Target().subgroupSize < 32u) { std::puts("skipped, native subgroup below32 cannot execute guest null-pixel graphics"); return VulkanTestSkipped; }
            GuestDraw(wave);
        } else if (mode.starts_with("--wave")) Variants(*device, wave);
        else Guards(*device);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
