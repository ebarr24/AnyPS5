#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using namespace ShaderRecompiler;

constexpr std::uint32_t Width = 8;
constexpr std::uint32_t Layers = 4;
constexpr std::uint32_t Sentinel = 0xdeadbeef;
alignas(4096) std::array<std::byte, 16384> Texels{};
alignas(256) std::array<std::uint32_t, 20> Output{};
alignas(256) std::array<std::uint32_t, 68> PhiOutput{};
alignas(256) std::array<std::byte, 64 * 16 * 4> Pixels{};

struct alignas(4096) Table {
    std::array<std::array<std::uint32_t, 8>, 2> heap{};
    std::array<std::uint32_t, 16> srt{};
};

Table Bindless{};

class RegisteredImages {
public:
    RegisteredImages() { GuestAllocations::Mutation().Add(Texels.data(), Texels.size(), true, true); }
    ~RegisteredImages() { GuestAllocations::Mutation().Remove(Texels.data()); }
    RegisteredImages(const RegisteredImages&) = delete;
    RegisteredImages& operator=(const RegisteredImages&) = delete;
};

std::array<std::uint32_t, 4> Buffer(const void* data, std::uint32_t stride, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, stride == 0u ? 0x31016facu : 0x01016facu};
}

std::array<std::uint32_t, 8> Texture(bool array) {
    const auto address = reinterpret_cast<std::uintptr_t>(Texels.data());
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (56u << 20u) | (3u << 30u),
            (Width - 1u) >> 2u | ((Width - 1u) << 14u), 0xfacu | ((array ? 13u : 9u) << 28u), array ? Layers - 1u : 0u, 0u, 0u, 0u};
}

std::array<std::uint8_t, 4> Value(std::uint32_t index) {
    return {static_cast<std::uint8_t>(17u + index * 53u), static_cast<std::uint8_t>(31u + index * 37u), static_cast<std::uint8_t>(47u + index * 29u), 255u};
}

void Fill(bool array) {
    Texels.fill(std::byte{0});
    const auto surface = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(Texture(array)));
    const auto& mip = surface.mips.at(0);
    for (std::uint32_t layer = 0; layer < (array ? Layers : 1u); ++layer) {
        for (std::uint32_t y = 0; y < Width; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto value = Value(array ? layer : y % Layers);
                const auto offset = surface.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * 4u;
                Require(offset + value.size() <= Texels.size(), "loop image surface exceeds synthetic storage");
                for (std::uint32_t component = 0; component < 4; ++component) Texels[offset + component] = std::byte{value[component]};
            }
        }
    }
}

void Vop1(std::vector<std::uint32_t>& code, std::uint32_t opcode, std::uint32_t destination, std::uint32_t source) {
    code.push_back(0x7e000000u | (destination << 17u) | (opcode << 9u) | source);
}

void Literal(std::vector<std::uint32_t>& code, std::uint32_t destination, float value) {
    Vop1(code, 1u, destination, 0xffu);
    code.push_back(std::bit_cast<std::uint32_t>(value));
}

std::size_t BeginLoop(std::vector<std::uint32_t>& code, bool header) {
    const auto begin = code.size();
    if (header) code.insert(code.end(), {0xbf0a8428, 0xbf840000});
    return begin;
}

void EndLoop(std::vector<std::uint32_t>& code, std::size_t begin, bool header) {
    code.push_back(0x80288128);
    if (!header) code.push_back(0xbf0a8428);
    const auto offset = static_cast<std::int32_t>(begin) - static_cast<std::int32_t>(code.size() + 1u);
    code.push_back((header ? 0xbf820000u : 0xbf850000u) | (static_cast<std::uint32_t>(offset) & 0xffffu));
    if (header) code.at(begin + 1u) |= static_cast<std::uint32_t>(code.size() - begin - 2u);
}

std::vector<std::uint32_t> Code(bool table, bool fragment, bool loop, bool array = true, bool header = false) {
    std::vector<std::uint32_t> code;
    if (table) {
        code = {0xf4080100, 0xfa000000, 0xf4080200, 0xfa000010, 0xf4080300, 0xfa000020,
                0xf4080700, 0xfa000030, 0x7e200500, 0x8f108510, 0xf42c0502, 0x20000000};
    } else {
        for (std::uint32_t reg = 1; reg <= 6; ++reg) Vop1(code, 1u, reg, 0x80u);
    }
    code.push_back(0xbea80380);
    const auto begin = BeginLoop(code, header);
    if (table) {
        Literal(code, 0u, 0.0625f);
        Vop1(code, 6u, 1u, 40u);
        Literal(code, 6u, 0.125f);
        code.push_back(0x10020d01u);
        code.insert(code.end(), {0xf09c0f08, 0x00450800});
    } else if (array) {
        Vop1(code, 6u, 3u, 40u);
        code.insert(code.end(), {0xf09c0f28, 0x00400801});
    } else {
        Literal(code, 1u, 0.0625f);
        Vop1(code, 6u, 2u, 40u);
        Literal(code, 6u, 0.125f);
        code.push_back(0x10040d02);
        code.insert(code.end(), {0xf0800f08, 0x00400801});
    }
    code.push_back(0xbf8c3f70);
    if (!fragment) {
        Vop1(code, 1u, 7u, 40u);
        code.push_back(0x340e0e84);
        code.insert(code.end(), {0xe0781000, table ? 0x80070807u : 0x80030807u});
    }
    if (loop) EndLoop(code, begin, header);
    if (fragment) code.insert(code.end(), {0xf800180f, 0x0b0a0908});
    code.push_back(0xbf810000);
    return code;
}

std::vector<std::uint32_t> UserData(bool table) {
    constexpr std::array<std::uint32_t, 4> sampler{2u | (2u << 3u) | (2u << 6u), 0u, 0u, 0u};
    const auto texture = Texture(!table);
    const auto output = Buffer(Output.data(), 0u, sizeof(Output));
    if (table) {
        Bindless.heap = {};
        Bindless.heap[0] = texture;
        Bindless.srt = {};
        const auto heap = Buffer(Bindless.heap.data(), 32u, 2u);
        std::copy(heap.begin(), heap.end(), Bindless.srt.begin());
        std::copy(sampler.begin(), sampler.end(), Bindless.srt.begin() + 4);
        std::copy(output.begin(), output.end(), Bindless.srt.begin() + 12);
        const auto address = reinterpret_cast<std::uintptr_t>(Bindless.srt.data());
        return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    }
    std::vector<std::uint32_t> data(16u);
    std::copy(texture.begin(), texture.end(), data.begin());
    std::copy(sampler.begin(), sampler.end(), data.begin() + 8);
    std::copy(output.begin(), output.end(), data.begin() + 12);
    return data;
}

std::vector<MemoryRegion> Memory(std::span<const std::uint32_t> code) {
    return {{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(&Bindless), std::as_bytes(std::span(&Bindless, 1))}};
}

void Compute(AgcDriver::VulkanDevice& device, std::uint32_t wave, bool table, bool loop, bool header = false) {
    Fill(!table);
    Output.fill(Sentinel);
    const auto code = Code(table, false, loop, true, header);
    const auto user = UserData(table);
    const auto memory = Memory(code);
    const ShaderComputeStageInfo compute{{1u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {wave, 0u, user, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(wave), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    const auto result = Recompile(request);
    device.Dispatch(result, 1u, 1u, 1u);
    device.WaitIdle();
    const auto iterations = loop ? Layers : 1u;
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        const auto expected = Value(iteration);
        for (std::uint32_t component = 0; component < 4; ++component) {
            const auto actual = std::bit_cast<float>(Output[iteration * 4u + component]);
            Require(std::fabs(actual * 255.0f - expected[component]) < 1e-3f,
                    "loop image compute value: wave" + std::to_string(wave) + " iteration " + std::to_string(iteration) + " component " + std::to_string(component) + " actual " + std::to_string(actual));
        }
    }
    for (std::uint32_t word = iterations * 4u; word < Output.size(); ++word) Require(Output[word] == Sentinel, "loop image output sentinel overwritten");
    std::printf("compute wave%u %s %s: %u enabled samples, values and sentinels passed\n", wave, table ? "bindless2D" : "direct2D-array", header ? "unconditional-continue" : loop ? "conditional-continue" : "outside-loop", iterations);
}

void Write(AgcDriver::VulkanDevice& device, std::uint32_t wave, bool loop, bool header = false) {
    Texels.fill(std::byte{0});
    auto texture = Texture(true);
    texture[1] = (texture[1] & ~0x3ff00000u) | (22u << 20u);
    std::vector<std::uint32_t> code{0xbea80380, 0x7e020280, 0x7e040280};
    const auto begin = BeginLoop(code, header);
    code.insert(code.end(), {0x7e060228, 0x7e100c28, 0x061010f2, 0xf0200128, 0x00000801});
    if (loop) EndLoop(code, begin, header);
    code.push_back(0xbf810000);
    const auto memory = Memory(code);
    const ShaderComputeStageInfo compute{{1u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {wave, 0u, texture, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(wave), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    const auto result = Recompile(request);
    device.Dispatch(result, 1u, 1u, 1u);
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(Texels.data()), Texels.size(), nullptr, "loop-image-continue-test");
    device.WaitIdle();
    const auto surface = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    const auto& mip = surface.mips.at(0);
    for (std::uint32_t layer = 0; layer < Layers; ++layer) {
        for (std::uint32_t y = 0; y < Width; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto offset = surface.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * sizeof(float);
                Require(offset + sizeof(float) <= Texels.size(), "loop image write exceeds synthetic storage");
                float actual;
                std::memcpy(&actual, Texels.data() + offset, sizeof(actual));
                const float expected = x == 0u && y == 0u && (loop || layer == 0u) ? static_cast<float>(layer + 1u) : 0.0f;
                Require(actual == expected, "loop image no-value write or untouched texel failed at layer " + std::to_string(layer) + " x " + std::to_string(x) + " y " + std::to_string(y) + " actual " + std::to_string(actual) + " expected " + std::to_string(expected));
            }
        }
    }
    std::printf("compute wave%u direct2D-array no-value write %s: %u enabled writes and untouched texels passed\n", wave, header ? "unconditional-continue" : loop ? "conditional-continue" : "outside-loop", loop ? Layers : 1u);
}

void Phi(AgcDriver::VulkanDevice& device, std::uint32_t wave, bool header = false) {
    constexpr std::uint32_t threads = 64;
    Fill(true);
    PhiOutput.fill(Sentinel);
    std::vector<std::uint32_t> code;
    for (std::uint32_t reg = 1; reg <= 6; ++reg) Vop1(code, 1u, reg, 0x80u);
    code.push_back(0xbea80380);
    Vop1(code, 6u, 12u, 0x100u);
    Vop1(code, 1u, 7u, 0x100u);
    code.push_back(0x340e0e82);
    const auto begin = BeginLoop(code, header);
    Vop1(code, 6u, 3u, 40u);
    code.insert(code.end(), {0xf09c0f28, 0x00400801, 0xbf8c3f70, 0x0618110c, 0xe0701000, 0x80030c07});
    EndLoop(code, begin, header);
    code.push_back(0xbf810000);
    auto user = UserData(false);
    const auto output = Buffer(PhiOutput.data(), 0u, sizeof(PhiOutput));
    std::copy(output.begin(), output.end(), user.begin() + 12);
    const auto memory = Memory(code);
    const ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {wave, 0u, user, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(wave), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    const auto result = Recompile(request);
    device.Dispatch(result, 1u, 1u, 1u);
    device.WaitIdle();
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        float expected = static_cast<float>(lane);
        for (std::uint32_t iteration = 0; iteration < Layers; ++iteration) expected += static_cast<float>(Value(iteration)[0]) / 255.0f;
        const auto actual = std::bit_cast<float>(PhiOutput[lane]);
        Require(std::fabs(actual - expected) < 1e-4f, "loop image Phi value: wave" + std::to_string(wave) + " lane " + std::to_string(lane) + " actual " + std::to_string(actual));
    }
    for (std::uint32_t word = threads; word < PhiOutput.size(); ++word) Require(PhiOutput[word] == Sentinel, "loop image Phi output sentinel overwritten");
    std::printf("compute wave%u direct2D-array %s loop-carried Phi: %u distinct lane values, four iterations and sentinels passed\n", wave, header ? "unconditional" : "conditional", threads);
}

void RunCase(AgcDriver::VulkanDevice& device, std::string_view selected) {
    for (const auto wave : {32u, 64u}) {
        for (const std::string_view flow : {"outside", "loop", "header"}) {
            const bool loop = flow != "outside", header = flow == "header";
            const auto suffix = "-" + std::to_string(wave) + "-" + std::string(flow);
            if (loop && selected == "compute-phi" + suffix) {
                Phi(device, wave, header);
                return;
            }
            for (const auto table : {false, true}) {
                if (selected == std::string(table ? "compute-bindless" : "compute-direct") + suffix) {
                    Compute(device, wave, table, loop, header);
                    return;
                }
            }
            if (selected == "compute-write" + suffix) {
                Write(device, wave, loop, header);
                return;
            }
        }
    }
    throw std::runtime_error("unknown loop image synthetic case");
}

void Fragment(AgcDriver::VulkanDevice& device, std::uint32_t wave, bool loop, bool array, bool header = false) {
    constexpr std::uint32_t width = 64, height = 16;
    constexpr std::array<std::array<float, 4>, 3> vertices{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
    constexpr std::array<std::uint32_t, 6> vertexCode{0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000};
    Fill(array);
    Pixels.fill(std::byte{0});
    const auto vertexUser = Buffer(vertices.data(), 16u, vertices.size());
    const std::array vertexMemory{MemoryRegion{reinterpret_cast<std::uintptr_t>(vertexCode.data()), std::as_bytes(std::span(vertexCode))}};
    RecompileRequest vertex{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0u, {}},
        {wave, 0u, vertexUser, std::nullopt, std::nullopt, ShaderVertexStageInfo{}, vertexMemory}, device.Target(), {0u, 0u, 0u, 64u}};
    vertex.useCache = false;
    const auto vertexResult = Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderPixelStageInfo pixel{};
    pixel.wave32 = wave == 32u;
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping.fill(0xe4u);
    const auto code = Code(false, true, loop, array, header);
    auto user = UserData(false);
    const auto texture = Texture(array);
    std::copy(texture.begin(), texture.end(), user.begin());
    const auto memory = Memory(code);
    RecompileRequest fragment{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {wave, 0u, user, std::nullopt, pixel, std::nullopt, memory}, device.Target(), {0u, 0u, vertexPush, 128u - vertexPush}};
    fragment.useCache = false;
    const auto pixelResult = Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertexResult, 0u}, {ShaderStage::Fragment, &pixelResult, vertexPush}}};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, wave, wave, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {width, height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {width, height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(height), static_cast<float>(width), -static_cast<float>(height), 0, 1};
    state.scissor = {{0, 0}, {width, height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    device.Draw(state, {0u, 3u, 0u, 1u, 0u, false}, shaders);
    device.WaitIdle();
    const auto expected = Value(loop ? Layers - 1u : 0u);
    for (std::uint32_t pixelIndex = 0; pixelIndex < width * height; ++pixelIndex) {
        for (std::uint32_t component = 0; component < 4; ++component) Require(Pixels[pixelIndex * 4u + component] == std::byte{expected[component]}, "fragment loop image exported wrong color");
    }
    std::printf("fragment wave%u %s %s: 1024 pixels and RGBA exports passed\n", wave, array ? "direct2D-array explicit-LZ" : "direct2D implicit", loop ? "4-iteration continue" : "outside-loop");
}

}

int main(int argc, char** argv) {
    try {
        RegisteredImages images;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::printf("loop image device %s, native subgroup %u\n", device->DeviceName().c_str(), device->Target().subgroupSize);
        if (argc == 3 && std::string_view(argv[1]) == "--case") {
            RunCase(*device, argv[2]);
            return 0;
        }
        const bool header = argc == 2 && (std::string_view(argv[1]) == "--header" || std::string_view(argv[1]) == "--fragment-header");
        const bool fragment = argc == 2 && (std::string_view(argv[1]) == "--fragment" || std::string_view(argv[1]) == "--fragment-header");
        if (argc != 1 && !fragment && !header) throw std::runtime_error("select --fragment, --header, --fragment-header or --case <public synthetic case>");
        if (fragment && device->Target().subgroupSize < 32u) {
            std::printf("skipped fragment wave32/wave64, native subgroup %u cannot hold a wave32\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        for (const auto wave : {32u, 64u}) {
            for (const auto loop : {false, true}) {
                if (header && !loop) continue;
                if (fragment) for (const auto array : {true, false}) Fragment(*device, wave, loop, array, header);
                else {
                    for (const auto table : {false, true}) Compute(*device, wave, table, loop, header);
                    Write(*device, wave, loop, header);
                }
            }
            if (!fragment) Phi(*device, wave, header);
        }
        std::puts("loop image continue tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
