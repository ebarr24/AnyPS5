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
#else
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
void APS5_VABI scePthreadExit(void*);
int APS5_VABI scePthreadCancel(Pthread);
void APS5_VABI scePthreadTestcancel();
int APS5_VABI sceKernelCreateSema(KernelSema*, const char*, std::uint32_t, int, int, void*);
int APS5_VABI sceKernelDeleteSema(KernelSema);
int APS5_VABI sceKernelSignalSema(KernelSema, int);
int APS5_VABI sceKernelWaitSema(KernelSema, int, KernelUseconds*);
int APS5_VABI sceKernelCancelSema(KernelSema, int, int*);
}

namespace {
constexpr int Ok = 0;
constexpr int TimedOut = static_cast<int>(0x8002003c);
constexpr int Canceled = static_cast<int>(0x80020055);
void* const GuestCanceled = reinterpret_cast<void*>(std::uintptr_t{1});
std::atomic<unsigned> returnInvocations{0};
std::atomic<unsigned> exitInvocations{0};

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
    _putenv_s("APS5_TRACE_PHASES", std::strcmp(mode, "on") == 0 ? "1" : (std::strcmp(mode, "zero") == 0 ? "0" : ""));
#else
    if (std::strcmp(mode, "on") == 0) setenv("APS5_TRACE_PHASES", "1", 1);
    else if (std::strcmp(mode, "zero") == 0) setenv("APS5_TRACE_PHASES", "0", 1);
    else unsetenv("APS5_TRACE_PHASES");
#endif
}

void* APS5_VABI ReturnValue(void* value) {
    returnInvocations.fetch_add(1, std::memory_order_relaxed);
    return value;
}
void* APS5_VABI ExitValue(void* value) {
    exitInvocations.fetch_add(1, std::memory_order_relaxed);
    scePthreadExit(value);
    return nullptr;
}

void JoinEntry(PthreadEntry entry, void* expected) {
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, entry, expected, nullptr) == Ok);
    void* result = nullptr;
    Require(scePthreadJoin(thread, &result) == Ok);
    Require(result == expected);
}

struct GuestCancelContext {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<unsigned> invocations{0};
    std::atomic<bool> returned{false};
};

void* APS5_VABI CancelAtTestcancel(void* raw) {
    auto& context = *static_cast<GuestCancelContext*>(raw);
    context.invocations.fetch_add(1, std::memory_order_relaxed);
    context.started.store(true, std::memory_order_release);
    for (;;) {
        scePthreadTestcancel();
        if (context.release.load(std::memory_order_acquire)) {
            // Observe a pending deferred cancellation before taking the
            // distinguishable normal-return path.
            scePthreadTestcancel();
            context.returned.store(true, std::memory_order_release);
            return reinterpret_cast<void*>(std::uintptr_t{0xCA11});
        }
        std::this_thread::yield();
    }
}

void ExerciseGuestPthreadCancellation(GuestCancelContext& context) {
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, &CancelAtTestcancel, &context, nullptr) == Ok);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!context.started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!context.started.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "GuestPhaseObserver: guest cancellation worker did not start within 1000 ms\n");
        std::fflush(stderr);
        std::abort();
    }
    Require(scePthreadCancel(thread) == Ok);
    context.release.store(true, std::memory_order_release);
    void* result = nullptr;
    Require(scePthreadJoin(thread, &result) == Ok);
    Require(result == GuestCanceled);
    Require(context.invocations.load(std::memory_order_relaxed) == 1);
    Require(!context.returned.load(std::memory_order_acquire));
}

struct WaitContext {
    KernelSema semaphore = nullptr;
    int result = 0;
};

void* APS5_VABI WaitForCanceledSemaphore(void* raw) {
    auto& context = *static_cast<WaitContext*>(raw);
    context.result = sceKernelWaitSema(context.semaphore, 1, nullptr);
    return nullptr;
}

void ExerciseGuestCancellation(KernelSema semaphore) {
    WaitContext context{semaphore};
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, WaitForCanceledSemaphore, &context, nullptr) == Ok);
    bool canceled = false;
    int lastWaiters = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        int waiters = 0;
        Require(sceKernelCancelSema(semaphore, 0, &waiters) == Ok);
        lastWaiters = waiters;
        if (waiters > 0) {
            canceled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!canceled) {
        std::fprintf(stderr, "GuestPhaseObserver: no semaphore waiter registered within 1000 ms; last_waiters=%d\n", lastWaiters);
        std::fflush(stderr);
        std::abort();
    }
    Require(scePthreadJoin(thread, nullptr) == Ok);
    Require(context.result == Canceled);
}

unsigned Count(const std::string& text, const char* needle) {
    unsigned count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += std::strlen(needle)) ++count;
    return count;
}

void Run(const char* mode) {
    SetTraceMode(mode);
    StderrCapture capture;
    returnInvocations.store(0, std::memory_order_relaxed);
    exitInvocations.store(0, std::memory_order_relaxed);

    // Same full callback workload in every mode. Cancellation first keeps its
    // callback-begin marker inside the bounded trace quota in the on-mode case.
    GuestCancelContext cancelContext;
    ExerciseGuestPthreadCancellation(cancelContext);
    JoinEntry(&ExitValue, reinterpret_cast<void*>(std::uintptr_t{0x1234}));
    for (unsigned i = 0; i < 16; ++i) JoinEntry(&ReturnValue, reinterpret_cast<void*>(std::uintptr_t{0x2000 + i}));
    Require(exitInvocations.load(std::memory_order_relaxed) == 1);
    Require(returnInvocations.load(std::memory_order_relaxed) == 16);

    KernelSema semaphore = nullptr;
    Require(sceKernelCreateSema(&semaphore, "phase-observer", 0, 1, 1, nullptr) == Ok);
    ExerciseGuestCancellation(semaphore);

    for (unsigned i = 0; i < 13; ++i) {
        Require(sceKernelSignalSema(semaphore, 1) == Ok);
        if (i == 0) {
            errno = EDOM;
#ifdef _WIN32
            SetLastError(0x5a17);
#endif
        }
        Require(sceKernelWaitSema(semaphore, 1, nullptr) == Ok);
        if (i == 0) {
            Require(errno == EDOM);
#ifdef _WIN32
            Require(GetLastError() == 0x5a17);
#endif
        }
    }
    KernelUseconds timeout = 0;
    Require(sceKernelWaitSema(semaphore, 1, &timeout) == TimedOut);
    Require(sceKernelDeleteSema(semaphore) == Ok);

    const std::string output = capture.Read();
    if (std::strcmp(mode, "on") != 0) {
        Require(output.find("[phase]") == std::string::npos);
        return;
    }

    Require(Count(output, "unit=guest_thread_callback edge=callback_begin") == 17);
    Require(Count(output, "unit=guest_thread_callback edge=callback_return") == 15);
    Require(Count(output, "unit=guest_thread_callback edge=truncated") == 1);
    std::size_t at = 0;
    unsigned callbackRecords = 0;
    while ((at = output.find("unit=guest_thread_callback", at)) != std::string::npos && callbackRecords < 2) {
        const auto lineBegin = output.rfind('\n', at);
        const auto lineEnd = output.find('\n', at);
        const auto line = output.substr(lineBegin == std::string::npos ? 0 : lineBegin + 1,
                                        (lineEnd == std::string::npos ? output.size() : lineEnd) -
                                            (lineBegin == std::string::npos ? 0 : lineBegin + 1));
        Require(line.find("edge=callback_begin") != std::string::npos);
        ++callbackRecords;
        at = lineEnd == std::string::npos ? output.size() : lineEnd + 1;
    }
    Require(callbackRecords == 2);
    Require(Count(output, "unit=semaphore_wait edge=api_entry") == 12);
    Require(Count(output, "unit=semaphore_wait edge=scope_exit") == 12);
    Require(Count(output, "unit=semaphore_wait edge=truncated") == 1);
    Require(Count(output, "unit=semaphore_wait edge=unwind") == 0);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* mode = argv[1];
    if (std::strcmp(mode, "off") != 0 && std::strcmp(mode, "zero") != 0 && std::strcmp(mode, "on") != 0) return 2;
    Run(mode);
    std::puts("GuestPhaseObserver: callback, semaphore wait/cancel, error-state, and cap checks passed");
    return 0;
}
