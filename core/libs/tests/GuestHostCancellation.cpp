#include "SceTypes.hpp"
#include "prx/libc/include/exceptions/Runtime.hpp"
#include "prx/libc/include/specifics/itanium/UnwindAbi.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <thread>

extern "C" {
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag*, const char*, std::uint32_t, std::uint64_t, const void*);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag, std::uint64_t, std::uint32_t, std::uint64_t*, KernelUseconds*);
}

namespace {
using ResolveTargets = bool (*)(HostUnwindTargetInfo_nid_no_patch*);
using SetTrace = void (*)(HostUnwindTraceBuffer_nid_no_patch*);
using MarkForcedUnwind = HostUnwindMarkerResult_nid_no_patch (*)(_Unwind_Exception*, _Unwind_Reason_Code);
using ConsumeForcedUnwind = bool (*)(_Unwind_Exception*);

struct Context {
    KernelEventFlag flag {};
    HostUnwindTraceBuffer_nid_no_patch trace {};
    std::atomic<bool> ready {};
    std::atomic<bool> release {};
    std::atomic<unsigned> callerFrameCleanups {};
    std::atomic<unsigned> cleanupHandlers {};
    SetTrace setTrace {};
};

void RequireAt(bool value, const char* expression, int line) {
    if (!value) {
        std::fprintf(stderr, "guest_host_cancellation:%d: assertion failed: %s\n", line, expression);
        std::abort();
    }
}

#define Require(value) RequireAt((value), #value, __LINE__)

template<class T>
T Resolve(const char* name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct CallerFrameCleanup {
    Context* context;
    ~CallerFrameCleanup() { context->callerFrameCleanups.fetch_add(1, std::memory_order_relaxed); }
};

[[gnu::noinline]] void GuestFacingWaitFrame(Context& context) {
    CallerFrameCleanup cleanup {&context};
    const int result = sceKernelWaitEventFlag(context.flag, 1, 0x02, nullptr, nullptr);
    static_cast<void>(result);
    std::abort();
}

void CleanupHandler(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    context.cleanupHandlers.fetch_add(1, std::memory_order_relaxed);
}

void* Worker(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    if (context.setTrace) context.setTrace(&context.trace);
    pthread_cleanup_push(CleanupHandler, &context);
    int oldType = 0;
    Require(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldType) == 0);
    static_cast<void>(oldType);
    context.ready.store(true, std::memory_order_release);
    while (!context.release.load(std::memory_order_acquire)) {}
    GuestFacingWaitFrame(context);
    pthread_cleanup_pop(0);
    return nullptr;
}

void CheckMarkerState(MarkForcedUnwind mark, ConsumeForcedUnwind consume) {
    _Unwind_Exception first {};
    _Unwind_Exception second {};
    Require(mark(&first, _URC_CONTINUE_UNWIND) == HostUnwindMarkerResult_nid_no_patch::NoMarker);
    Require(!consume(&first));
    Require(mark(&first, _URC_FATAL_PHASE2_ERROR) == HostUnwindMarkerResult_nid_no_patch::NoMarker);
    Require(!consume(&first));
    Require(mark(&first, _URC_INSTALL_CONTEXT) == HostUnwindMarkerResult_nid_no_patch::Marked);
    Require(!consume(&second));
    Require(mark(&second, _URC_INSTALL_CONTEXT) == HostUnwindMarkerResult_nid_no_patch::Rejected);
    Require(mark(&first, _URC_INSTALL_CONTEXT) == HostUnwindMarkerResult_nid_no_patch::Rejected);
    Require(consume(&first));
    Require(!consume(&first));
}

void CheckTrace(Context& context, const HostUnwindTargetInfo_nid_no_patch& targets, ResolveTargets resolveTargets) {
    Require(!context.trace.overflow);
    Require(context.trace.count != 0);
    Dl_info kernelInfo {}, personalityInfo {}, resumeInfo {};
    Dl_info libcInfo {};
    const auto waitTarget = dlsym(RTLD_DEFAULT, "sceKernelWaitEventFlag");
    Require(waitTarget && dladdr(waitTarget, &kernelInfo) != 0);
    Require(dladdr(reinterpret_cast<void*>(resolveTargets), &libcInfo) != 0);
    Require(dladdr(reinterpret_cast<void*>(targets.personality), &personalityInfo) != 0);
    Require(dladdr(reinterpret_cast<void*>(targets.resume), &resumeInfo) != 0);
    Require(reinterpret_cast<std::uintptr_t>(personalityInfo.dli_fbase) == targets.personalityOwner);
    Require(reinterpret_cast<std::uintptr_t>(resumeInfo.dli_fbase) == targets.resumeOwner);
    Require(std::strcmp(personalityInfo.dli_fname, targets.personalityOwnerName) == 0);
    Require(std::strcmp(resumeInfo.dli_fname, targets.resumeOwnerName) == 0);
    Require(personalityInfo.dli_fbase != libcInfo.dli_fbase);
    Require(resumeInfo.dli_fbase != libcInfo.dli_fbase);
    std::fprintf(stderr, "host unwind runtime owners: personality=%s resume=%s\n", personalityInfo.dli_fname, resumeInfo.dli_fname);
    const auto isRuntime = [](const char* path, const char* prefix) {
        if (!path) return false;
        const char* name = std::strrchr(path, '/');
        name = name ? name + 1 : path;
        return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
    };
    Require(isRuntime(personalityInfo.dli_fname, "libstdc++.so") || isRuntime(personalityInfo.dli_fname, "libgcc_s.so"));
    Require(isRuntime(resumeInfo.dli_fname, "libgcc_s.so"));

    unsigned callbacks = 0;
    unsigned installed = 0;
    unsigned resumed = 0;
    for (unsigned i = 0; i < context.trace.count; ++i) {
        const auto& event = context.trace.events[i];
        std::fprintf(stderr, "host unwind event %u: kind=%u class=0x%016llx actions=0x%x result=%d\n",
            i, static_cast<unsigned>(event.kind), static_cast<unsigned long long>(event.exceptionClass),
            static_cast<unsigned>(event.actions), static_cast<int>(event.result));
        if (event.kind == HostUnwindTraceKind_nid_no_patch::Personality) {
            ++callbacks;
            Require((event.actions & (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE)) == (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE));
            Require(!(event.actions & _UA_SEARCH_PHASE));
            Require(event.target == targets.personality);
            Require(event.owner == targets.personalityOwner);
            Require(!LibcException::Native(event.exceptionClass));
        } else if (event.kind == HostUnwindTraceKind_nid_no_patch::MarkerInstalled) {
            ++installed;
            Require((event.actions & (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE)) == (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE));
            Require(!(event.actions & _UA_SEARCH_PHASE));
            Require(event.result == _URC_INSTALL_CONTEXT);
            Require(event.target == targets.personality);
            Require(event.owner == targets.personalityOwner);
            Require(!LibcException::Native(event.exceptionClass));
        } else if (event.kind == HostUnwindTraceKind_nid_no_patch::ResumeMatched) {
            ++resumed;
            Require(event.target == targets.resume);
            Require(event.owner == targets.resumeOwner);
            Require(!LibcException::Native(event.exceptionClass));
            Dl_info callInfo {};
            Require(event.callSite != 0 && dladdr(reinterpret_cast<void*>(event.callSite), &callInfo) != 0);
            Require(callInfo.dli_fbase == kernelInfo.dli_fbase);
            bool sameExceptionInstalled = false;
            for (unsigned j = 0; j < context.trace.count; ++j) {
                const auto& prior = context.trace.events[j];
                if (prior.kind == HostUnwindTraceKind_nid_no_patch::MarkerInstalled && prior.exception == event.exception) {
                    sameExceptionInstalled = true;
                    Require(prior.exceptionClass == event.exceptionClass);
                }
            }
            Require(sameExceptionInstalled);
        }
    }
    Require(callbacks > 0 && installed > 0 && installed == resumed);
}
}

int main() {
    const auto resolveTargets = Resolve<ResolveTargets>("_ResolveNativeHostUnwindTargets_nid_no_patch");
    const auto setTrace = Resolve<SetTrace>("_SetHostUnwindTraceBuffer_nid_no_patch");
    const auto mark = Resolve<MarkForcedUnwind>("_MarkNativeForcedUnwind_nid_no_patch");
    const auto consume = Resolve<ConsumeForcedUnwind>("_ConsumeNativeForcedUnwindMarker_nid_no_patch");
    const bool anyObserver = resolveTargets || setTrace || mark || consume;
    const bool observerAvailable = resolveTargets && setTrace && mark && consume;
    Require(!anyObserver || observerAvailable);

    HostUnwindTargetInfo_nid_no_patch targets {};
    if (observerAvailable) {
        CheckMarkerState(mark, consume);
    }

    Context context;
    context.setTrace = setTrace;
    Require(sceKernelCreateEventFlag(&context.flag, "host-forced-personality", 0, 0, nullptr) == 0);
    pthread_t worker {};
    Require(pthread_create(&worker, nullptr, Worker, &context) == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!context.ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(context.ready.load(std::memory_order_acquire));
    context.release.store(true, std::memory_order_release);
    std::fprintf(stderr, "host unwind test: issuing deferred pthread cancellation\n");
    Require(pthread_cancel(worker) == 0);
    void* result = nullptr;
    Require(pthread_join(worker, &result) == 0);
    Require(result == PTHREAD_CANCELED);
    Require(context.callerFrameCleanups.load(std::memory_order_relaxed) == 1);
    Require(context.cleanupHandlers.load(std::memory_order_relaxed) == 1);
    if (observerAvailable) {
        Require(resolveTargets(&targets));
        Require(targets.personality != 0 && targets.personalityOwner != 0 && targets.personalityOwnerName);
        Require(targets.resume != 0 && targets.resumeOwner != 0 && targets.resumeOwnerName);
        CheckTrace(context, targets, resolveTargets);
    }
    return 0;
}
