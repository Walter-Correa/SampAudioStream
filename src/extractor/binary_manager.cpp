#include "sampaudiostream/extractor/binary_manager.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#endif

namespace sampaudiostream {

namespace {

enum class HostArch {
    X86,
    X64,
    Arm64,
    Armv7
};

HostArch DetectHostArch() {
#ifdef _WIN32
    SYSTEM_INFO sys_info{};
    GetNativeSystemInfo(&sys_info);
    switch (sys_info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return HostArch::X64;
        case PROCESSOR_ARCHITECTURE_ARM64:
            return HostArch::Arm64;
        case PROCESSOR_ARCHITECTURE_INTEL:
        default:
            return HostArch::X86;
    }
#else
    struct utsname uts{};
    if (uname(&uts) == 0) {
        std::string machine(uts.machine);
        if (machine == "x86_64" || machine == "amd64") return HostArch::X64;
        if (machine == "aarch64" || machine == "arm64") return HostArch::Arm64;
        if (machine.rfind("armv7", 0) == 0) return HostArch::Armv7;
    }
    return sizeof(void*) == 8 ? HostArch::X64 : HostArch::X86;
#endif
}

std::string GetYtDlpFilename() {
#ifdef _WIN32
    return "yt-dlp.exe";
#else
    return "yt-dlp";
#endif
}

std::string GetJsRuntimeFilename() {
#ifdef _WIN32
    return "qjs.exe";
#else
    return "qjs";
#endif
}

std::string GetYtDlpDownloadUrl() {
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    if (arch == HostArch::X86) {
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_x86.exe";
    }
    return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
#else
    switch (arch) {
        case HostArch::Arm64:
            return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64";
        case HostArch::Armv7:
            return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_armv7l";
        case HostArch::X64:
        case HostArch::X86:
        default:
            return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux";
    }
#endif
}

std::string GetJsRuntimeDownloadUrl() {
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    if (arch == HostArch::X86) {
        return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-windows-x86.exe";
    }
    return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-windows-x86_64.exe";
#else
    switch (arch) {
        case HostArch::Arm64:
            return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-aarch64";
        case HostArch::Armv7:
            return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-armv7";
        case HostArch::X86:
            return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-x86";
        case HostArch::X64:
        default:
            return "https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-x86_64";
    }
#endif
}

} // namespace

BinaryManager::BinaryManager(const PluginConfig& config)
    : config_(config) {
    std::filesystem::path bin_dir(config_.bin_dir);
    ytdlp_path_ = (bin_dir / GetYtDlpFilename()).string();
    js_runtime_path_ = (bin_dir / GetJsRuntimeFilename()).string();
}

void BinaryManager::InitializeAsync(ThreadPool& pool) {
    bool expected = false;
    if (!initializing_.compare_exchange_strong(expected, true)) {
        return;
    }

    pool.Post([this]() {
        EnsureBinaries();
        initializing_.store(false);
    });
}

bool BinaryManager::IsReady() const {
    return ready_.load();
}

bool BinaryManager::IsInitializing() const {
    return initializing_.load();
}

std::string BinaryManager::GetYtDlpPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ytdlp_path_;
}

std::string BinaryManager::GetJsRuntimePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return js_runtime_path_;
}

void BinaryManager::EnsureBinaries() {
    std::error_code ec;
    std::filesystem::create_directories(config_.bin_dir, ec);

    if (!EnsureYtDlp()) {
        Logger::Instance().Error("Failed to provision yt-dlp extractor binary. Platform stream resolution will be unavailable.");
        ready_.store(false);
        return;
    }

    if (config_.enable_js_runtime) {
        if (!EnsureJsRuntime()) {
            Logger::Instance().Warn("Failed to provision QuickJS runtime; some YouTube streams may have limited formats.");
        }
    }

    ready_.store(true);
    Logger::Instance().Info("Audio extractor subsystem ready (Zero-Setup).");

    if (config_.auto_update_extractor) {
        UpdateYtDlpIfDue();
    }
}

bool BinaryManager::EnsureYtDlp() {
    std::string path = GetYtDlpPath();
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 100) {
        MakeExecutable(path);
        return true;
    }

    if (!config_.auto_download_extractor) {
        Logger::Instance().Error("yt-dlp binary missing and auto_download is disabled: " + path);
        return false;
    }

    std::string url = GetYtDlpDownloadUrl();
    Logger::Instance().Info("Downloading stream extractor (yt-dlp) from " + url + " ...");

    std::string err;
    if (!HttpClient::DownloadToFile(url, path, err)) {
        Logger::Instance().Error("yt-dlp download failed: " + err);
        return false;
    }

    MakeExecutable(path);
    Logger::Instance().Info("yt-dlp downloaded successfully to " + path);
    return true;
}

bool BinaryManager::EnsureJsRuntime() {
    std::string path = GetJsRuntimePath();
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 50) {
        MakeExecutable(path);
        return true;
    }

    if (!config_.auto_download_extractor) {
        return false;
    }

    std::string url = GetJsRuntimeDownloadUrl();
    Logger::Instance().Info("Downloading lightweight JS runtime (QuickJS-ng) from " + url + " ...");

    std::string err;
    if (!HttpClient::DownloadToFile(url, path, err)) {
        Logger::Instance().Warn("QuickJS-ng download failed: " + err);
        return false;
    }

    MakeExecutable(path);
    Logger::Instance().Info("QuickJS-ng downloaded successfully to " + path);
    return true;
}

void BinaryManager::UpdateYtDlpIfDue() {
    std::filesystem::path state_file = std::filesystem::path(config_.bin_dir) / "extractor_state.json";
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::int64_t last_check = 0;
    if (std::filesystem::exists(state_file)) {
        try {
            std::ifstream ifs(state_file);
            nlohmann::json j;
            ifs >> j;
            last_check = j.value("last_update_check", std::int64_t{0});
        } catch (...) {}
    }

    // Check once every 24 hours
    if (now_epoch - last_check < 86400) {
        return;
    }

    // Update timestamp first so failed network doesn't spam every boot
    try {
        std::ofstream ofs(state_file, std::ios::trunc);
        ofs << nlohmann::json{{"last_update_check", now_epoch}}.dump(2);
    } catch (...) {}

    // Check GitHub latest release tag vs stored tag or re-download if newer
    auto resp = HttpClient::Get("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest", {}, 10000);
    if (!resp.Ok()) {
        return;
    }

    try {
        auto j = nlohmann::json::parse(resp.body);
        std::string latest_tag = j.value("tag_name", "");
        if (latest_tag.empty()) return;

        std::string current_tag;
        if (std::filesystem::exists(state_file)) {
            std::ifstream ifs(state_file);
            nlohmann::json sj;
            ifs >> sj;
            current_tag = sj.value("ytdlp_tag", "");
        }

        if (current_tag != latest_tag) {
            if (!current_tag.empty()) {
                Logger::Instance().Info("New yt-dlp version available (" + latest_tag + "), updating in background...");
                std::string err;
                std::string path = GetYtDlpPath();
                if (HttpClient::DownloadToFile(GetYtDlpDownloadUrl(), path, err)) {
                    MakeExecutable(path);
                    Logger::Instance().Info("yt-dlp updated to " + latest_tag);
                }
            }
            std::ofstream ofs(state_file, std::ios::trunc);
            ofs << nlohmann::json{{"last_update_check", now_epoch}, {"ytdlp_tag", latest_tag}}.dump(2);
        }
    } catch (...) {}
}

bool BinaryManager::MakeExecutable(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    return chmod(path.c_str(), 0755) == 0;
#endif
}

} // namespace sampaudiostream
