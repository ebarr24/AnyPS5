#define main ConstantStoreReferenceEntryPoint
#include "ConstantStore.cpp"
#undef main
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <string_view>

extern "C" int APS5_VABI sceAgcCreateInterpolantMapping(ShaderRegister*, const Shader*, const Shader*);
extern "C" int APS5_VABI sceAgcLinkShaders(ShaderRegister*, ShaderRegister*, const void*, const Shader*, const Shader*, std::uint32_t);

static_assert(sizeof(Shader) == 96 && alignof(Shader) == 8);
static_assert(offsetof(Shader, file_header) == 0 && offsetof(Shader, version) == 4);
static_assert(offsetof(Shader, user_data) == 8 && offsetof(Shader, code) == 16);
static_assert(offsetof(Shader, cx_registers) == 24 && offsetof(Shader, sh_registers) == 32);
static_assert(offsetof(Shader, specials) == 40 && offsetof(Shader, input_semantics) == 48 && offsetof(Shader, output_semantics) == 56);
static_assert(offsetof(Shader, header_size) == 64 && offsetof(Shader, shader_size) == 68);
static_assert(offsetof(Shader, embedded_constant_buffer_size_dqw) == 72 && offsetof(Shader, target) == 76);
static_assert(offsetof(Shader, num_input_semantics) == 80 && offsetof(Shader, scratch_size_dw_per_thread) == 84);
static_assert(offsetof(Shader, num_output_semantics) == 86 && offsetof(Shader, special_sizes_bytes) == 88);
static_assert(offsetof(Shader, type) == 90 && offsetof(Shader, num_cx_registers) == 91 && offsetof(Shader, num_sh_registers) == 92);
static_assert(sizeof(Shader::num_sh_registers) == 1 && sizeof(ShaderRegister) == 8 && sizeof(ShaderSemantic) == 4);

namespace {

struct Header {
    Shader shader{};
    std::array<ShaderRegister, 16> registers{};
    std::array<ShaderRegister, 8> context{};
    ShaderSpecialRegs specials{};
    ShaderUserData users{};
    ShaderSemantic input{};
    ShaderSemantic output{};

    void Initialize(std::uint8_t type, std::span<const std::uint32_t> code, std::size_t entry) {
        shader.file_header = 0x34333231u;
        shader.version = 0x18u;
        shader.code = code.data();
        shader.header_size = sizeof(Header);
        shader.shader_size = code.size_bytes();
        shader.type = type;
        shader.sh_registers = registers.data();
        shader.cx_registers = context.data();
        shader.specials = &specials;
        shader.user_data = &users;
        const auto address = reinterpret_cast<std::uintptr_t>(code.data()) + entry * 4u;
        const auto program = type == 0u ? 0x20cu : type == 1u ? 8u : 0xc8u;
        const auto resource = type == 0u ? 0x213u : type == 1u ? 0xbu : 0x8bu;
        registers[0] = {program, static_cast<std::uint32_t>(address >> 8u)};
        registers[1] = {program + 1u, static_cast<std::uint32_t>(address >> 40u)};
        registers[2] = {resource, 0};
        shader.num_sh_registers = 3;
        specials.dispatch_modifier = 0x8000u;
        specials.vgt_shader_stages_en = {ShaderRegs::VGT_SHADER_STAGES_EN, 0x00402000u};
        specials.vgt_gs_out_prim_type = {ShaderRegs::VGT_GS_OUT_PRIM_TYPE, 2};
        specials.ge_cntl = {ShaderRegs::GE_CNTL, 0};
        specials.ge_user_vgpr_en = {ShaderRegs::GE_USER_VGPR_EN, 0};
        context[0] = {0x1b6, 0x8000u};
        context[1] = {0x1b3, 0};
        context[2] = {0x1b4, 0};
        context[3] = {0x203, 0};
        context[4] = {0x1c5, 9};
        shader.num_cx_registers = 5;
    }
};

struct BoundCopy {
    alignas(Shader) std::array<std::byte, 96> bytes{};
    explicit BoundCopy(const Shader& shader) { std::memcpy(bytes.data(), &shader, 96); }
    Shader* Get() { return reinterpret_cast<Shader*>(bytes.data()); }
};

template<typename TAction> void RejectCopy(TAction action, std::string_view expected) {
    try { action(); }
    catch (const std::exception& error) {
        Require(std::string_view(error.what()).find(expected) != std::string_view::npos, error.what());
        return;
    }
    throw std::runtime_error("mismatched header copy was accepted");
}

template<typename TResolve> void Boundaries(Header& header, TResolve resolve, std::string_view replaced) {
    resolve(&header.shader);
    BoundCopy copy(header.shader);
    resolve(copy.Get());
    copy.Get()->user_data = nullptr;
    resolve(copy.Get());
    copy.Get()->user_data = reinterpret_cast<ShaderUserData*>(0x12345678u);
    resolve(copy.Get());
    for (const auto offset : {93u, 94u, 95u}) {
        for (const auto value : {0x7du, 0x7fu}) {
            copy.bytes[offset] = static_cast<std::byte>(value);
            resolve(copy.Get());
            copy.bytes[offset] = std::byte{};
        }
    }
    for (const auto offset : {76u, 90u, 92u, 24u, 32u, 40u, 48u, 56u}) {
        BoundCopy changed(header.shader);
        changed.bytes[offset] ^= std::byte{1};
        RejectCopy([&] { resolve(changed.Get()); }, replaced);
    }
    BoundCopy unknown(header.shader);
    unknown.Get()->code = reinterpret_cast<const void*>(0x100000u);
    RejectCopy([&] { resolve(unknown.Get()); }, "unregistered shader");
    RejectCopy([&] { resolve(reinterpret_cast<const Shader*>(copy.bytes.data() + 1u)); }, "null or misaligned address");
    RejectCopy([&] { resolve(reinterpret_cast<const Shader*>(0x1000u)); }, "not readable");
    std::printf("header copies accepted fixed fields/user_data/tail93..95; last semantic byte92/target/type/code/metadata/range mismatches rejected\n");
}

void StaticCopies() {
    GuestBlock guest;
    alignas(256) auto code = NarrowConstantStoreFixture::Code;
    Header header;
    header.Initialize(0, code, 0);
    header.registers[2].value = 4;
    header.registers[3] = {0x207, 8};
    header.registers[4] = {0x208, 1};
    header.registers[5] = {0x209, 1};
    header.registers[6] = {0x212, 0};
    const auto address = reinterpret_cast<std::uintptr_t>(guest.Data());
    header.registers[7] = {0x240, static_cast<std::uint32_t>(address)};
    header.registers[8] = {0x241, static_cast<std::uint32_t>(address >> 32u)};
    header.shader.num_sh_registers = 9;
    auto* fixed = reinterpret_cast<std::byte*>(&header.shader);
    fixed[93] = fixed[94] = fixed[95] = std::byte{0x7f};
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    Boundaries(header, [](const Shader* shader) { AgcDriverResolveShaderAbi_nid_postfix(shader, {}, {}); }, "static ABI refers to a replaced shader header");
    BoundCopy bound(header.shader);
    bound.Get()->user_data = nullptr;
    bound.bytes[93] = bound.bytes[94] = bound.bytes[95] = std::byte{};
    const auto frozen = header.registers;
    header.registers[3].value = 0;
    header.specials.dispatch_modifier = 0;
    code[0] = 0xffffffffu;
    AgcDriverResolveShaderAbi_nid_postfix(bound.Get(), {}, {});
    std::vector<std::uint32_t> commands;
    for (std::size_t i = 0; i < 9u; ++i) commands.insert(commands.end(), {0xc0017600u, frozen[i].offset, frozen[i].value});
    commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    sceAgcDriverSubmitAcb(0x20, &packet);
    AgcDriverWaitIdle_nid_postfix();
    for (std::size_t offset = 0; offset < BlockBytes; ++offset) {
        const auto expected = offset < 8u * 16u ? NarrowConstantStoreFixture::ExpectedLane[offset % 16u] : NarrowConstantStoreFixture::Fill;
        Require(guest.Data()[offset] == expected, "bound copy dispatch used live code/metadata or corrupted a sentinel");
    }
    std::puts("static bound copies retained registered immutable code/metadata:8 native lane outputs and65536 bytes including sentinels passed");
}

void GraphicsCopies(std::string_view mode) {
    alignas(256) std::array<std::uint32_t, 72> vertexCode{};
    alignas(256) std::array<std::uint32_t, 77> pixelCode{};
    vertexCode.fill(0xffffffffu);
    pixelCode.fill(0xffffffffu);
    const std::array<std::uint32_t, 8> vertex{0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e0602ffu, 0x3f800000u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
    const std::array<std::uint32_t, 13> pixel{0x7e0002ffu, 0x3e800000u, 0x7e0202ffu, 0x3f000000u, 0x7e0402ffu, 0x3f400000u, 0x7e0602ffu, 0x3f800000u, 0xf800100fu, 0x03020100u, 0xf800183fu, 0x03020100u, 0xbf810000u};
    std::copy(vertex.begin(), vertex.end(), vertexCode.begin() + 64u);
    std::copy(pixel.begin(), pixel.end(), pixelCode.begin() + 64u);
    Header front, fragment;
    front.Initialize(2, vertexCode, 64);
    fragment.Initialize(1, pixelCode, 64);
    front.output.semantic = 9;
    front.output.hardware_mapping = 5;
    front.shader.output_semantics = &front.output;
    front.shader.num_output_semantics = 1;
    fragment.input.semantic = 9;
    fragment.input.is_flat_shaded = 1;
    fragment.shader.input_semantics = &fragment.input;
    fragment.shader.num_input_semantics = 1;
    AgcDriverRegisterShader_nid_postfix(&front.shader);
    AgcDriverRegisterShader_nid_postfix(&fragment.shader);
    const std::array<ShaderRegister, 1> primitive{{{0x242, 4}}};
    if (mode == "--graphics") {
        Boundaries(front, [&](const Shader* shader) {
            const std::array<const Shader*, 2> stages{shader, &fragment.shader};
            AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, {}, primitive);
        }, "graphics ABI refers to a replaced shader header");
    } else if (mode == "--rectangle") {
        AgcDriverResolveShaderAbi_nid_postfix(&front.shader, {}, primitive);
        AgcDriverResolveShaderAbi_nid_postfix(&fragment.shader, {}, {});
        Boundaries(front, [&](const Shader* shader) { AgcDriverResolveGraphicsAbi_nid_postfix(shader, &fragment.shader, 7); }, "rectangle ABI refers to a replaced shader header");
        Boundaries(fragment, [&](const Shader* shader) { AgcDriverResolveGraphicsAbi_nid_postfix(&front.shader, shader, 7); }, "rectangle ABI refers to a replaced shader header");
    } else {
        BoundCopy boundFront(front.shader), boundPixel(fragment.shader);
        boundFront.Get()->user_data = boundPixel.Get()->user_data = nullptr;
        boundFront.bytes[93] = std::byte{0x7f};
        boundPixel.bytes[95] = std::byte{0x7d};
        std::array<ShaderRegister, 33> interpolants{};
        interpolants.back() = {0xdeadbeef, 0xcafebabe};
        Require(sceAgcCreateInterpolantMapping(interpolants.data(), boundFront.Get(), boundPixel.Get()) == 0, "exported interpolant mapping rejected public copies");
        Require(interpolants[0].offset == 0x191u && interpolants[0].value == 0x405u && interpolants.back().value == 0xcafebabeu, "public copy mapping or guard differs");
        std::array<ShaderRegister, 35> context{};
        std::array<ShaderRegister, 4> primitiveOutput{};
        context.back() = primitiveOutput.back() = {0xdeadbeef, 0xcafebabe};
        Require(sceAgcLinkShaders(context.data(), primitiveOutput.data(), nullptr, boundFront.Get(), boundPixel.Get(), 7) == 0, "exported shader linking rejected public copies");
        Require(context[2].value == 0x405u && primitiveOutput[2].value == 7u && context.back().value == 0xcafebabeu && primitiveOutput.back().value == 0xcafebabeu, "public guest link output or sentinel differs");
        const auto savedContext = context;
        const auto savedPrimitive = primitiveOutput;
        boundFront.Get()->target ^= 1u;
        RejectCopy([&] { sceAgcLinkShaders(context.data(), primitiveOutput.data(), nullptr, boundFront.Get(), boundPixel.Get(), 7); }, "replaced shader header");
        Require(std::memcmp(context.data(), savedContext.data(), sizeof(context)) == 0 && std::memcmp(primitiveOutput.data(), savedPrimitive.data(), sizeof(primitiveOutput)) == 0, "failed bound-copy preparation published guest outputs");
        std::puts("exported guest interpolant/link boundary:semantic9 maps to5|flat, primitive7 and transactional output sentinels passed");
    }
    std::printf("%s bound-copy registered graphics/rectangle artifact preparation passed\n", mode.data());
}

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 1 ? "--static" : argv[1];
        Require(argc <= 2 && (mode == "--static" || mode == "--graphics" || mode == "--rectangle" || mode == "--guest" || mode == "--layout"), "invalid bound-copy mode");
        if (mode == "--layout") { std::puts("Shader96-byte ABI:semantic end93, tail93..95; all independently pinned field offsets and sizes passed"); return 0; }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::printf("bound-copy device %s, native subgroup%u\n", device->DeviceName().c_str(), device->Target().subgroupSize);
        if (mode == "--static") StaticCopies();
        else GraphicsCopies(mode);
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (const std::exception& shutdownError) { std::cerr << shutdownError.what() << '\n'; }
        return 1;
    }
}
