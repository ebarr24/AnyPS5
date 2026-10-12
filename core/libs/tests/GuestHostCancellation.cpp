#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <time.h>

extern "C" {
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag*, const char*, std::uint32_t, std::uint64_t, const void*);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag, std::uint64_t, std::uint32_t, std::uint64_t*, KernelUseconds*);
int APS5_VABI sceKernelPollEventFlag(KernelEventFlag, std::uint64_t, std::uint32_t, std::uint64_t*);
}

namespace {
using ResolveTargets = bool (*)(HostUnwindTargetInfo_nid_no_patch*);
using SetTrace = void (*)(HostUnwindTraceBuffer_nid_no_patch*);
using MarkForcedUnwind = HostUnwindMarkerResult_nid_no_patch (*)(_Unwind_Exception*, _Unwind_Reason_Code);
using ConsumeForcedUnwind = bool (*)(_Unwind_Exception*);
using MarkForcedUnwindWithTargets = HostUnwindMarkerResult_nid_no_patch (*)(
    _Unwind_Exception*, _Unwind_Reason_Code, const HostUnwindTargetInfo_nid_no_patch*);
using ConsumeForcedUnwindWithTargets = bool (*)(_Unwind_Exception*, HostUnwindTargetInfo_nid_no_patch*);
using ArmResolverGate = void (*)();
using ResolverGateEntered = bool (*)();
using ReleaseResolverGate = void (*)();
using ResolverContenders = unsigned (*)();

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

static constexpr int SCE_KERNEL_ERROR_EPERM = static_cast<int>(0x80020001);
static constexpr int SCE_KERNEL_ERROR_EBUSY = static_cast<int>(0x80020010);
static constexpr std::uint32_t EVF_ATTR_SINGLE = 0x10;
static constexpr std::uint32_t EVF_WAITMODE_OR = 0x02;

void WaitUntilEventFlagHasWaiter(KernelEventFlag flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int status = SCE_KERNEL_ERROR_EBUSY;
    do {
        status = sceKernelPollEventFlag(flag, 1, EVF_WAITMODE_OR, nullptr);
        if (status == SCE_KERNEL_ERROR_EPERM) return;
        Require(status == SCE_KERNEL_ERROR_EBUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    Require(false);
}

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
    constexpr int eventFlagCanceled = static_cast<int>(0x80020055u);
    for (;;) {
        const int result = sceKernelWaitEventFlag(context.flag, 1, 0x02, nullptr, nullptr);
        if (result != eventFlagCanceled) std::abort();
    }
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

bool SameTargets(const HostUnwindTargetInfo_nid_no_patch& left, const HostUnwindTargetInfo_nid_no_patch& right);

#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
void CheckMarkerPairState(MarkForcedUnwindWithTargets mark, ConsumeForcedUnwindWithTargets consume) {
    _Unwind_Exception first {};
    _Unwind_Exception second {};
    const char personalityName[] = "test personality owner";
    const char resumeName[] = "test resume owner";
    const HostUnwindTargetInfo_nid_no_patch targets {0x101, 0x202, personalityName, 0x303, 0x404, resumeName, true};
    HostUnwindTargetInfo_nid_no_patch consumed {};
    Require(mark(&first, _URC_CONTINUE_UNWIND, &targets) == HostUnwindMarkerResult_nid_no_patch::NoMarker);
    Require(!consume(&first, &consumed));
    Require(mark(&first, _URC_FATAL_PHASE2_ERROR, &targets) == HostUnwindMarkerResult_nid_no_patch::NoMarker);
    Require(!consume(&first, &consumed));
    Require(mark(&first, _URC_INSTALL_CONTEXT, &targets) == HostUnwindMarkerResult_nid_no_patch::Marked);
    Require(!consume(&second, &consumed));
    Require(consumed.personality == 0 && consumed.resume == 0);
    Require(mark(&second, _URC_INSTALL_CONTEXT, &targets) == HostUnwindMarkerResult_nid_no_patch::Rejected);
    Require(mark(&first, _URC_INSTALL_CONTEXT, &targets) == HostUnwindMarkerResult_nid_no_patch::Rejected);
    Require(consume(&first, &consumed));
    Require(SameTargets(consumed, targets) && consumed.contenderLocal);
    Require(!consume(&first, &consumed));
}
#endif

bool SameTargets(const HostUnwindTargetInfo_nid_no_patch& left, const HostUnwindTargetInfo_nid_no_patch& right) {
    return left.personality == right.personality && left.personalityOwner == right.personalityOwner &&
        left.personalityOwnerName == right.personalityOwnerName && left.resume == right.resume &&
        left.resumeOwner == right.resumeOwner && left.resumeOwnerName == right.resumeOwnerName;
}

void CheckResolverContention() {
    const auto resolve = Resolve<ResolveTargets>("_ResolveNativeHostUnwindTargets_nid_no_patch");
    const auto arm = Resolve<ArmResolverGate>("_ArmNativeHostUnwindResolverTestGate_nid_no_patch");
    const auto entered = Resolve<ResolverGateEntered>("_NativeHostUnwindResolverTestGateEntered_nid_no_patch");
    const auto release = Resolve<ReleaseResolverGate>("_ReleaseNativeHostUnwindResolverTestGate_nid_no_patch");
    const auto contenders = Resolve<ResolverContenders>("_NativeHostUnwindResolverTestContenders_nid_no_patch");
    Require(resolve && arm && entered && release && contenders);

    arm();
    HostUnwindTargetInfo_nid_no_patch ownerTargets {};
    bool ownerResult = false;
    std::atomic<bool> ownerDone {};
    std::thread owner([&] {
        ownerResult = resolve(&ownerTargets);
        ownerDone.store(true, std::memory_order_release);
    });

    const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered() && std::chrono::steady_clock::now() < gateDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool gateHeld = entered();

    HostUnwindTargetInfo_nid_no_patch loserTargets {};
    bool loserResult = false;
    std::atomic<bool> loserDone {};
    std::thread loser([&] {
        loserResult = resolve(&loserTargets);
        loserDone.store(true, std::memory_order_release);
    });

    const auto contentionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!gateHeld || contenders() == 0) && std::chrono::steady_clock::now() < contentionDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool enteredContention = gateHeld && contenders() == 1;
    const auto loserDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (enteredContention && !loserDone.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < loserDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const bool loserReturnedBeforeRelease = loserDone.load(std::memory_order_acquire);
    const bool loserOutputValidBeforeRelease = loserReturnedBeforeRelease && loserResult &&
        loserTargets.personality && loserTargets.personalityOwner && loserTargets.personalityOwnerName &&
        loserTargets.resume && loserTargets.resumeOwner && loserTargets.resumeOwnerName;
    const bool winnerStillHeld = gateHeld && entered() && !ownerDone.load(std::memory_order_acquire);
    release();
    owner.join();
    loser.join();

    std::fprintf(stderr,
        "resolver contention: contenderObserved=%d returnedBeforeOwnerRelease=%d winnerHeld=%d localPairValid=%d localPair=%d\n",
        enteredContention, loserReturnedBeforeRelease, winnerStillHeld, loserOutputValidBeforeRelease,
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
        loserTargets.contenderLocal
#else
        -1
#endif
    );
    Require(gateHeld && enteredContention);
    Require(loserReturnedBeforeRelease);
    Require(winnerStillHeld);
    Require(loserOutputValidBeforeRelease);
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
    Require(loserTargets.contenderLocal);
#endif
    Require(ownerResult);
    Require(ownerTargets.personality && ownerTargets.personalityOwner && ownerTargets.personalityOwnerName);
    Require(ownerTargets.resume && ownerTargets.resumeOwner && ownerTargets.resumeOwnerName);

    HostUnwindTargetInfo_nid_no_patch cachedTargets {};
    Require(resolve(&cachedTargets));
    Require(SameTargets(cachedTargets, ownerTargets));
    Require(SameTargets(cachedTargets, loserTargets));
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
    Require(!cachedTargets.contenderLocal);
#endif
}

void CheckTrace(Context& context, const HostUnwindTargetInfo_nid_no_patch& targets, ResolveTargets resolveTargets,
    const char* routeName, bool expectContenderLocal = false) {
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
    std::fprintf(stderr, "host unwind [%s] runtime owners: personality=%s base=%p resume=%s base=%p\n",
        routeName, personalityInfo.dli_fname, personalityInfo.dli_fbase, resumeInfo.dli_fname, resumeInfo.dli_fbase);
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
    unsigned kernelResumed = 0;
    for (unsigned i = 0; i < context.trace.count; ++i) {
        const auto& event = context.trace.events[i];
        Require(event.exception != 0);
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
        const int localPair = event.contenderLocal;
#else
        const int localPair = -1;
#endif
        std::fprintf(stderr, "host unwind [%s] event %u: kind=%u class=0x%016llx actions=0x%x result=%d\n",
            routeName, i, static_cast<unsigned>(event.kind), static_cast<unsigned long long>(event.exceptionClass),
            static_cast<unsigned>(event.actions), static_cast<int>(event.result));
        std::fprintf(stderr, "host unwind [%s] event %u: localPair=%d\n", routeName, i, localPair);
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
            std::fprintf(stderr, "host unwind [%s] Resume call site: %s\n", routeName,
                callInfo.dli_fname ? callInfo.dli_fname : "<unknown>");
            if (callInfo.dli_fbase == kernelInfo.dli_fbase) ++kernelResumed;
            bool sameExceptionInstalled = false;
            for (unsigned j = 0; j < i; ++j) {
                const auto& prior = context.trace.events[j];
                if (prior.kind == HostUnwindTraceKind_nid_no_patch::MarkerInstalled && prior.exception == event.exception) {
                    sameExceptionInstalled = true;
                    Require(prior.exceptionClass == event.exceptionClass);
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
                    Require(prior.contenderLocal == event.contenderLocal);
#endif
                }
            }
            Require(sameExceptionInstalled);
        }
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
        if (event.kind == HostUnwindTraceKind_nid_no_patch::Personality ||
            event.kind == HostUnwindTraceKind_nid_no_patch::MarkerInstalled ||
            event.kind == HostUnwindTraceKind_nid_no_patch::ResumeMatched)
            Require(event.contenderLocal == expectContenderLocal);
#else
        static_cast<void>(expectContenderLocal);
#endif
    }
    Require(callbacks > 0 && installed > 0 && installed == resumed);
    Require(kernelResumed > 0);
}

bool JoinBefore(pthread_t thread, void** result, unsigned seconds) {
    timespec deadline {};
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
    deadline.tv_sec += seconds;
    return pthread_timedjoin_np(thread, result, &deadline) == 0;
}

void CheckUninstrumentedConcurrentCancellation() {
    KernelEventFlag firstFlag {};
    KernelEventFlag secondFlag {};
    Require(sceKernelCreateEventFlag(&firstFlag, "host-unwind-uninstrumented-first", EVF_ATTR_SINGLE, 0, nullptr) == 0);
    Require(sceKernelCreateEventFlag(&secondFlag, "host-unwind-uninstrumented-second", EVF_ATTR_SINGLE, 0, nullptr) == 0);
    Context first {};
    Context second {};
    first.flag = firstFlag;
    second.flag = secondFlag;
    pthread_t firstThread {};
    pthread_t secondThread {};
    Require(pthread_create(&firstThread, nullptr, Worker, &first) == 0);
    Require(pthread_create(&secondThread, nullptr, Worker, &second) == 0);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!first.ready.load(std::memory_order_acquire) || !second.ready.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < readyDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(first.ready.load(std::memory_order_acquire));
    Require(second.ready.load(std::memory_order_acquire));
    first.release.store(true, std::memory_order_release);
    second.release.store(true, std::memory_order_release);
    WaitUntilEventFlagHasWaiter(firstFlag);
    WaitUntilEventFlagHasWaiter(secondFlag);
    std::fprintf(stderr, "uninstrumented concurrent cancellation: both single-waiter EventFlags are parked\n");
    Require(pthread_cancel(firstThread) == 0);
    Require(pthread_cancel(secondThread) == 0);
    void* firstResult = nullptr;
    void* secondResult = nullptr;
    Require(pthread_join(firstThread, &firstResult) == 0);
    Require(pthread_join(secondThread, &secondResult) == 0);
    Require(firstResult == PTHREAD_CANCELED && secondResult == PTHREAD_CANCELED);
    Require(first.callerFrameCleanups.load(std::memory_order_relaxed) == 1);
    Require(first.cleanupHandlers.load(std::memory_order_relaxed) == 1);
    Require(second.callerFrameCleanups.load(std::memory_order_relaxed) == 1);
    Require(second.cleanupHandlers.load(std::memory_order_relaxed) == 1);
}

void CheckConcurrentColdCancellation() {
    const auto resolve = Resolve<ResolveTargets>("_ResolveNativeHostUnwindTargets_nid_no_patch");
    const auto setTrace = Resolve<SetTrace>("_SetHostUnwindTraceBuffer_nid_no_patch");
    const auto arm = Resolve<ArmResolverGate>("_ArmNativeHostUnwindResolverTestGate_nid_no_patch");
    const auto entered = Resolve<ResolverGateEntered>("_NativeHostUnwindResolverTestGateEntered_nid_no_patch");
    const auto release = Resolve<ReleaseResolverGate>("_ReleaseNativeHostUnwindResolverTestGate_nid_no_patch");
    const auto contenders = Resolve<ResolverContenders>("_NativeHostUnwindResolverTestContenders_nid_no_patch");
    if (!resolve || !setTrace || !arm || !entered || !release || !contenders) {
        CheckUninstrumentedConcurrentCancellation();
        return;
    }

    KernelEventFlag ownerFlag {};
    KernelEventFlag contenderFlag {};
    Require(sceKernelCreateEventFlag(&ownerFlag, "host-unwind-cold-owner", EVF_ATTR_SINGLE, 0, nullptr) == 0);
    Require(sceKernelCreateEventFlag(&contenderFlag, "host-unwind-cold-contender", EVF_ATTR_SINGLE, 0, nullptr) == 0);
    Context ownerContext {};
    Context contenderContext {};
    ownerContext.flag = ownerFlag;
    contenderContext.flag = contenderFlag;
    ownerContext.setTrace = setTrace;
    contenderContext.setTrace = setTrace;

    arm();
    pthread_t ownerThread {};
    pthread_t contenderThread {};
    Require(pthread_create(&ownerThread, nullptr, Worker, &ownerContext) == 0);
    Require(pthread_create(&contenderThread, nullptr, Worker, &contenderContext) == 0);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!ownerContext.ready.load(std::memory_order_acquire) ||
            !contenderContext.ready.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < readyDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(ownerContext.ready.load(std::memory_order_acquire));
    Require(contenderContext.ready.load(std::memory_order_acquire));
    ownerContext.release.store(true, std::memory_order_release);
    contenderContext.release.store(true, std::memory_order_release);

    WaitUntilEventFlagHasWaiter(ownerFlag);
    WaitUntilEventFlagHasWaiter(contenderFlag);
    std::fprintf(stderr, "concurrent cancellation: owner and contender EventFlags are parked\n");

    Require(pthread_cancel(ownerThread) == 0);
    const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!entered() && std::chrono::steady_clock::now() < gateDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool ownerHeldBeforeNativeLookup = entered();
    Require(ownerHeldBeforeNativeLookup);
    std::fprintf(stderr, "concurrent cancellation: resolver gate entered before contender cancel\n");

    Require(pthread_cancel(contenderThread) == 0);
    void* contenderResult = nullptr;
    const bool contenderJoinedBeforeOwnerRelease = JoinBefore(contenderThread, &contenderResult, 5);
    const bool winnerStillHeld = entered();
    release();
    void* ownerResult = nullptr;
    const bool ownerJoined = JoinBefore(ownerThread, &ownerResult, 5);
    if (!contenderJoinedBeforeOwnerRelease) {
        void* ignored = nullptr;
        static_cast<void>(JoinBefore(contenderThread, &ignored, 5));
    }
    std::fprintf(stderr,
        "concurrent cancellation: gateHeld=%d contenderBeforeOwnerRelease=%d winnerStillHeld=%d contenderCanceled=%d ownerJoined=%d ownerCanceled=%d\n",
        ownerHeldBeforeNativeLookup, contenderJoinedBeforeOwnerRelease, winnerStillHeld,
        contenderResult == PTHREAD_CANCELED, ownerJoined, ownerResult == PTHREAD_CANCELED);
    Require(ownerHeldBeforeNativeLookup);
    Require(contenderJoinedBeforeOwnerRelease);
    Require(winnerStillHeld);
    Require(contenderResult == PTHREAD_CANCELED);
    Require(ownerJoined);
    Require(ownerResult == PTHREAD_CANCELED);
    Require(ownerContext.callerFrameCleanups.load(std::memory_order_relaxed) == 1);
    Require(ownerContext.cleanupHandlers.load(std::memory_order_relaxed) == 1);
    Require(contenderContext.callerFrameCleanups.load(std::memory_order_relaxed) == 1);
    Require(contenderContext.cleanupHandlers.load(std::memory_order_relaxed) == 1);
    Require(contenders() >= 1);

    HostUnwindTargetInfo_nid_no_patch publishedTargets {};
    Require(resolve(&publishedTargets));
    Require(!publishedTargets.contenderLocal);
    CheckTrace(ownerContext, publishedTargets, resolve, "winner", false);
    CheckTrace(contenderContext, publishedTargets, resolve, "contender", true);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--resolver-contention") == 0) {
        CheckResolverContention();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--concurrent-cold-cancellation") == 0) {
        CheckConcurrentColdCancellation();
        return 0;
    }

    const auto resolveTargets = Resolve<ResolveTargets>("_ResolveNativeHostUnwindTargets_nid_no_patch");
    const auto setTrace = Resolve<SetTrace>("_SetHostUnwindTraceBuffer_nid_no_patch");
    const auto mark = Resolve<MarkForcedUnwind>("_MarkNativeForcedUnwind_nid_no_patch");
    const auto consume = Resolve<ConsumeForcedUnwind>("_ConsumeNativeForcedUnwindMarker_nid_no_patch");
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
    const auto markPair = Resolve<MarkForcedUnwindWithTargets>("_MarkNativeForcedUnwindWithTargets_nid_no_patch");
    const auto consumePair = Resolve<ConsumeForcedUnwindWithTargets>("_ConsumeNativeForcedUnwindMarkerWithTargets_nid_no_patch");
#endif
    const bool anyObserver = resolveTargets || setTrace || mark || consume;
    const bool observerAvailable = resolveTargets && setTrace && mark && consume;
    Require(!anyObserver || observerAvailable);

    HostUnwindTargetInfo_nid_no_patch targets {};
    if (observerAvailable) {
        CheckMarkerState(mark, consume);
#if defined(APS5_HOST_UNWIND_TARGET_PAIR_MARKER)
        Require(markPair && consumePair);
        CheckMarkerPairState(markPair, consumePair);
#endif
    }

    Context context;
    context.setTrace = setTrace;
    Require(sceKernelCreateEventFlag(&context.flag, "host-forced-personality", EVF_ATTR_SINGLE, 0, nullptr) == 0);
    pthread_t worker {};
    Require(pthread_create(&worker, nullptr, Worker, &context) == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!context.ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(context.ready.load(std::memory_order_acquire));
    context.release.store(true, std::memory_order_release);
    WaitUntilEventFlagHasWaiter(context.flag);
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
        CheckTrace(context, targets, resolveTargets, "single");
    }
    return 0;
}
