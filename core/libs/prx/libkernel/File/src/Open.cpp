#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::_wopen(p.wstring().c_str(), nativeFlags, static_cast<int>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) == FILE_TYPE_PIPE) {
        errno = ESPIPE;
        return -1;
    }
    return ::_lseeki64(fd, offset, whence);
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelRead: nbytes exceeds platform limit");
    }
    return ::_read(fd, buf, static_cast<unsigned int>(n));
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelWrite: nbytes exceeds platform limit");
    }
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
static int NativeClose(int fd) {
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const int result = ::_close(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}
static int NativeUnlink(const std::filesystem::path& p) {
    return ::_wunlink(p.wstring().c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
static int NativeClose(int fd) { return ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f;
}
#endif

static int SceErrorFromErrno(int error) {
    constexpr int GuestEio = 5;
    const int guest = error > 0 && error <= 34 ? error : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

static std::optional<int> ScalarIoError(int error) {
    if (error == EACCES) return SCE_KERNEL_ERROR_EACCES;
    if (error == EAGAIN) return SCE_KERNEL_ERROR_EAGAIN;
#ifdef EWOULDBLOCK
    if (error == EWOULDBLOCK) return SCE_KERNEL_ERROR_EAGAIN;
#endif
    if (error == EBADF) return SCE_KERNEL_ERROR_EBADF;
#ifdef EDEADLK
    if (error == EDEADLK) return SCE_KERNEL_ERROR_EDEADLK;
#endif
#ifdef EDQUOT
    if (error == EDQUOT) return SCE_KERNEL_ERROR_EDQUOT;
#endif
#ifdef EFBIG
    if (error == EFBIG) return SCE_KERNEL_ERROR_EFBIG;
#endif
    if (error == EFAULT) return SCE_KERNEL_ERROR_EFAULT;
#ifdef EINTR
    if (error == EINTR) return SCE_KERNEL_ERROR_EINTR;
#endif
    if (error == EINVAL) return SCE_KERNEL_ERROR_EINVAL;
    if (error == EIO) return SCE_KERNEL_ERROR_EIO;
#ifdef EISDIR
    if (error == EISDIR) return SCE_KERNEL_ERROR_EISDIR;
#endif
#ifdef EMFILE
    if (error == EMFILE) return SCE_KERNEL_ERROR_EMFILE;
#endif
#ifdef ENFILE
    if (error == ENFILE) return SCE_KERNEL_ERROR_ENFILE;
#endif
#ifdef ENODEV
    if (error == ENODEV) return SCE_KERNEL_ERROR_ENODEV;
#endif
#ifdef ENOMEM
    if (error == ENOMEM) return SCE_KERNEL_ERROR_ENOMEM;
#endif
#ifdef ENOSPC
    if (error == ENOSPC) return SCE_KERNEL_ERROR_ENOSPC;
#endif
#ifdef ENOTDIR
    if (error == ENOTDIR) return SCE_KERNEL_ERROR_ENOTDIR;
#endif
#ifdef ENOTTY
    if (error == ENOTTY) return SCE_KERNEL_ERROR_ENOTTY;
#endif
#ifdef EOVERFLOW
    if (error == EOVERFLOW) return SCE_KERNEL_ERROR_EOVERFLOW;
#endif
#ifdef EPIPE
    if (error == EPIPE) return SCE_KERNEL_ERROR_EPIPE;
#endif
#ifdef EROFS
    if (error == EROFS) return SCE_KERNEL_ERROR_EROFS;
#endif
#ifdef ESPIPE
    if (error == ESPIPE) return SCE_KERNEL_ERROR_ESPIPE;
#endif
#ifdef ETXTBSY
    if (error == ETXTBSY) return SCE_KERNEL_ERROR_ETXTBSY;
#endif
    return std::nullopt;
}

template <typename Operation>
static std::int64_t ScalarIoResult(Operation operation, const char* function, const char* action, int descriptor) {
    const int savedErrno = errno;
    try {
        const std::int64_t result = operation();
        const int nativeError = errno;
        errno = savedErrno;
        if (result >= 0) return result;
        if (const auto error = ScalarIoError(nativeError)) return *error;
        throw std::runtime_error(std::string(function) + ": " + action + " failed, fd=" + std::to_string(descriptor) + ", errno=" + std::to_string(nativeError));
    } catch (...) {
        errno = savedErrno;
        throw;
    }
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    APS5_LOG_OUT("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, MapFlags(flags), mode);
    auto native = ResolvePath_nid_no_patch(path);
    int fd = NativeOpen(native, MapFlags(flags), mode);
#ifdef _WIN32
    if (fd < 0 && errno != ENOENT) {
        std::error_code error;
        if (std::filesystem::is_directory(native, error)) fd = File::OpenDirectoryDescriptor(native);
    }
#endif
    if (fd < 0) {
        return SceErrorFromErrno(errno);
    }
    if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC)))
        RecordWrittenPath_nid_no_patch(native);
    return fd;
}

int APS5_VABI sceKernelClose(int d) {
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(d);
#endif
    if (NativeClose(d) != 0) {
        if (errno == EBADF) return SCE_KERNEL_ERROR_EBADF;
        throw std::runtime_error(std::string(__func__) + ": close failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return 0;
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) return SCE_KERNEL_ERROR_EFAULT;
    char emptyBuffer = 0;
    void* buffer = buf == nullptr ? &emptyBuffer : buf;
    return ScalarIoResult([&] {
        const GuestArena::HostWrite destination(buffer, nbytes);
        if (!destination.Open()) {
            errno = EFAULT;
            return std::int64_t{-1};
        }
        return static_cast<std::int64_t>(NativeRead(d, buffer, nbytes));
    }, __func__, "read", d);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) return SCE_KERNEL_ERROR_EFAULT;
    const char emptyBuffer = 0;
    const void* buffer = buf == nullptr ? &emptyBuffer : buf;
    return ScalarIoResult([&] { return NativeWrite(d, buffer, nbytes); }, __func__, "write", d);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 4) return SCE_KERNEL_ERROR_EINVAL;
    if (whence == 3 || whence == 4) throw std::runtime_error(std::string(__func__) + ": whence=" + std::to_string(whence) + " is not implemented");
    return ScalarIoResult([&] { return NativeLseek(d, offset, whence); }, __func__, "lseek", d);
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (sb == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": sb is null");
    }
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) {
        return SceErrorFromErrno(2);
    }
    File::FillFileStat(native, sb);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    auto native = ResolvePath_nid_no_patch(path);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelFcntl() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
