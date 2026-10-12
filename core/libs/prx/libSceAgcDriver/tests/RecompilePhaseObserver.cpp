#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace ShaderRecompiler;

void RequireImpl(bool value, int line) {
    if (!value) {
        std::fprintf(stdout, "RecompilePhaseObserver: requirement failed at line %d\n", line);
        std::fflush(stdout);
        std::abort();
    }
}
#define Require(value) RequireImpl((value), __LINE__)

void RequireCaptured(bool value, int line, const std::string& output) {
    if (!value) {
        std::fprintf(stdout, "RecompilePhaseObserver: captured-output assertion failed at line %d\n", line);
        if (!output.empty()) std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);
        std::abort();
    }
}

class StderrCapture {
public:
    StderrCapture() : file_(std::tmpfile()) {
        Require(file_ != nullptr);
#ifdef _WIN32
        previous_ = GetStdHandle(STD_ERROR_HANDLE);
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_)));
        Require(handle != INVALID_HANDLE_VALUE && SetStdHandle(STD_ERROR_HANDLE, handle));
#else
        previous_ = ::dup(STDERR_FILENO);
        Require(previous_ >= 0 && ::dup2(::fileno(file_), STDERR_FILENO) >= 0);
#endif
    }
    ~StderrCapture() {
#ifdef _WIN32
        SetStdHandle(STD_ERROR_HANDLE, previous_);
#else
        ::dup2(previous_, STDERR_FILENO);
        ::close(previous_);
#endif
        std::fclose(file_);
    }
    std::string Read() {
        std::fflush(file_);
        Require(std::fseek(file_, 0, SEEK_END) == 0);
        const long size = std::ftell(file_);
        Require(size >= 0 && std::fseek(file_, 0, SEEK_SET) == 0);
        std::string text(static_cast<std::size_t>(size), '\0');
        Require(text.empty() || std::fread(text.data(), 1, text.size(), file_) == text.size());
        return text;
    }
private:
    std::FILE* file_;
#ifdef _WIN32
    HANDLE previous_ = INVALID_HANDLE_VALUE;
#else
    int previous_ = -1;
#endif
};

void SetMode(const char* mode) {
#ifdef _WIN32
    _putenv_s("APS5_TRACE_PHASES", std::strcmp(mode, "on") == 0 ? "1" : (std::strcmp(mode, "zero") == 0 ? "0" : ""));
    _putenv_s("ANYPS5_NO_SHADER_CACHE", "1");
#else
    if (std::strcmp(mode, "on") == 0) setenv("APS5_TRACE_PHASES", "1", 1);
    else if (std::strcmp(mode, "zero") == 0) setenv("APS5_TRACE_PHASES", "0", 1);
    else unsetenv("APS5_TRACE_PHASES");
    setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
#endif
}

unsigned Count(const std::string& text, const char* needle) {
    unsigned count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += std::strlen(needle)) ++count;
    return count;
}

RecompileRequest Request() {
    static constexpr std::array<std::uint32_t, 4> code{0x7e021500u, 0xe0700000u, 0x80000100u, 0xbf810000u};
    static constexpr std::array<std::uint32_t, 5> capabilities{
        spv::CapabilityShader,
        spv::CapabilityGroupNonUniform,
        spv::CapabilityFloat16,
        spv::CapabilityRoundingModeRTE,
        spv::CapabilityDenormPreserve,
    };
    static constexpr std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    static constexpr std::array<std::uint32_t, 4> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};

    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00403000u;
    request.target.spirvVersion = 0x00010600u;
    request.target.subgroupSize = 64;
    request.target.bdaAbiVersion = 1;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 49152;
    request.layout = {0, 0, 0, 128};
    request.useCache = true;
    return request;
}

void Run(const char* mode) {
    SetMode(mode);
    StderrCapture capture;
    auto request = Request();
    const auto cold = Recompile(request);
    const auto warm = Recompile(request);
    Require(!cold.cacheHit);
    Require(warm.cacheHit);
    Require(!cold.spirv.empty() && cold.spirv == warm.spirv);

    auto uncachedRequest = request;
    uncachedRequest.useCache = false;
    const auto uncached = Recompile(uncachedRequest);
    Require(!uncached.cacheHit && uncached.spirv == cold.spirv);

    // Repeat the valid cache-enabled request so the real source-plan and variant
    // lookup scopes execute; useCache=false bypasses both instrumented phases.
    // Keep the identical functional request sequence across on/off/zero processes.
    for (unsigned i = 0; i < 16; ++i) {
        const auto variant = Recompile(request);
        Require(variant.cacheHit && variant.spirv == cold.spirv);
    }

    const std::string output = capture.Read();
    if (std::strcmp(mode, "on") != 0) {
        RequireCaptured(output.find("[phase]") == std::string::npos, __LINE__, output);
        return;
    }
    // The recompiler TU emits at most 64 records: 16 cached requests each produce
    // entry/exit records for both scopes, then exactly one truncation marker.
    std::fprintf(stdout, "RecompilePhaseObserver counts: source-entry=%u source-exit=%u variant-entry=%u variant-exit=%u truncated=%u\n",
                 Count(output, "unit=recompiler phase=source_plan_lookup_prepare edge=scope_entry"),
                 Count(output, "unit=recompiler phase=source_plan_lookup_prepare edge=scope_exit"),
                 Count(output, "unit=recompiler phase=variant_lookup_disk_load_or_compile edge=scope_entry"),
                 Count(output, "unit=recompiler phase=variant_lookup_disk_load_or_compile edge=scope_exit"),
                 Count(output, "unit=recompiler edge=truncated"));
    std::fflush(stdout);
    RequireCaptured(Count(output, "unit=recompiler phase=source_plan_lookup_prepare edge=scope_entry") == 16, __LINE__, output);
    RequireCaptured(Count(output, "unit=recompiler phase=source_plan_lookup_prepare edge=scope_exit") == 16, __LINE__, output);
    RequireCaptured(Count(output, "unit=recompiler phase=variant_lookup_disk_load_or_compile edge=scope_entry") == 16, __LINE__, output);
    RequireCaptured(Count(output, "unit=recompiler phase=variant_lookup_disk_load_or_compile edge=scope_exit") == 16, __LINE__, output);
    RequireCaptured(Count(output, "unit=recompiler edge=truncated") == 1, __LINE__, output);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* mode = argv[1];
    if (std::strcmp(mode, "off") != 0 && std::strcmp(mode, "zero") != 0 && std::strcmp(mode, "on") != 0) return 2;
    Run(mode);
    std::puts("RecompilePhaseObserver: direct cold/warm request and phase checks passed");
    return 0;
}
