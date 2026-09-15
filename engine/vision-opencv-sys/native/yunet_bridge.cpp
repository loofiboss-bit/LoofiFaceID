// SPDX-License-Identifier: GPL-3.0-or-later

#include "yunet_bridge.h"
#include "yunet_output_validation.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define KFACEAUTH_HAS_LANDLOCK_HEADER 1
#endif
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
constexpr size_t ExpectedModelBytes = 232589;
constexpr size_t ExpectedSFaceModelBytes = 38696353;
constexpr int32_t MaximumTopK = 5000;
constexpr size_t MaximumDetections = 5000;
constexpr int32_t MaximumWidth = 1920;
constexpr int32_t MaximumHeight = 1080;
constexpr int32_t MaximumThreadCount = 16;
constexpr int32_t TrackingWidth = 320;
constexpr int32_t TrackingHeight = 320;

enum class Backend : int32_t
{
    Cpu = KFACEAUTH_YUNET_BACKEND_CPU,
    OpenVino = KFACEAUTH_YUNET_BACKEND_OPENVINO,
    Vulkan = KFACEAUTH_YUNET_BACKEND_VULKAN,
};

struct BackendSpec
{
    Backend kind;
    int backendId;
    int targetId;
};

constexpr BackendSpec CpuBackend{Backend::Cpu, cv::dnn::DNN_BACKEND_OPENCV, cv::dnn::DNN_TARGET_CPU};
constexpr BackendSpec OpenVinoBackend{Backend::OpenVino, cv::dnn::DNN_BACKEND_INFERENCE_ENGINE,
                                      cv::dnn::DNN_TARGET_CPU};
constexpr BackendSpec VulkanBackend{Backend::Vulkan, cv::dnn::DNN_BACKEND_VKCOM, cv::dnn::DNN_TARGET_VULKAN};

std::mutex ThreadMutex;
int32_t ConfiguredThreadCount = 0;
std::atomic<bool> WorkerSandboxApplied{false};
std::atomic<bool> SeccompInstalled{false};

class PinnedRegion;

struct Detector
{
    cv::Ptr<cv::FaceDetectorYN> value;
    Backend backend = Backend::Cpu;
    std::vector<uint8_t> model;
    std::unique_ptr<PinnedRegion> pinnedModel;
    float scoreThreshold = 0.9F;
    float nmsThreshold = 0.3F;
    int32_t topK = MaximumTopK;
};

struct Recognizer
{
    cv::Ptr<cv::FaceRecognizerSF> value;
    Backend backend = Backend::Cpu;
    std::vector<uint8_t> model;
    std::unique_ptr<PinnedRegion> pinnedModel;
};

static_assert(sizeof(KFaceAuthYuNetDetection) == sizeof(float) * 15);
static_assert(alignof(KFaceAuthYuNetDetection) == alignof(float));

bool validGeometry(int32_t width, int32_t height)
{
    return width > 0 && width <= MaximumWidth && height > 0 && height <= MaximumHeight;
}

bool validThreshold(float value)
{
    return std::isfinite(value) && value > 0.0F && value < 1.0F;
}

bool validPackedBgr(size_t bgrSize, int32_t width, int32_t height, size_t stride)
{
    if (!validGeometry(width, height))
        return false;
    const auto widthSize = static_cast<size_t>(width);
    const auto heightSize = static_cast<size_t>(height);
    if (widthSize > std::numeric_limits<size_t>::max() / 3)
        return false;
    const size_t minimumStride = widthSize * 3;
    return stride == minimumStride && heightSize <= std::numeric_limits<size_t>::max() / stride &&
           bgrSize == stride * heightSize;
}

void secureClear(void *data, size_t size)
{
    if (!data || size == 0)
        return;
    auto *bytes = static_cast<volatile uint8_t *>(data);
    for (size_t index = 0; index < size; ++index)
        bytes[index] = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

void clearBytes(std::vector<uint8_t> *bytes)
{
    if (bytes && !bytes->empty())
        secureClear(bytes->data(), bytes->size());
}

void clearMat(cv::Mat *matrix)
{
    if (!matrix || matrix->empty() || !matrix->data)
        return;
    const size_t rowBytes = matrix->step[0];
    for (int row = 0; row < matrix->rows; ++row)
        secureClear(matrix->ptr<uint8_t>(row), rowBytes);
}

class MatClearGuard
{
  public:
    MatClearGuard(cv::Mat *first, cv::Mat *second, cv::Mat *third, cv::Mat *fourth = nullptr)
        : matrices_{first, second, third, fourth}
    {
    }

    ~MatClearGuard()
    {
        for (cv::Mat *matrix : matrices_)
            clearMat(matrix);
    }

    MatClearGuard(const MatClearGuard &) = delete;
    MatClearGuard &operator=(const MatClearGuard &) = delete;

  private:
    std::array<cv::Mat *, 4> matrices_;
};

class PinnedRegion
{
  public:
    PinnedRegion() = default;

    PinnedRegion(void *data, size_t size) : data_(data), size_(size), locked_(tryLock(data, size)) {}

    PinnedRegion(const PinnedRegion &) = delete;
    PinnedRegion &operator=(const PinnedRegion &) = delete;

    PinnedRegion(PinnedRegion &&other) noexcept : data_(other.data_), size_(other.size_), locked_(other.locked_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.locked_ = false;
    }

    PinnedRegion &operator=(PinnedRegion &&other) noexcept
    {
        if (this != &other)
        {
            unlock();
            data_ = other.data_;
            size_ = other.size_;
            locked_ = other.locked_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.locked_ = false;
        }
        return *this;
    }

    ~PinnedRegion()
    {
        unlock();
    }

    [[nodiscard]] bool locked() const
    {
        return locked_;
    }

    void unlock()
    {
#if defined(__linux__)
        if (locked_ && data_ && size_ > 0)
            ::munlock(data_, size_);
#endif
        locked_ = false;
    }

  private:
    static bool tryLock(void *data, size_t size)
    {
#if defined(__linux__) && defined(SYS_mlock2)
        if (!data || size == 0)
            return false;
        struct rlimit limit{};
        if (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur < limit.rlim_max)
        {
            struct rlimit raised = limit;
            raised.rlim_cur = limit.rlim_max;
            (void)::setrlimit(RLIMIT_MEMLOCK, &raised);
        }
        return ::syscall(SYS_mlock2, data, size, MLOCK_ONFAULT) == 0;
#else
        (void)data;
        (void)size;
        return false;
#endif
    }

    void *data_ = nullptr;
    size_t size_ = 0;
    bool locked_ = false;
};

bool requireMemoryPinning()
{
    const char *value = std::getenv("KFACEAUTH_REQUIRE_MEMLOCK");
    return value && std::strcmp(value, "1") == 0;
}

int32_t defaultThreadCount()
{
    const char *overrideValue = std::getenv("KFACEAUTH_OPENCV_THREADS");
    if (overrideValue)
    {
        char *end = nullptr;
        const long parsed = std::strtol(overrideValue, &end, 10);
        if (end != overrideValue && *end == '\0' && parsed >= 1 && parsed <= MaximumThreadCount)
            return static_cast<int32_t>(parsed);
    }
    return cv::getNumberOfCPUs() <= 4 ? 2 : 4;
}

void configureThreads()
{
    std::lock_guard lock(ThreadMutex);
    if (ConfiguredThreadCount == 0)
    {
        ConfiguredThreadCount = defaultThreadCount();
        cv::setNumThreads(ConfiguredThreadCount);
    }
}

bool environmentFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "yes") == 0);
}

bool backendDisabled(Backend backend)
{
    if (backend == Backend::OpenVino)
        return environmentFlag("KFACEAUTH_TEST_FAIL_OPENVINO");
    if (backend == Backend::Vulkan)
        return environmentFlag("KFACEAUTH_TEST_FAIL_VULKAN");
    return false;
}

bool backendAvailable(Backend backend)
{
    if (backend == Backend::Cpu)
        return true;
#if !defined(KFACEAUTH_COMPILED_OPENVINO)
    if (backend == Backend::OpenVino)
        return false;
#endif
    if (backendDisabled(backend))
        return false;
    if (backend == Backend::Vulkan && !WorkerSandboxApplied.load(std::memory_order_acquire) &&
        !environmentFlag("KFACEAUTH_ALLOW_UNSANDBOXED_ACCELERATION"))
        return false;

    const BackendSpec requested = backend == Backend::OpenVino ? OpenVinoBackend : VulkanBackend;
    try
    {
        const auto available = cv::dnn::getAvailableBackends();
        return std::any_of(available.cbegin(), available.cend(), [&requested](const auto &entry)
                           { return entry.first == requested.backendId && entry.second == requested.targetId; });
    }
    catch (...)
    {
        return false;
    }
}

struct BackendRequest
{
    Backend requested = Backend::Cpu;
    bool automatic = true;
};

BackendRequest requestedBackend()
{
    const char *value = std::getenv("KFACEAUTH_INFERENCE_BACKEND");
    if (!value)
        value = std::getenv("KFACEAUTH_ACCELERATION");
    if (!value || std::strcmp(value, "auto") == 0)
        return {};
    if (std::strcmp(value, "openvino") == 0)
        return {Backend::OpenVino, false};
    if (std::strcmp(value, "vulkan") == 0)
        return {Backend::Vulkan, false};
    return {Backend::Cpu, false};
}

std::array<BackendSpec, 3> backendCandidates()
{
    const BackendRequest request = requestedBackend();
    if (!request.automatic)
    {
        if (request.requested == Backend::OpenVino)
            return {OpenVinoBackend, CpuBackend, CpuBackend};
        if (request.requested == Backend::Vulkan)
            return {VulkanBackend, CpuBackend, CpuBackend};
        return {CpuBackend, CpuBackend, CpuBackend};
    }
    return {OpenVinoBackend, VulkanBackend, CpuBackend};
}

template <typename Factory>
auto createWithFallback(Factory factory, Backend *selected, std::vector<uint8_t> *modelForFallback)
    -> decltype(factory(CpuBackend))
{
    for (const BackendSpec &candidate : backendCandidates())
    {
        if (!backendAvailable(candidate.kind))
            continue;
        try
        {
            auto value = factory(candidate);
            if (!value.empty())
            {
                *selected = candidate.kind;
                if (candidate.kind == Backend::Cpu)
                    clearBytes(modelForFallback);
                return value;
            }
        }
        catch (...)
        {
            // An unavailable accelerator is an expected runtime condition.
        }
    }
    return {};
}

template <typename Factory>
auto createCpu(Factory factory, std::vector<uint8_t> *modelForFallback) -> decltype(factory(CpuBackend))
{
    try
    {
        auto value = factory(CpuBackend);
        if (!value.empty())
            return value;
    }
    catch (...)
    {
    }
    clearBytes(modelForFallback);
    return {};
}
} // namespace

extern "C" int kfaceauth_yunet_disable_core_dumps(void)
{
#if defined(__linux__)
    const rlimit limit{0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) != 0)
        return KFACEAUTH_YUNET_HARDENING_FAILURE;
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        return KFACEAUTH_YUNET_HARDENING_FAILURE;
#endif
    return KFACEAUTH_YUNET_OK;
}

extern "C" const char *kfaceauth_yunet_opencv_version(void)
{
    return CV_VERSION;
}

extern "C" int kfaceauth_yunet_set_thread_count(int32_t thread_count)
{
    if (thread_count < 1 || thread_count > MaximumThreadCount)
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;
    std::lock_guard lock(ThreadMutex);
    cv::setNumThreads(thread_count);
    ConfiguredThreadCount = thread_count;
    return KFACEAUTH_YUNET_OK;
}

extern "C" int32_t kfaceauth_yunet_thread_count(void)
{
    configureThreads();
    return cv::getNumThreads();
}

#if defined(__linux__) && defined(KFACEAUTH_HAS_LANDLOCK_HEADER) && defined(SYS_landlock_create_ruleset) &&            \
    defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self)
namespace
{
constexpr __u64 LandlockReadAccess =
    LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
constexpr __u64 LandlockWriteAccess = LANDLOCK_ACCESS_FS_WRITE_FILE;

__u64 landlockHandledAccess(__u32 abi)
{
    __u64 handled = LandlockReadAccess | LandlockWriteAccess;
#ifdef LANDLOCK_ACCESS_FS_REMOVE_DIR
    handled |= LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
               LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
               LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#endif
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    return handled;
}

bool addLandlockRule(int ruleset, const char *path, __u64 access, bool required)
{
    const int descriptor = ::open(path, O_PATH | O_CLOEXEC);
    if (descriptor < 0)
        return !required;
    struct landlock_path_beneath_attr rule{static_cast<__u64>(access), descriptor};
    const long result = ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    const int savedErrno = errno;
    ::close(descriptor);
    return result == 0 || (!required && savedErrno == ENOENT);
}

bool applyLandlock(const char *modelRoot, const char *writableRoot)
{
    if (!modelRoot || *modelRoot == '\0')
        return false;
    struct landlock_ruleset_attr probe{};
    const long abiResult =
        ::syscall(SYS_landlock_create_ruleset, &probe, sizeof(probe), LANDLOCK_CREATE_RULESET_VERSION);
    if (abiResult < 0)
        return false;
    const auto abi = static_cast<__u32>(abiResult);
    probe.handled_access_fs = landlockHandledAccess(abi);
    const int ruleset = static_cast<int>(::syscall(SYS_landlock_create_ruleset, &probe, sizeof(probe), 0));
    if (ruleset < 0)
        return false;

    const __u64 modelAccess = LandlockReadAccess;
    bool valid = addLandlockRule(ruleset, modelRoot, modelAccess, true);
    if (writableRoot && *writableRoot)
        valid = addLandlockRule(ruleset, writableRoot, landlockHandledAccess(abi), true) && valid;
    const bool allowGpuPaths = requestedBackend().automatic || requestedBackend().requested == Backend::Vulkan;
    if (allowGpuPaths)
    {
        valid = addLandlockRule(ruleset, "/usr/share/vulkan/icd.d", modelAccess, false) && valid;
        valid = addLandlockRule(ruleset, "/usr/lib64/dri", modelAccess, false) && valid;
        valid = addLandlockRule(ruleset, "/usr/lib/dri", modelAccess, false) && valid;
        valid = addLandlockRule(ruleset, "/usr/lib64", modelAccess, false) && valid;
        valid = addLandlockRule(ruleset, "/usr/lib", modelAccess, false) && valid;
        valid = addLandlockRule(ruleset, "/dev/dri", modelAccess | LandlockWriteAccess, false) && valid;
    }
    if (!valid)
    {
        ::close(ruleset);
        return false;
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || ::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0)
    {
        ::close(ruleset);
        return false;
    }
    ::close(ruleset);
    WorkerSandboxApplied.store(true, std::memory_order_release);
    return true;
}
} // namespace
#endif

extern "C" int kfaceauth_yunet_configure_worker_sandbox(const char *model_root, const char *writable_root)
{
    if (WorkerSandboxApplied.load(std::memory_order_acquire))
        return KFACEAUTH_YUNET_SANDBOX_ALREADY_APPLIED;
#if defined(__linux__) && defined(KFACEAUTH_HAS_LANDLOCK_HEADER) && defined(SYS_landlock_create_ruleset) &&            \
    defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self)
    if (applyLandlock(model_root, writable_root))
        return KFACEAUTH_YUNET_SANDBOX_APPLIED;
    if (!model_root || ::access(model_root, R_OK | X_OK) != 0)
        return KFACEAUTH_YUNET_SANDBOX_FAILURE;
    return KFACEAUTH_YUNET_SANDBOX_UNAVAILABLE;
#else
    (void)model_root;
    (void)writable_root;
    return KFACEAUTH_YUNET_SANDBOX_UNAVAILABLE;
#endif
}

#if defined(__linux__)
namespace
{
void appendAllowedSyscall(std::vector<sock_filter> *filter, int syscallNumber)
{
    filter->push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(syscallNumber), 0, 1));
    filter->push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
}

bool installSeccompFilter(bool allowDrmIoctl)
{
    if (SeccompInstalled.load(std::memory_order_acquire))
        return true;
    std::vector<sock_filter> filter;
    filter.reserve(64);
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
#if defined(__x86_64__)
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
#elif defined(__aarch64__)
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0));
#else
    return false;
#endif
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

#define KFACEAUTH_ALLOW_SYSCALL(name) appendAllowedSyscall(&filter, __NR_##name)
    KFACEAUTH_ALLOW_SYSCALL(read);
    KFACEAUTH_ALLOW_SYSCALL(write);
    KFACEAUTH_ALLOW_SYSCALL(close);
    KFACEAUTH_ALLOW_SYSCALL(fstat);
    KFACEAUTH_ALLOW_SYSCALL(newfstatat);
    KFACEAUTH_ALLOW_SYSCALL(openat);
    KFACEAUTH_ALLOW_SYSCALL(readlinkat);
    KFACEAUTH_ALLOW_SYSCALL(lseek);
    KFACEAUTH_ALLOW_SYSCALL(mmap);
    KFACEAUTH_ALLOW_SYSCALL(mprotect);
    KFACEAUTH_ALLOW_SYSCALL(munmap);
    KFACEAUTH_ALLOW_SYSCALL(brk);
    KFACEAUTH_ALLOW_SYSCALL(madvise);
    KFACEAUTH_ALLOW_SYSCALL(mremap);
    KFACEAUTH_ALLOW_SYSCALL(futex);
    KFACEAUTH_ALLOW_SYSCALL(clock_gettime);
    KFACEAUTH_ALLOW_SYSCALL(getpid);
    KFACEAUTH_ALLOW_SYSCALL(gettid);
    KFACEAUTH_ALLOW_SYSCALL(getrandom);
    KFACEAUTH_ALLOW_SYSCALL(rt_sigaction);
    KFACEAUTH_ALLOW_SYSCALL(rt_sigprocmask);
    KFACEAUTH_ALLOW_SYSCALL(rt_sigreturn);
    KFACEAUTH_ALLOW_SYSCALL(set_robust_list);
    KFACEAUTH_ALLOW_SYSCALL(rseq);
    KFACEAUTH_ALLOW_SYSCALL(sched_getaffinity);
    KFACEAUTH_ALLOW_SYSCALL(sched_yield);
    KFACEAUTH_ALLOW_SYSCALL(prctl);
#ifdef __NR_io_uring_setup
    KFACEAUTH_ALLOW_SYSCALL(io_uring_setup);
#endif
    KFACEAUTH_ALLOW_SYSCALL(exit);
    KFACEAUTH_ALLOW_SYSCALL(exit_group);
#ifdef __NR_clone
    KFACEAUTH_ALLOW_SYSCALL(clone);
#endif
#ifdef __NR_clone3
    KFACEAUTH_ALLOW_SYSCALL(clone3);
#endif
#ifdef __NR_getdents64
    KFACEAUTH_ALLOW_SYSCALL(getdents64);
#endif
#undef KFACEAUTH_ALLOW_SYSCALL

    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, static_cast<__u8>(allowDrmIoctl ? 1 : 0), 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    if (allowDrmIoctl)
    {
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])));
        filter.push_back(BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 8));
        filter.push_back(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xff));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x64, 1, 0));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    struct sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
#ifdef SYS_seccomp
    if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) != 0)
        return false;
#else
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
        return false;
#endif
    SeccompInstalled.store(true, std::memory_order_release);
    return true;
}
} // namespace
#endif

extern "C" int kfaceauth_yunet_install_seccomp(int allow_drm_ioctl)
{
#if defined(__linux__)
    return installSeccompFilter(allow_drm_ioctl != 0) ? KFACEAUTH_YUNET_OK : KFACEAUTH_YUNET_HARDENING_FAILURE;
#else
    (void)allow_drm_ioctl;
    return KFACEAUTH_YUNET_HARDENING_FAILURE;
#endif
}

extern "C" int kfaceauth_yunet_backend(void *engine)
{
    if (!engine)
        return KFACEAUTH_YUNET_BACKEND_CPU;
    return static_cast<int>(static_cast<Detector *>(engine)->backend);
}

extern "C" const char *kfaceauth_yunet_backend_name(int backend)
{
    switch (backend)
    {
    case KFACEAUTH_YUNET_BACKEND_OPENVINO:
        return "openvino";
    case KFACEAUTH_YUNET_BACKEND_VULKAN:
        return "vulkan";
    case KFACEAUTH_YUNET_BACKEND_CPU:
    default:
        return "cpu";
    }
}

extern "C" int kfaceauth_yunet_create(const uint8_t *model_bytes, size_t model_size, int32_t width, int32_t height,
                                      float score_threshold, float nms_threshold, int32_t top_k, void **detector_out)
{
    if (!model_bytes || model_size != ExpectedModelBytes || !detector_out || *detector_out ||
        !validGeometry(width, height) || !validThreshold(score_threshold) || !validThreshold(nms_threshold) ||
        top_k <= 0 || top_k > MaximumTopK)
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    configureThreads();
    std::vector<uint8_t> model;
    try
    {
        model.assign(model_bytes, model_bytes + model_size);
        const std::vector<uint8_t> emptyConfig;
        Backend selected = Backend::Cpu;
        auto detector = createWithFallback(
            [&model, &emptyConfig, width, height, score_threshold, nms_threshold, top_k](const BackendSpec &backend)
            {
                return cv::FaceDetectorYN::create("ONNX", model, emptyConfig, cv::Size(width, height), score_threshold,
                                                  nms_threshold, top_k, backend.backendId, backend.targetId);
            },
            &selected, &model);
        if (detector.empty())
        {
            clearBytes(&model);
            return KFACEAUTH_YUNET_RUNTIME_FAILURE;
        }
        constexpr float ThresholdTolerance = 1.0e-6F;
        if (std::abs(detector->getScoreThreshold() - score_threshold) > ThresholdTolerance ||
            std::abs(detector->getNMSThreshold() - nms_threshold) > ThresholdTolerance || detector->getTopK() != top_k)
        {
            clearBytes(&model);
            return KFACEAUTH_YUNET_RUNTIME_FAILURE;
        }
        auto result = std::make_unique<Detector>();
        result->value = std::move(detector);
        result->backend = selected;
        result->scoreThreshold = score_threshold;
        result->nmsThreshold = nms_threshold;
        result->topK = top_k;
        if (selected != Backend::Cpu)
        {
            result->model = std::move(model);
            result->pinnedModel = std::make_unique<PinnedRegion>(result->model.data(), result->model.size());
            if (requireMemoryPinning() && !result->pinnedModel->locked())
            {
                clearBytes(&result->model);
                return KFACEAUTH_YUNET_HARDENING_FAILURE;
            }
        }
        clearBytes(&model);
        *detector_out = result.release();
        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        clearBytes(&model);
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}

extern "C" int kfaceauth_yunet_detect(void *detector, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width,
                                      int32_t height, size_t stride, KFaceAuthYuNetDetection *detections,
                                      size_t detection_capacity, size_t *detection_count)
{
    return kfaceauth_yunet_detect_scaled(detector, bgr_bytes, bgr_size, width, height, stride, 0, 0, detections,
                                         detection_capacity, detection_count);
}

namespace
{
int detectOnce(Detector *detector, const uint8_t *bgrBytes, int32_t width, int32_t height, size_t stride,
               int32_t inferenceWidth, int32_t inferenceHeight, KFaceAuthYuNetDetection *detections,
               size_t detectionCapacity, size_t *detectionCount)
{
    *detectionCount = 0;
    cv::Mat image;
    cv::Mat inference;
    cv::Mat faces;
    PinnedRegion pinnedInput;
    PinnedRegion pinnedInference;
    PinnedRegion pinnedFaces;
    MatClearGuard clearOnExit(&image, &inference, &faces);
    try
    {
        image.create(height, width, CV_8UC3);
        for (int32_t row = 0; row < height; ++row)
        {
            const auto rowOffset = static_cast<size_t>(row) * stride;
            std::copy_n(bgrBytes + rowOffset, stride, image.ptr<uint8_t>(row));
        }
        pinnedInput = PinnedRegion(image.data, image.total() * image.elemSize());
        if (requireMemoryPinning() && !pinnedInput.locked())
            return KFACEAUTH_YUNET_HARDENING_FAILURE;

        const bool scaled =
            inferenceWidth > 0 && inferenceHeight > 0 && (inferenceWidth != width || inferenceHeight != height);
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        int32_t offsetX = 0;
        int32_t offsetY = 0;
        if (scaled)
        {
            const double scale =
                std::min(static_cast<double>(inferenceWidth) / width, static_cast<double>(inferenceHeight) / height);
            const int32_t resizedWidth = std::max(1, static_cast<int32_t>(std::lround(width * scale)));
            const int32_t resizedHeight = std::max(1, static_cast<int32_t>(std::lround(height * scale)));
            scaleX = static_cast<float>(resizedWidth) / static_cast<float>(width);
            scaleY = static_cast<float>(resizedHeight) / static_cast<float>(height);
            offsetX = (inferenceWidth - resizedWidth) / 2;
            offsetY = (inferenceHeight - resizedHeight) / 2;
            cv::Mat resized;
            cv::resize(image, resized, cv::Size(resizedWidth, resizedHeight), 0.0, 0.0, cv::INTER_AREA);
            inference = cv::Mat::zeros(inferenceHeight, inferenceWidth, CV_8UC3);
            resized.copyTo(inference(cv::Rect(offsetX, offsetY, resizedWidth, resizedHeight)));
            clearMat(&resized);
        }
        else
        {
            inference = image;
        }
        pinnedInference = PinnedRegion(inference.data, inference.total() * inference.elemSize());
        if (requireMemoryPinning() && !pinnedInference.locked())
        {
            clearMat(&image);
            if (scaled)
                clearMat(&inference);
            return KFACEAUTH_YUNET_HARDENING_FAILURE;
        }

        detector->value->setInputSize(inference.size());
        detector->value->detect(inference, faces);
        if (faces.empty())
        {
            clearMat(&image);
            if (scaled)
                clearMat(&inference);
            return KFACEAUTH_YUNET_OK;
        }
        pinnedFaces = PinnedRegion(faces.data, faces.total() * faces.elemSize());
        if (requireMemoryPinning() && !pinnedFaces.locked())
        {
            clearMat(&image);
            if (scaled)
                clearMat(&inference);
            clearMat(&faces);
            return KFACEAUTH_YUNET_HARDENING_FAILURE;
        }
        if (!KFaceAuthYuNet::validOutputShape(faces.dims, faces.type(), CV_32FC1, faces.rows, faces.cols))
        {
            clearMat(&image);
            if (scaled)
                clearMat(&inference);
            clearMat(&faces);
            return KFACEAUTH_YUNET_MALFORMED_OUTPUT;
        }
        const auto count = static_cast<size_t>(faces.rows);
        if (count > detectionCapacity)
        {
            clearMat(&image);
            if (scaled)
                clearMat(&inference);
            clearMat(&faces);
            return KFACEAUTH_YUNET_OUTPUT_TOO_LARGE;
        }
        for (size_t row = 0; row < count; ++row)
        {
            const float *source = faces.ptr<float>(static_cast<int>(row));
            auto &destination = detections[row].values;
            destination[0] = (source[0] - static_cast<float>(offsetX)) / scaleX;
            destination[1] = (source[1] - static_cast<float>(offsetY)) / scaleY;
            destination[2] = source[2] / scaleX;
            destination[3] = source[3] / scaleY;
            for (int landmark = 0; landmark < 5; ++landmark)
            {
                destination[4 + landmark * 2] = (source[4 + landmark * 2] - static_cast<float>(offsetX)) / scaleX;
                destination[5 + landmark * 2] = (source[5 + landmark * 2] - static_cast<float>(offsetY)) / scaleY;
            }
            destination[14] = source[14];
        }
        *detectionCount = count;
        clearMat(&image);
        if (scaled)
            clearMat(&inference);
        clearMat(&faces);
        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        clearMat(&image);
        if (!inference.empty() && inference.data != image.data)
            clearMat(&inference);
        clearMat(&faces);
        *detectionCount = 0;
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}
} // namespace

extern "C" int kfaceauth_yunet_detect_scaled(void *detector, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width,
                                             int32_t height, size_t stride, int32_t inference_width,
                                             int32_t inference_height, KFaceAuthYuNetDetection *detections,
                                             size_t detection_capacity, size_t *detection_count)
{
    if (!detector || !bgr_bytes || !detections || !detection_count ||
        !validPackedBgr(bgr_size, width, height, stride) || detection_capacity == 0 ||
        detection_capacity > MaximumDetections || (inference_width == 0) != (inference_height == 0) ||
        (inference_width != 0 && !validGeometry(inference_width, inference_height)))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    auto *typedDetector = static_cast<Detector *>(detector);
    int status = detectOnce(typedDetector, bgr_bytes, width, height, stride, inference_width, inference_height,
                            detections, detection_capacity, detection_count);
    if (status == KFACEAUTH_YUNET_RUNTIME_FAILURE && typedDetector->backend != Backend::Cpu &&
        !typedDetector->model.empty())
    {
        const std::vector<uint8_t> model = typedDetector->model;
        const std::vector<uint8_t> emptyConfig;
        if (typedDetector->pinnedModel)
            typedDetector->pinnedModel->unlock();
        auto cpu = createCpu(
            [&model, &emptyConfig, width, height, typedDetector](const BackendSpec &backend)
            {
                return cv::FaceDetectorYN::create("ONNX", model, emptyConfig, cv::Size(width, height),
                                                  typedDetector->scoreThreshold, typedDetector->nmsThreshold,
                                                  typedDetector->topK, backend.backendId, backend.targetId);
            },
            &typedDetector->model);
        if (!cpu.empty())
        {
            typedDetector->value = std::move(cpu);
            typedDetector->backend = Backend::Cpu;
            clearBytes(&typedDetector->model);
            status = detectOnce(typedDetector, bgr_bytes, width, height, stride, inference_width, inference_height,
                                detections, detection_capacity, detection_count);
        }
    }
    return status;
}

extern "C" void kfaceauth_yunet_destroy(void *detector)
{
    try
    {
        auto *typedDetector = static_cast<Detector *>(detector);
        if (typedDetector)
            clearBytes(&typedDetector->model);
        delete typedDetector;
    }
    catch (...)
    {
        // Destructors must never unwind across the C ABI.
    }
}

extern "C" int kfaceauth_sface_create(const uint8_t *model_bytes, size_t model_size, void **recognizer_out)
{
    if (!model_bytes || model_size != ExpectedSFaceModelBytes || !recognizer_out || *recognizer_out)
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    configureThreads();
    std::vector<uint8_t> model;
    try
    {
        model.assign(model_bytes, model_bytes + model_size);
        const std::vector<uint8_t> emptyConfig;
        Backend selected = Backend::Cpu;
        auto recognizer = createWithFallback(
            [&model, &emptyConfig](const BackendSpec &backend)
            { return cv::FaceRecognizerSF::create("ONNX", model, emptyConfig, backend.backendId, backend.targetId); },
            &selected, &model);
        if (recognizer.empty())
        {
            clearBytes(&model);
            return KFACEAUTH_YUNET_RUNTIME_FAILURE;
        }
        auto result = std::make_unique<Recognizer>();
        result->value = std::move(recognizer);
        result->backend = selected;
        if (selected != Backend::Cpu)
        {
            result->model = std::move(model);
            result->pinnedModel = std::make_unique<PinnedRegion>(result->model.data(), result->model.size());
            if (requireMemoryPinning() && !result->pinnedModel->locked())
            {
                clearBytes(&result->model);
                return KFACEAUTH_YUNET_HARDENING_FAILURE;
            }
        }
        clearBytes(&model);
        *recognizer_out = result.release();
        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        clearBytes(&model);
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}

namespace
{
int extractOnce(Recognizer *recognizer, const uint8_t *bgrBytes, int32_t width, int32_t height, size_t stride,
                const KFaceAuthYuNetDetection *detection, float *embedding, size_t embeddingCapacity,
                size_t *embeddingCount)
{
    *embeddingCount = 0;
    std::fill_n(embedding, embeddingCapacity, 0.0F);
    cv::Mat image;
    cv::Mat faceRow;
    cv::Mat aligned;
    cv::Mat feature;
    PinnedRegion pinnedInput;
    PinnedRegion pinnedFaceRow;
    PinnedRegion pinnedAligned;
    PinnedRegion pinnedFeature;
    MatClearGuard clearOnExit(&image, &faceRow, &aligned, &feature);
    try
    {
        image.create(height, width, CV_8UC3);
        for (int32_t row = 0; row < height; ++row)
        {
            const auto rowOffset = static_cast<size_t>(row) * stride;
            std::copy_n(bgrBytes + rowOffset, stride, image.ptr<uint8_t>(row));
        }
        pinnedInput = PinnedRegion(image.data, image.total() * image.elemSize());
        if (requireMemoryPinning() && !pinnedInput.locked())
            return KFACEAUTH_YUNET_HARDENING_FAILURE;

        faceRow.create(1, 15, CV_32FC1);
        std::copy_n(detection->values, 15, faceRow.ptr<float>(0));
        pinnedFaceRow = PinnedRegion(faceRow.data, faceRow.total() * faceRow.elemSize());
        if (requireMemoryPinning() && !pinnedFaceRow.locked())
            return KFACEAUTH_YUNET_HARDENING_FAILURE;

        recognizer->value->alignCrop(image, faceRow, aligned);
        if (aligned.dims != 2 || aligned.type() != CV_8UC3 || aligned.rows != KFACEAUTH_SFACE_ALIGNED_HEIGHT ||
            aligned.cols != KFACEAUTH_SFACE_ALIGNED_WIDTH)
            return KFACEAUTH_YUNET_MALFORMED_OUTPUT;
        pinnedAligned = PinnedRegion(aligned.data, aligned.total() * aligned.elemSize());
        if (requireMemoryPinning() && !pinnedAligned.locked())
            return KFACEAUTH_YUNET_HARDENING_FAILURE;

        recognizer->value->feature(aligned, feature);
        if (feature.dims != 2 || feature.type() != CV_32FC1 || feature.rows != 1 ||
            feature.cols != KFACEAUTH_SFACE_EMBEDDING_DIMENSION || !feature.isContinuous())
            return KFACEAUTH_YUNET_MALFORMED_OUTPUT;
        pinnedFeature = PinnedRegion(feature.data, feature.total() * feature.elemSize());
        if (requireMemoryPinning() && !pinnedFeature.locked())
            return KFACEAUTH_YUNET_HARDENING_FAILURE;

        const float *source = feature.ptr<float>(0);
        if (!std::all_of(source, source + KFACEAUTH_SFACE_EMBEDDING_DIMENSION,
                         [](float value) { return std::isfinite(value); }))
            return KFACEAUTH_YUNET_MALFORMED_OUTPUT;
        std::copy_n(source, KFACEAUTH_SFACE_EMBEDDING_DIMENSION, embedding);
        *embeddingCount = KFACEAUTH_SFACE_EMBEDDING_DIMENSION;
        clearMat(&image);
        clearMat(&faceRow);
        clearMat(&aligned);
        clearMat(&feature);
        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        clearMat(&image);
        clearMat(&faceRow);
        clearMat(&aligned);
        clearMat(&feature);
        std::fill_n(embedding, embeddingCapacity, 0.0F);
        *embeddingCount = 0;
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}
} // namespace

extern "C" int kfaceauth_sface_extract(void *recognizer, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width,
                                       int32_t height, size_t stride, const KFaceAuthYuNetDetection *detection,
                                       float *embedding, size_t embedding_capacity, size_t *embedding_count)
{
    if (!recognizer || !bgr_bytes || !detection || !embedding || !embedding_count ||
        !validPackedBgr(bgr_size, width, height, stride) || embedding_capacity != KFACEAUTH_SFACE_EMBEDDING_DIMENSION)
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;
    if (!std::all_of(std::begin(detection->values), std::end(detection->values),
                     [](float value) { return std::isfinite(value); }))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    auto *typedRecognizer = static_cast<Recognizer *>(recognizer);
    int status = extractOnce(typedRecognizer, bgr_bytes, width, height, stride, detection, embedding,
                             embedding_capacity, embedding_count);
    if (status == KFACEAUTH_YUNET_RUNTIME_FAILURE && typedRecognizer->backend != Backend::Cpu &&
        !typedRecognizer->model.empty())
    {
        const std::vector<uint8_t> model = typedRecognizer->model;
        const std::vector<uint8_t> emptyConfig;
        if (typedRecognizer->pinnedModel)
            typedRecognizer->pinnedModel->unlock();
        auto cpu = createCpu(
            [&model, &emptyConfig](const BackendSpec &backend)
            { return cv::FaceRecognizerSF::create("ONNX", model, emptyConfig, backend.backendId, backend.targetId); },
            &typedRecognizer->model);
        if (!cpu.empty())
        {
            typedRecognizer->value = std::move(cpu);
            typedRecognizer->backend = Backend::Cpu;
            clearBytes(&typedRecognizer->model);
            status = extractOnce(typedRecognizer, bgr_bytes, width, height, stride, detection, embedding,
                                 embedding_capacity, embedding_count);
        }
    }
    return status;
}

extern "C" int kfaceauth_sface_cosine(void *recognizer, const float *left, size_t left_count, const float *right,
                                      size_t right_count, double *similarity)
{
    if (!recognizer || !left || !right || !similarity || left_count != KFACEAUTH_SFACE_EMBEDDING_DIMENSION ||
        right_count != KFACEAUTH_SFACE_EMBEDDING_DIMENSION ||
        !std::all_of(left, left + left_count, [](float value) { return std::isfinite(value); }) ||
        !std::all_of(right, right + right_count, [](float value) { return std::isfinite(value); }))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    *similarity = 0.0;
    try
    {
        const cv::Mat leftFeature(1, KFACEAUTH_SFACE_EMBEDDING_DIMENSION, CV_32FC1, const_cast<float *>(left));
        const cv::Mat rightFeature(1, KFACEAUTH_SFACE_EMBEDDING_DIMENSION, CV_32FC1, const_cast<float *>(right));
        const double score = static_cast<Recognizer *>(recognizer)
                                 ->value->match(leftFeature, rightFeature, cv::FaceRecognizerSF::FR_COSINE);
        if (!std::isfinite(score) || score < -1.000001 || score > 1.000001)
            return KFACEAUTH_YUNET_MALFORMED_OUTPUT;
        *similarity = score;
        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        *similarity = 0.0;
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}

extern "C" int kfaceauth_sface_backend(void *recognizer)
{
    if (!recognizer)
        return KFACEAUTH_YUNET_BACKEND_CPU;
    return static_cast<int>(static_cast<Recognizer *>(recognizer)->backend);
}

extern "C" void kfaceauth_sface_destroy(void *recognizer)
{
    try
    {
        auto *typedRecognizer = static_cast<Recognizer *>(recognizer);
        if (typedRecognizer)
            clearBytes(&typedRecognizer->model);
        delete typedRecognizer;
    }
    catch (...)
    {
        // Destructors must never unwind across the C ABI.
    }
}

extern "C" int kfaceauth_estimate_head_pose(const KFaceAuthYuNetDetection *detection, int32_t image_width,
                                            int32_t image_height, KFaceAuthHeadPose *pose_out)
{
    if (!detection || !pose_out || image_width <= 0 || image_height <= 0)
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;
    if (!std::all_of(std::begin(detection->values), std::end(detection->values),
                     [](float value) { return std::isfinite(value); }))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    pose_out->yaw = 0.0F;
    pose_out->pitch = 0.0F;
    pose_out->roll = 0.0F;

    try
    {
        const std::vector<cv::Point3f> objectPoints = {
            cv::Point3f(32.0F, 33.0F, -15.0F),  cv::Point3f(-32.0F, 33.0F, -15.0F),  cv::Point3f(0.0F, 0.0F, 25.0F),
            cv::Point3f(25.0F, -42.0F, -10.0F), cv::Point3f(-25.0F, -42.0F, -10.0F),
        };

        const std::vector<cv::Point2f> imagePoints = {
            cv::Point2f(detection->values[4], detection->values[5]),
            cv::Point2f(detection->values[6], detection->values[7]),
            cv::Point2f(detection->values[8], detection->values[9]),
            cv::Point2f(detection->values[10], detection->values[11]),
            cv::Point2f(detection->values[12], detection->values[13]),
        };

        const auto focal_length = static_cast<double>(std::max(image_width, image_height));
        const cv::Point2d center(static_cast<double>(image_width) / 2.0, static_cast<double>(image_height) / 2.0);
        const cv::Mat cameraMatrix =
            (cv::Mat_<double>(3, 3) << focal_length, 0.0, center.x, 0.0, focal_length, center.y, 0.0, 0.0, 1.0);
        const cv::Mat distCoeffs = cv::Mat::zeros(4, 1, CV_64F);

        cv::Mat rvec;
        cv::Mat tvec;
        bool success =
            cv::solvePnP(objectPoints, imagePoints, cameraMatrix, distCoeffs, rvec, tvec, false, cv::SOLVEPNP_EPNP);
        if (!success)
        {
            success = cv::solvePnP(objectPoints, imagePoints, cameraMatrix, distCoeffs, rvec, tvec, false,
                                   cv::SOLVEPNP_ITERATIVE);
        }
        if (success)
        {
            cv::solvePnPRefineLM(objectPoints, imagePoints, cameraMatrix, distCoeffs, rvec, tvec);
            cv::Mat rotationMatrix;
            cv::Rodrigues(rvec, rotationMatrix);

            const double r00 = rotationMatrix.at<double>(0, 0);
            const double r10 = rotationMatrix.at<double>(1, 0);
            const double r20 = rotationMatrix.at<double>(2, 0);
            const double r21 = rotationMatrix.at<double>(2, 1);
            const double r22 = rotationMatrix.at<double>(2, 2);

            const double sy = std::sqrt(r00 * r00 + r10 * r10);
            double pitch = 0.0;
            double yaw = 0.0;
            double roll = 0.0;
            constexpr double Rad2Deg = 180.0 / 3.14159265358979323846;

            if (sy > 1e-6)
            {
                pitch = std::atan2(-r20, sy) * Rad2Deg;
                yaw = std::atan2(r10, r00) * Rad2Deg;
                roll = std::atan2(r21, r22) * Rad2Deg;
            }
            else
            {
                pitch = std::atan2(-r20, sy) * Rad2Deg;
                yaw = std::atan2(-rotationMatrix.at<double>(1, 2), rotationMatrix.at<double>(1, 1)) * Rad2Deg;
                roll = 0.0;
            }

            clearMat(&rvec);
            clearMat(&tvec);
            clearMat(&rotationMatrix);

            if (std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll))
            {
                pose_out->yaw = static_cast<float>(yaw);
                pose_out->pitch = static_cast<float>(pitch);
                pose_out->roll = static_cast<float>(roll);
                return KFACEAUTH_YUNET_OK;
            }
        }
        clearMat(&rvec);
        clearMat(&tvec);
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
    catch (...)
    {
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}

extern "C" int kfaceauth_analyze_texture(const uint8_t *bgr_bytes, size_t bgr_size, int32_t width, int32_t height,
                                         size_t stride, const KFaceAuthYuNetDetection *detection,
                                         KFaceAuthTextureMetrics *metrics_out)
{
    if (!bgr_bytes || !detection || !metrics_out || !validPackedBgr(bgr_size, width, height, stride))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;
    if (!std::all_of(std::begin(detection->values), std::end(detection->values),
                     [](float value) { return std::isfinite(value); }))
        return KFACEAUTH_YUNET_INVALID_ARGUMENT;

    metrics_out->lbp_entropy = 0.0F;
    metrics_out->moire_energy = 0.0F;

    cv::Mat image;
    cv::Mat gray;
    cv::Mat crop;
    cv::Mat floatCrop;
    cv::Mat dftResult;
    MatClearGuard guard1(&image, &gray, &crop);
    MatClearGuard guard2(&floatCrop, &dftResult, nullptr);

    try
    {
        image.create(height, width, CV_8UC3);
        for (int32_t row = 0; row < height; ++row)
        {
            const auto rowOffset = static_cast<size_t>(row) * stride;
            std::copy_n(bgr_bytes + rowOffset, stride, image.ptr<uint8_t>(row));
        }

        const float fx = detection->values[0];
        const float fy = detection->values[1];
        const float fw = detection->values[2];
        const float fh = detection->values[3];

        const int32_t x0 = std::clamp(static_cast<int32_t>(fx), 0, width - 1);
        const int32_t y0 = std::clamp(static_cast<int32_t>(fy), 0, height - 1);
        const int32_t x1 = std::clamp(static_cast<int32_t>(fx + fw), x0 + 1, width);
        const int32_t y1 = std::clamp(static_cast<int32_t>(fy + fh), y0 + 1, height);

        if (x1 - x0 < 16 || y1 - y0 < 16)
            return KFACEAUTH_YUNET_INVALID_ARGUMENT;

        const cv::Rect roi(x0, y0, x1 - x0, y1 - y0);
        cv::cvtColor(image(roi), crop, cv::COLOR_BGR2GRAY);
        cv::resize(crop, gray, cv::Size(112, 112));

        // 1. Local Binary Pattern (LBP) calculation on 112x112 gray
        std::array<uint32_t, 256> lbpHist = {0};
        const int rows = gray.rows;
        const int cols = gray.cols;
        uint32_t totalLbp = 0;

        for (int r = 1; r < rows - 1; ++r)
        {
            const uint8_t *prev = gray.ptr<uint8_t>(r - 1);
            const uint8_t *curr = gray.ptr<uint8_t>(r);
            const uint8_t *next = gray.ptr<uint8_t>(r + 1);
            for (int c = 1; c < cols - 1; ++c)
            {
                const uint8_t centerVal = curr[c];
                uint8_t code = 0;
                if (prev[c - 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 7));
                if (prev[c] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 6));
                if (prev[c + 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 5));
                if (curr[c + 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 4));
                if (next[c + 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 3));
                if (next[c] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 2));
                if (next[c - 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 1));
                if (curr[c - 1] >= centerVal)
                    code = static_cast<uint8_t>(code | static_cast<uint8_t>(1U << 0));
                lbpHist[code]++;
                totalLbp++;
            }
        }

        double entropy = 0.0;
        if (totalLbp > 0)
        {
            const auto total = static_cast<double>(totalLbp);
            for (uint32_t count : lbpHist)
            {
                if (count > 0)
                {
                    const double p = static_cast<double>(count) / total;
                    entropy -= p * (std::log(p) / std::log(2.0));
                }
            }
        }
        metrics_out->lbp_entropy = static_cast<float>(entropy);

        // 2. 2D FFT Moiré / high-frequency periodicity analysis
        gray.convertTo(floatCrop, CV_32F, 1.0 / 255.0);
        cv::dft(floatCrop, dftResult, cv::DFT_COMPLEX_OUTPUT);

        std::vector<cv::Mat> planes;
        cv::split(dftResult, planes);
        cv::Mat mag;
        cv::magnitude(planes[0], planes[1], mag);
        mag += cv::Scalar::all(1.0e-6);

        const int cx = mag.cols / 2;
        const int cy = mag.rows / 2;
        cv::Mat q0(mag, cv::Rect(0, 0, cx, cy));
        cv::Mat q1(mag, cv::Rect(cx, 0, cx, cy));
        cv::Mat q2(mag, cv::Rect(0, cy, cx, cy));
        cv::Mat q3(mag, cv::Rect(cx, cy, cx, cy));

        cv::Mat tmp;
        q0.copyTo(tmp);
        q3.copyTo(q0);
        tmp.copyTo(q3);

        q1.copyTo(tmp);
        q2.copyTo(q1);
        tmp.copyTo(q2);

        double totalEnergy = 0.0;
        double highFreqEnergy = 0.0;
        double maxHighFreqPeak = 0.0;
        int highFreqCount = 0;

        const int radiusInner = 15;
        const int radiusOuter = 50;

        for (int y = 0; y < mag.rows; ++y)
        {
            const float *magRow = mag.ptr<float>(y);
            for (int x = 0; x < mag.cols; ++x)
            {
                const auto val = static_cast<double>(magRow[x]);
                totalEnergy += val;
                const double dist = std::sqrt(static_cast<double>((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                if (dist >= radiusInner && dist <= radiusOuter)
                {
                    highFreqEnergy += val;
                    if (val > maxHighFreqPeak)
                    {
                        maxHighFreqPeak = val;
                    }
                    highFreqCount++;
                }
            }
        }

        double moireScore = 0.0;
        if (highFreqCount > 0 && highFreqEnergy > 1e-6)
        {
            const double avgHighFreq = highFreqEnergy / static_cast<double>(highFreqCount);
            const double papr = maxHighFreqPeak / avgHighFreq;
            moireScore = papr * (highFreqEnergy / (totalEnergy + 1e-6));
        }

        metrics_out->moire_energy = static_cast<float>(moireScore);

        clearMat(&tmp);
        clearMat(&mag);
        for (cv::Mat &p : planes)
            clearMat(&p);

        return KFACEAUTH_YUNET_OK;
    }
    catch (...)
    {
        return KFACEAUTH_YUNET_RUNTIME_FAILURE;
    }
}
