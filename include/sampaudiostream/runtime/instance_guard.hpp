#pragma once

#include <atomic>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

class RuntimeInstanceGuard {
public:
    static bool Acquire(const char* owner_tag, std::string& out_error) {
        bool expected = false;
        if (!in_process_active_.compare_exchange_strong(expected, true)) {
            out_error = std::string("SampAudioStream is already active in this process (rejected duplicate init from ") +
                        (owner_tag ? owner_tag : "unknown") + "). Do not load both SA-MP plugin and open.mp component.";
            return false;
        }

#ifdef _WIN32
        DWORD pid = GetCurrentProcessId();
        std::wstring name = L"Local\\SampAudioStream_Instance_" + std::to_wstring(pid);
        HANDLE mtx = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mtx) {
            in_process_active_.store(false);
            out_error = "Failed to create process instance guard mutex.";
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mtx);
            in_process_active_.store(false);
            out_error = std::string("SampAudioStream is already active in this process (rejected duplicate init from ") +
                        (owner_tag ? owner_tag : "unknown") + ").";
            return false;
        }
        os_mutex_ = mtx;
#else
        std::string lock_path = "/tmp/sampaudiostream_" + std::to_string(static_cast<long>(getpid())) + ".lock";
        int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd >= 0) {
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                ::close(fd);
                in_process_active_.store(false);
                out_error = std::string("SampAudioStream is already active in this process (rejected duplicate init from ") +
                            (owner_tag ? owner_tag : "unknown") + ").";
                return false;
            }
            lock_fd_ = fd;
            lock_path_ = std::move(lock_path);
        }
#endif
        return true;
    }

    static void Release() {
#ifdef _WIN32
        if (os_mutex_) {
            CloseHandle(os_mutex_);
            os_mutex_ = nullptr;
        }
#else
        if (lock_fd_ >= 0) {
            ::flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
            if (!lock_path_.empty()) {
                ::unlink(lock_path_.c_str());
                lock_path_.clear();
            }
        }
#endif
        in_process_active_.store(false);
    }

private:
    static inline std::atomic<bool> in_process_active_{false};
#ifdef _WIN32
    static inline HANDLE os_mutex_{nullptr};
#else
    static inline int lock_fd_{-1};
    static inline std::string lock_path_;
#endif
};

} // namespace sampaudiostream
