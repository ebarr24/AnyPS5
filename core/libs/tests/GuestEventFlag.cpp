#include "SceTypes.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag*, const char*, std::uint32_t, std::uint64_t, const void*);
int APS5_VABI sceKernelDeleteEventFlag(KernelEventFlag);
int APS5_VABI sceKernelSetEventFlag(KernelEventFlag, std::uint64_t);
int APS5_VABI sceKernelCancelEventFlag(KernelEventFlag, std::uint64_t, int*);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag, std::uint64_t, std::uint32_t, std::uint64_t*, KernelUseconds*);
}

namespace {
constexpr int Ok = 0;
constexpr int Invalid = static_cast<int>(0x80020016);
constexpr int TimedOut = static_cast<int>(0x8002003c);
constexpr int Canceled = static_cast<int>(0x80020055);
constexpr std::uint32_t WaitOr = 0x02;
constexpr std::uint32_t ClearAll = 0x10;

void Require(bool value) {
    if (!value) std::abort();
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

void SetTraceMode(const char* mode) {
#ifdef _WIN32
    _putenv_s("APS5_TRACE_PHASES", (std::strcmp(mode, "on") == 0 || std::strcmp(mode, "invalid") == 0 || std::strcmp(mode, "posix-cancel") == 0) ? "1" : (std::strcmp(mode, "zero") == 0 ? "0" : ""));
#else
    if (std::strcmp(mode, "on") == 0 || std::strcmp(mode, "invalid") == 0 || std::strcmp(mode, "posix-cancel") == 0) setenv("APS5_TRACE_PHASES", "1", 1);
    else if (std::strcmp(mode, "zero") == 0) setenv("APS5_TRACE_PHASES", "0", 1);
    else unsetenv("APS5_TRACE_PHASES");
#endif
}

unsigned Count(const std::string& text, const char* needle) {
    unsigned count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += std::strlen(needle)) ++count;
    return count;
}

struct GuestWaitContext {
    KernelEventFlag flag = nullptr;
    int result = 0;
};

void* APS5_VABI WaitForGuestCancellation(void* raw) {
    auto& context = *static_cast<GuestWaitContext*>(raw);
    context.result = sceKernelWaitEventFlag(context.flag, 1, WaitOr, nullptr, nullptr);
    return nullptr;
}

void ExerciseGuestCancellation(KernelEventFlag flag) {
    GuestWaitContext context{flag};
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, WaitForGuestCancellation, &context, nullptr) == Ok);
    int lastWaiters = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    bool canceled = false;
    while (std::chrono::steady_clock::now() < deadline) {
        int waiters = 0;
        Require(sceKernelCancelEventFlag(flag, 0, &waiters) == Ok);
        lastWaiters = waiters;
        if (waiters > 0) {
            canceled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!canceled) {
        std::fprintf(stderr, "GuestEventFlag: no event-flag waiter registered within 1000 ms; last_waiters=%d\n", lastWaiters);
        std::fflush(stderr);
        std::abort();
    }
    Require(scePthreadJoin(thread, nullptr) == Ok);
    Require(context.result == Canceled);
}

#if defined(__linux__)
struct HostCancelContext {
    KernelEventFlag flag = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
};

void* HostWaitUntilCanceled(void* raw) {
    auto& context = *static_cast<HostCancelContext*>(raw);
    context.ready.store(true, std::memory_order_release);
    while (!context.release.load(std::memory_order_acquire)) ::sched_yield();
    sceKernelWaitEventFlag(context.flag, 1, WaitOr, nullptr, nullptr);
    return nullptr;
}

void RunHostCancellation(KernelEventFlag flag, StderrCapture& capture) {
    HostCancelContext context{flag};
    pthread_t thread{};
    Require(pthread_create(&thread, nullptr, HostWaitUntilCanceled, &context) == 0);
    while (!context.ready.load(std::memory_order_acquire)) ::sched_yield();
    Require(pthread_cancel(thread) == 0);
    context.release.store(true, std::memory_order_release);
    void* result = nullptr;
    Require(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
    const std::string output = capture.Read();
    Require(Count(output, "unit=event_flag_wait edge=api_entry") == 1);
    Require(Count(output, "unit=event_flag_wait edge=scope_exit") + Count(output, "unit=event_flag_wait edge=unwind") == 1);
}
#endif

void Run(const char* mode) {
    SetTraceMode(mode);
    StderrCapture capture;
    if (std::strcmp(mode, "invalid") == 0) {
        KernelEventFlag flag = nullptr;
        Require(sceKernelCreateEventFlag(&flag, "phase-invalid", 0, 0, nullptr) == Ok);
        Require(sceKernelWaitEventFlag(flag, 0, WaitOr, nullptr, nullptr) == Invalid);
        Require(sceKernelDeleteEventFlag(flag) == Ok);
        Require(capture.Read().empty());
        return;
    }
#if defined(__linux__)
    if (std::strcmp(mode, "posix-cancel") == 0) {
        KernelEventFlag flag = nullptr;
        Require(sceKernelCreateEventFlag(&flag, "phase-host-cancel", 0, 0, nullptr) == Ok);
        RunHostCancellation(flag, capture);
        // Host pthread cancellation unwinds the wait before its waiter count can be decremented;
        // keep this one fixture object until process teardown rather than claim normal deletion.
        return;
    }
#endif

    KernelEventFlag flag = nullptr;
    Require(sceKernelCreateEventFlag(&flag, "phase-event-flag", 0, 1, nullptr) == Ok);
    std::uint64_t pattern = 0;
    errno = EDOM;
    Require(sceKernelWaitEventFlag(flag, 1, WaitOr | ClearAll, &pattern, nullptr) == Ok && pattern == 1);
    Require(errno == EDOM);
    // The guest wait's Windows synchronization path may update LastError even
    // with tracing disabled; LastError preservation is covered at the callback
    // and semaphore observer boundaries where the API contract is observable.
    Require(sceKernelDeleteEventFlag(flag) == Ok);

    KernelEventFlag canceledFlag = nullptr;
    Require(sceKernelCreateEventFlag(&canceledFlag, "phase-event-cancel", 0, 0, nullptr) == Ok);
    ExerciseGuestCancellation(canceledFlag);
    Require(sceKernelDeleteEventFlag(canceledFlag) == Ok);

    KernelEventFlag timeoutFlag = nullptr;
    Require(sceKernelCreateEventFlag(&timeoutFlag, "phase-event-timeout", 0, 0, nullptr) == Ok);
    for (unsigned i = 0; i < 12; ++i) {
        KernelUseconds timeout = 0;
        Require(sceKernelWaitEventFlag(timeoutFlag, 1, WaitOr, nullptr, &timeout) == TimedOut);
    }
    Require(sceKernelDeleteEventFlag(timeoutFlag) == Ok);
    if (std::strcmp(mode, "on") == 0) {
        const std::string output = capture.Read();
        Require(Count(output, "unit=event_flag_wait edge=api_entry") == 12);
        Require(Count(output, "unit=event_flag_wait edge=scope_exit") == 12);
        Require(Count(output, "unit=event_flag_wait edge=truncated") == 1);
        Require(Count(output, "unit=event_flag_wait edge=unwind") == 0);
    } else {
        Require(capture.Read().find("[phase]") == std::string::npos);
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* mode = argv[1];
    if (std::strcmp(mode, "off") != 0 && std::strcmp(mode, "zero") != 0 && std::strcmp(mode, "on") != 0 && std::strcmp(mode, "invalid") != 0 && std::strcmp(mode, "posix-cancel") != 0) return 2;
    Run(mode);
    std::puts("GuestEventFlag: wait, guest cancellation, default silence, cap, and host-cancel checks passed");
    return 0;
}
