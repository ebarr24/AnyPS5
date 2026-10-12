#include "prx/libkernel/Semaphore/include/Semaphore.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <stdexcept>
#include <string>
#include <utility>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#ifdef __linux__
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
struct PhaseRecord {
 char data[160]{};
 std::size_t length = 0;
};

void AppendPhaseChar(PhaseRecord& record, char value) noexcept {
 if (record.length < sizeof(record.data)) record.data[record.length++] = value;
}

void AppendPhaseText(PhaseRecord& record, const char* text) noexcept {
 while (*text != '\0' && record.length < sizeof(record.data)) AppendPhaseChar(record, *text++);
}

void AppendPhaseNumber(PhaseRecord& record, std::uint64_t value) noexcept {
 char digits[20];
 unsigned count = 0;
 do {
  digits[count++] = static_cast<char>('0' + value % 10);
  value /= 10;
 } while (value != 0 && count < sizeof(digits));
 while (count != 0) AppendPhaseChar(record, digits[--count]);
}

void WritePhaseRecord(const PhaseRecord& record) noexcept {
#ifdef _WIN32
 const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
 if (output != nullptr && output != INVALID_HANDLE_VALUE) {
  DWORD written = 0;
  WriteFile(output, record.data, static_cast<DWORD>(record.length), &written, nullptr);
 }
#elif defined(__linux__)
 struct stat status{};
 if (::syscall(SYS_fstat, STDERR_FILENO, &status) != 0 || !S_ISREG(status.st_mode)) return;
 std::size_t written = 0;
 for (unsigned attempt = 0; attempt < 8 && written < record.length; ++attempt) {
  const auto result = ::syscall(SYS_write, STDERR_FILENO, record.data + written, record.length - written);
  if (result > 0) written += static_cast<std::size_t>(result);
  else if (result < 0 && errno == EINTR) continue;
  else break;
 }
#else
 static_cast<void>(record);
#endif
}

void TracePhase(const char* edge) noexcept {
#ifdef _WIN32
 const DWORD savedLastError = GetLastError();
#endif
 const int savedErrno = errno;
#if defined(_WIN32) || defined(__linux__)
 static const bool enabled = [] {
  const char* value = std::getenv("APS5_TRACE_PHASES");
  return value != nullptr && value[0] == '1' && value[1] == '\0';
 }();
#else
 static const bool enabled = false;
#endif
 static std::atomic<unsigned> emitted{0};
 static std::atomic_flag truncationReported = ATOMIC_FLAG_INIT;
 if (enabled) {
  unsigned count = emitted.load(std::memory_order_relaxed);
  while (count < 24 && !emitted.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {}
  if (count < 24) {
   const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
   const auto rawNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
   const auto nanos = rawNanos < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(rawNanos);
   PhaseRecord record;
   AppendPhaseText(record, "[phase] unit=semaphore_wait edge=");
   AppendPhaseText(record, edge);
   AppendPhaseText(record, " mono_ns=");
   AppendPhaseNumber(record, nanos);
   AppendPhaseChar(record, '\n');
   WritePhaseRecord(record);
  } else if (!truncationReported.test_and_set(std::memory_order_relaxed)) {
   PhaseRecord record;
   AppendPhaseText(record, "[phase] unit=semaphore_wait edge=truncated\n");
   WritePhaseRecord(record);
  }
 }
 errno = savedErrno;
#ifdef _WIN32
 SetLastError(savedLastError);
#endif
}

class PhaseScope {
public:
 PhaseScope() noexcept : uncaught_(std::uncaught_exceptions()) { TracePhase("api_entry"); }
 ~PhaseScope() { TracePhase(std::uncaught_exceptions() > uncaught_ ? "unwind" : "scope_exit"); }
private:
 int uncaught_;
};
}

KernelSemaPrivate::KernelSemaPrivate(std::int32_t initCount, std::int32_t maxCount, std::string name, bool isFifo)
 : name(std::move(name)), tokenCount(initCount), initCount(initCount), maxCount(maxCount), isFifo(isFifo) {
}

extern "C" {

int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt) {
 (void)opt;
 if (sem == nullptr || name == nullptr || attr > 2 || init < 0 || max <= 0 || init > max) {
  APS5_INVALID_ARG_EX;
 }

 *sem = new KernelSemaPrivate(init, max, std::string(name), attr == 1);
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelPollSema(KernelSema sem, int need) {
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (sem->tokenCount < need) {
  return SCE_KERNEL_ERROR_EBUSY;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelSignalSema(KernelSema sem, int count) {
 if (sem == nullptr || count < 0) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount - sem->tokenCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 sem->tokenCount += count;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time) {
 if (sem == nullptr || need <= 0) {
  APS5_INVALID_ARG_EX;
 }

 PhaseScope phase;
 std::unique_lock<std::mutex> lock(sem->mutex);
 const std::uint64_t generation = sem->cancelGeneration;
 ++sem->waiterCount;
 ++sem->cancelableCount;
 struct WaiterGuard {
  KernelSemaPrivate* sem;
  std::uint64_t generation;
  ~WaiterGuard() {
   --sem->waiterCount;
   if (sem->cancelGeneration == generation) {
    --sem->cancelableCount;
   }
   sem->condition.NotifyAll();
  }
 } waiterGuard{sem, generation};

 const auto waitStart = std::chrono::steady_clock::now();
 const auto traceWait = [&](bool timedOut) {
  KernelTraceWait_nid_postfix("sema", __builtin_return_address(0), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), timedOut);
 };
 const auto released = [&] { return sem->tokenCount >= need || sem->deleted || sem->cancelGeneration != generation; };
 if (time == nullptr) {
  sem->condition.Wait(lock, released);
  traceWait(false);
  if (sem->deleted) {
   return SCE_KERNEL_ERROR_EACCES;
  }
  if (sem->cancelGeneration != generation) {
   return SCE_KERNEL_ERROR_ECANCELED;
  }
  sem->tokenCount -= need;
  return KERNEL_SEMA_OK;
 }

 const std::uint64_t deadline = TimedWait::DeadlineNanos(*time);
 const bool acquired = sem->condition.WaitUntil(lock, deadline, released);
 *time = static_cast<KernelUseconds>(TimedWait::RemainingMicros(deadline));
 traceWait(!acquired);
 if (sem->deleted) {
  return SCE_KERNEL_ERROR_EACCES;
 }
 if (sem->cancelGeneration != generation) {
  return SCE_KERNEL_ERROR_ECANCELED;
 }
 if (!acquired) {
  return SCE_KERNEL_ERROR_ETIMEDOUT;
 }
 sem->tokenCount -= need;
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelCancelSema(KernelSema sem, int count, int* threads) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 std::lock_guard<std::mutex> lock(sem->mutex);
 if (count > sem->maxCount) {
  return SCE_KERNEL_ERROR_EINVAL;
 }
 if (threads != nullptr) {
  *threads = sem->cancelableCount;
 }
 sem->tokenCount = count < 0 ? sem->initCount : count;
 sem->cancelableCount = 0;
 ++sem->cancelGeneration;
 sem->condition.NotifyAll();
 return KERNEL_SEMA_OK;
}

int APS5_VABI sceKernelDeleteSema(KernelSema sem) {
 if (sem == nullptr) {
  APS5_INVALID_ARG_EX;
 }

 std::unique_lock<std::mutex> lock(sem->mutex);
 sem->deleted = true;
 sem->condition.NotifyAll();
 sem->condition.Wait(lock, [&] { return sem->waiterCount == 0; });
 lock.unlock();
 delete sem;
 return KERNEL_SEMA_OK;
}

}
