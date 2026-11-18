#include "sampaudiostream/extractor/binary_manager.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

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

std::string GetYtDlpAssetName() {
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    return (arch == HostArch::X86) ? "yt-dlp_x86.exe" : "yt-dlp.exe";
#else
    switch (arch) {
        case HostArch::Arm64:
            return "yt-dlp_linux_aarch64";
        case HostArch::Armv7:
            return "yt-dlp_linux_armv7l";
        case HostArch::X64:
        case HostArch::X86:
        default:
            return "yt-dlp_linux";
    }
#endif
}

std::string GetYtDlpOnedirZipAssetName() {
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    return (arch == HostArch::X86) ? "yt-dlp_win_x86.zip" : "yt-dlp_win.zip";
#else
    switch (arch) {
        case HostArch::Arm64:
            return "yt-dlp_linux_aarch64.zip";
        case HostArch::Armv7:
            return "yt-dlp_linux_armv7l.zip";
        case HostArch::X64:
        case HostArch::X86:
        default:
            return "yt-dlp_linux.zip";
    }
#endif
}

std::string FindSystemZipExtractor(std::vector<std::string>& out_prefix_args, bool& out_is_python_zipfile) {
    out_prefix_args.clear();
    out_is_python_zipfile = false;
#ifdef _WIN32
    wchar_t sys_dir[MAX_PATH]{};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path tar_path = std::filesystem::path(sys_dir) / L"tar.exe";
        if (std::filesystem::exists(tar_path)) {
            return tar_path.string();
        }
    }
    return "";
#else
    for (const char* p : {"/usr/bin/unzip", "/bin/unzip"}) {
        if (std::filesystem::exists(p)) {
            out_prefix_args = {"-o", "-q"};
            return p;
        }
    }
    for (const char* p : {"/usr/bin/bsdtar", "/usr/bin/tar", "/bin/tar"}) {
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    for (const char* p : {"/usr/bin/python3", "/bin/python3"}) {
        if (std::filesystem::exists(p)) {
            out_is_python_zipfile = true;
            return p;
        }
    }
    return "";
#endif
}

bool ExtractZipArchive(const std::string& zip_path, const std::string& dest_dir, const std::atomic<bool>* cancel_flag, std::string& out_error) {
    std::vector<std::string> prefix;
    bool is_python = false;
    std::string tool = FindSystemZipExtractor(prefix, is_python);
    if (tool.empty()) {
        out_error = "No system zip extractor found";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    std::vector<std::string> args;
    if (is_python) {
        args = {"-m", "zipfile", "-e", zip_path, dest_dir};
    } else if (!prefix.empty()) {
        // unzip -o -q <zip> -d <dest>
        args = prefix;
        args.push_back(zip_path);
        args.push_back("-d");
        args.push_back(dest_dir);
    } else {
        // bsdtar / Windows tar.exe: tar -xf <zip> -C <dest>
        args = {"-xf", zip_path, "-C", dest_dir};
    }
    std::string out_stdout, out_stderr;
    if (!ExecuteSubprocess(tool, args, out_stdout, out_stderr, 60000, cancel_flag, nullptr)) {
        out_error = out_stderr.empty() ? "Archive extraction failed" : out_stderr;
        return false;
    }
    return true;
}

std::string FindExtractedYtDlpExe(const std::filesystem::path& dir) {
#ifdef _WIN32
    const char* candidates[] = {"yt-dlp.exe", "yt-dlp_x86.exe"};
#else
    const char* candidates[] = {"yt-dlp_linux", "yt-dlp_linux_aarch64", "yt-dlp_linux_armv7l", "yt-dlp"};
#endif
    for (const char* name : candidates) {
        std::filesystem::path p = dir / name;
        if (std::filesystem::exists(p) && std::filesystem::file_size(p) > 100 * 1024) {
            return p.string();
        }
    }
    return "";
}

std::string GetYtDlpDownloadUrlForTag(const std::string& tag, const std::string& asset_name) {
    return "https://github.com/yt-dlp/yt-dlp/releases/download/" + tag + "/" + asset_name;
}

std::string GetYtDlpChecksumsUrlForTag(const std::string& tag) {
    return "https://github.com/yt-dlp/yt-dlp/releases/download/" + tag + "/SHA2-256SUMS";
}

// Standalone RFC 6234 SHA-256 hasher (zero extra platform library requirements)
class Sha256Hasher {
public:
    Sha256Hasher()
        : H_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

    void Update(const void* data, std::size_t size) {
        if (!data || size == 0) return;
        total_bits_ += static_cast<std::uint64_t>(size) * 8ULL;
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::size_t rem = size;
        while (rem > 0) {
            std::size_t take = std::min<std::size_t>(64 - block_len_, rem);
            std::memcpy(block_.data() + block_len_, p, take);
            block_len_ += take;
            p += take;
            rem -= take;
            if (block_len_ == 64) {
                Transform(block_.data());
                block_len_ = 0;
            }
        }
    }

    std::string FinalizeHex() {
        block_[block_len_++] = 0x80;
        if (block_len_ > 56) {
            while (block_len_ < 64) block_[block_len_++] = 0;
            Transform(block_.data());
            block_len_ = 0;
        }
        while (block_len_ < 56) block_[block_len_++] = 0;
        for (int i = 0; i < 8; ++i) {
            block_[56 + i] = static_cast<std::uint8_t>((total_bits_ >> ((7 - i) * 8)) & 0xFF);
        }
        Transform(block_.data());

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (std::uint32_t word : H_) {
            oss << std::setw(8) << word;
        }
        return oss.str();
    }

private:
    static std::uint32_t Rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void Transform(const std::uint8_t block[64]) {
        static constexpr std::array<std::uint32_t, 64> K = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = H_[0], b = H_[1], c = H_[2], d = H_[3], e = H_[4], f = H_[5], g = H_[6], h = H_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            std::uint32_t ch = (e & f) ^ ((~e) & g);
            std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        H_[0] += a; H_[1] += b; H_[2] += c; H_[3] += d;
        H_[4] += e; H_[5] += f; H_[6] += g; H_[7] += h;
    }

    std::array<std::uint32_t, 8> H_{};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_len_{0};
    std::uint64_t total_bits_{0};
};

bool VerifyYtDlpChecksum(
    const std::string& downloaded_path,
    const std::string& tag,
    const std::string& asset_name,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error) {
    if (tag.empty()) {
        out_error = "Missing release tag for yt-dlp integrity verification";
        return false;
    }
    auto resp = HttpClient::Get(
        GetYtDlpChecksumsUrlForTag(tag),
        {},
        15000,
        cancel_flag);
    if (!resp.Ok() || resp.body.empty()) {
        out_error = "Failed to download yt-dlp SHA2-256SUMS for integrity verification";
        return false;
    }

    std::string expected_hash;
    std::istringstream iss(resp.body);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream lss(line);
        std::string hash, name;
        if (lss >> hash >> name) {
            if (!name.empty() && name.front() == '*') name.erase(name.begin());
            if (name == asset_name) {
                for (char& ch : hash) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                expected_hash = std::move(hash);
                break;
            }
        }
    }
    if (expected_hash.size() != 64) {
        out_error = "Asset checksum entry not found in SHA2-256SUMS for " + asset_name;
        return false;
    }

    std::string actual_hash = ComputeFileSha256Hex(downloaded_path);
    if (actual_hash != expected_hash) {
        out_error = "SHA-256 checksum mismatch for " + asset_name;
        return false;
    }
    return true;
}

struct JsRuntimeAsset {
    const char* name;
    const char* url;
    const char* sha256;
};

constexpr const char* kPinnedQuickJsVersion = "v0.18.0";
constexpr const char* kPinnedDenoVersion = "v2.9.8";

struct DenoRuntimeAsset {
    bool supported{false};
    const char* name{nullptr};
    const char* url{nullptr};
    const char* sha256{nullptr};
};

std::string GetDenoFilename() {
#ifdef _WIN32
    return "deno.exe";
#else
    return "deno";
#endif
}

DenoRuntimeAsset GetDenoRuntimeAsset(int supported_override) {
    if (supported_override == 0) {
        return {};
    }
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    if (arch != HostArch::X64 && supported_override != 1) {
        return {};
    }
    return {
        true,
        "deno-x86_64-pc-windows-msvc.zip",
        "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip",
        "cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf"
    };
#else
    switch (arch) {
        case HostArch::X64:
            return {
                true,
                "deno-x86_64-unknown-linux-gnu.zip",
                "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-unknown-linux-gnu.zip",
                "e890975bc2549807bbcc0b584ffafda72cc7c93349dd99c80483dd784f6c4609"
            };
        case HostArch::Arm64:
            return {
                true,
                "deno-aarch64-unknown-linux-gnu.zip",
                "https://github.com/denoland/deno/releases/download/v2.9.8/deno-aarch64-unknown-linux-gnu.zip",
                "d49596c20b9788cf8f5d8ddf644103d4bde56ed239753f0bdb92ba9361d51905"
            };
        default:
            if (supported_override == 1) {
                return {
                    true,
                    "deno-x86_64-unknown-linux-gnu.zip",
                    "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-unknown-linux-gnu.zip",
                    "e890975bc2549807bbcc0b584ffafda72cc7c93349dd99c80483dd784f6c4609"
                };
            }
            return {};
    }
#endif
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string StripLeadingV(std::string s) {
    s = TrimAscii(std::move(s));
    while (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.erase(s.begin());
    }
    return s;
}

std::string ParseDenoVersionOutput(const std::string& raw_out) {
    std::istringstream iss(raw_out);
    std::string first_line;
    if (!std::getline(iss, first_line)) {
        return "";
    }
    std::istringstream ls(first_line);
    std::string tok1, tok2;
    ls >> tok1 >> tok2;
    std::string candidate = StripLeadingV((tok1 == "deno" && !tok2.empty()) ? tok2 : tok1);
    if (candidate.empty() || !std::isdigit(static_cast<unsigned char>(candidate.front()))) {
        return "";
    }
    if (candidate.size() > 32) {
        candidate.resize(32);
    }
    return candidate;
}

JsRuntimeAsset GetJsRuntimeAsset() {
    HostArch arch = DetectHostArch();
#ifdef _WIN32
    if (arch == HostArch::X86) {
        return {
            "qjs-windows-x86.exe",
            "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-windows-x86.exe",
            "79aa85a8208596dc04333e99726c254d43b9e814803f2100ea593341f2c95b0e"
        };
    }
    return {
        "qjs-windows-x86_64.exe",
        "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-windows-x86_64.exe",
        "ff03398d74ea6e6bd3a8e58058f7003225abcac229d7d57775b7aaae812a49c7"
    };
#else
    switch (arch) {
        case HostArch::Arm64:
            return {
                "qjs-linux-aarch64",
                "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-linux-aarch64",
                "2c005c7171424a598d31c53c8dc4a0ae2e4958eb6fbb37908c885a307c78187c"
            };
        case HostArch::Armv7:
            return {
                "qjs-linux-armv7",
                "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-linux-armv7",
                "4bbc426083b4a31f35cbb9471ce6138a8d8db8efdfce15b596597ad60ddd79ac"
            };
        case HostArch::X86:
            return {
                "qjs-linux-x86",
                "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-linux-x86",
                "6e4e46849f6450a0afc572b8d10a0178d70040314f39669ca98ca8e95f114648"
            };
        case HostArch::X64:
        default:
            return {
                "qjs-linux-x86_64",
                "https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/qjs-linux-x86_64",
                "85ded23b9fabe8c1644fc40a8d2bbdd71157affcbd807359c618cc717ee24d7d"
            };
    }
#endif
}

nlohmann::json LoadJsonObjectOrEmpty(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return nlohmann::json::object();
    }
    try {
        std::ifstream ifs(path);
        nlohmann::json j;
        ifs >> j;
        if (j.is_object()) {
            return j;
        }
    } catch (...) {
    }
    return nlohmann::json::object();
}

std::string QueryLatestYtDlpTag(const std::atomic<bool>* cancel_flag, std::string& out_error) {
    auto resp = HttpClient::Get("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest", {}, 10000, cancel_flag);
    if (!resp.Ok()) {
        out_error = resp.error.empty() ? ("GitHub API HTTP " + std::to_string(resp.status_code)) : resp.error;
        return "";
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        std::string tag = j.value("tag_name", "");
        if (tag.empty()) {
            out_error = "GitHub release JSON missing tag_name";
        }
        return tag;
    } catch (const std::exception& ex) {
        out_error = std::string("Failed to parse GitHub release JSON: ") + ex.what();
        return "";
    }
}

void SaveJsonObject(const std::filesystem::path& path, const nlohmann::json& j) {
    try {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << j.dump(2);
    } catch (...) {
    }
}

} // namespace

std::string ComputeStringSha256Hex(const std::string& input) {
    Sha256Hasher hasher;
    hasher.Update(input.data(), input.size());
    return hasher.FinalizeHex();
}

std::string ComputeFileSha256Hex(const std::string& file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) return "";
    Sha256Hasher hasher;
    std::array<char, 8192> rbuf{};
    while (ifs.good()) {
        ifs.read(rbuf.data(), static_cast<std::streamsize>(rbuf.size()));
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;
        hasher.Update(rbuf.data(), static_cast<std::size_t>(got));
    }
    return hasher.FinalizeHex();
}

YtDlpUpdateAction DecideYtDlpUpdateAction(const YtDlpUpdateDecisionInput& input) {
    if (input.last_check_epoch > 0 && (input.now_epoch - input.last_check_epoch) < 86400) {
        return YtDlpUpdateAction::SkipRecentlyChecked;
    }
    if (input.latest_tag.empty()) {
        return YtDlpUpdateAction::CheckLatestVersion;
    }
    if (input.current_tag != input.latest_tag) {
        return YtDlpUpdateAction::DownloadAndUpdate;
    }
    return YtDlpUpdateAction::UpToDate;
}

BinaryManager::BinaryManager(const PluginConfig& config)
    : config_(config) {
    std::filesystem::path bin_dir(config_.bin_dir);
    std::string onedir_exe = FindExtractedYtDlpExe(bin_dir / "yt-dlp-onedir");
    if (!onedir_exe.empty()) {
        ytdlp_path_ = std::move(onedir_exe);
    } else {
        ytdlp_path_ = (bin_dir / GetYtDlpFilename()).string();
    }
    js_runtime_path_ = (bin_dir / GetJsRuntimeFilename()).string();
    deno_path_ = (bin_dir / "deno" / GetDenoFilename()).string();
}

void BinaryManager::InitializeAsync(ThreadPool& pool, std::function<void(bool ready)> on_settled) {
    bool expected = false;
    if (!initializing_.compare_exchange_strong(expected, true)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(settle_mutex_);
        settled_ = false;
        on_settled_ = std::move(on_settled);
    }
    {
        std::lock_guard<std::mutex> lock(bg_mutex_);
        bg_done_ = false;
    }
    provisioning_failed_.store(false);
    deno_healthy_.store(false);
    deno_disabled_for_session_.store(false);
    consecutive_deno_failures_.store(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        deno_version_.clear();
        deno_unavailable_warned_ = false;
    }

    last_periodic_check_ = std::chrono::steady_clock::now();
    cancel_flag_.store(false);
    if (!pool.Post([this]() {
            if (!cancel_flag_.load()) {
                EnsureBinaries();
            } else {
                MarkSettled();
                MarkBackgroundDone();
            }
            initializing_.store(false);
        })) {
        MarkSettled();
        MarkBackgroundDone();
        initializing_.store(false);
    }
}

void BinaryManager::MaybePeriodicUpdate(ThreadPool& pool, std::uint32_t active_extractors) {
    if (!config_.auto_update_extractor || !ready_.load() || initializing_.load() ||
        updating_.load() || active_extractors > 0 || cancel_flag_.load()) {
        return;
    }
#ifndef _WIN32
    {
        HostArch arch = DetectHostArch();
        if (arch == HostArch::X86 || arch == HostArch::Armv7) {
            return;
        }
    }
#endif

    auto now = std::chrono::steady_clock::now();
    if (last_periodic_check_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::hours>(now - last_periodic_check_).count() < 24) {
        return;
    }
    last_periodic_check_ = now;

    bool expected = false;
    if (!updating_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!pool.Post([this]() {
            if (!cancel_flag_.load()) {
                UpdateYtDlpIfDue();
            }
            updating_.store(false);
        })) {
        updating_.store(false);
    }
}

void BinaryManager::Cancel() {
    cancel_flag_.store(true);
    settled_cv_.notify_all();
    bg_cv_.notify_all();
}

bool BinaryManager::IsReady() const {
    return ready_.load();
}

bool BinaryManager::IsInitializing() const {
    return initializing_.load();
}

bool BinaryManager::IsSettled() const {
    std::lock_guard<std::mutex> lock(settle_mutex_);
    return settled_;
}

bool BinaryManager::HasProvisioningFailed() const {
    return provisioning_failed_.load();
}

bool BinaryManager::NeedsInitialDownload() const {
    if (!config_.auto_download_extractor) {
        return false;
    }
    std::error_code ec;
    const std::string ytdlp = GetYtDlpPath();
    const auto ytdlp_size = std::filesystem::file_size(ytdlp, ec);
    if (ec || ytdlp_size <= 1024ULL * 100ULL) {
        return true;
    }
    if (config_.enable_js_runtime) {
        ec.clear();
        const std::string qjs = GetJsRuntimePath();
        const auto qjs_size = std::filesystem::file_size(qjs, ec);
        if (ec || qjs_size <= 1024ULL * 50ULL) {
            return true;
        }
    }
    return false;
}

bool BinaryManager::WaitUntilSettled(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(settle_mutex_);
    settled_cv_.wait_for(lock, timeout, [this]() {
        return settled_ || cancel_flag_.load();
    });
    return settled_;
}

void BinaryManager::MarkSettled() {
    std::function<void(bool)> cb;
    {
        std::lock_guard<std::mutex> lock(settle_mutex_);
        if (settled_) {
            return;
        }
        settled_ = true;
        cb = on_settled_;
    }
    settled_cv_.notify_all();
    if (cb) {
        cb(ready_.load());
    }
}

void BinaryManager::MarkBackgroundDone() {
    {
        std::lock_guard<std::mutex> lock(bg_mutex_);
        bg_done_ = true;
    }
    bg_cv_.notify_all();
}

bool BinaryManager::WaitForBackgroundProvisioningForTesting(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(bg_mutex_);
    bg_cv_.wait_for(lock, timeout, [this]() {
        return bg_done_ || cancel_flag_.load();
    });
    return bg_done_;
}

std::string BinaryManager::GetYtDlpPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ytdlp_path_;
}

std::string BinaryManager::GetJsRuntimePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return js_runtime_path_;
}

std::string BinaryManager::GetDenoPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deno_path_;
}

std::string BinaryManager::GetDenoCacheDir() const {
    return (std::filesystem::path(config_.bin_dir) / "deno-cache").string();
}

std::string BinaryManager::GetDenoVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deno_version_;
}

bool BinaryManager::IsDenoSupported() const {
    return GetDenoRuntimeAsset(deno_supported_override_.load()).supported;
}

bool BinaryManager::IsDenoReady() const {
    return deno_healthy_.load() && !deno_disabled_for_session_.load();
}

void BinaryManager::SetDenoSupportedOverrideForTesting(int supported_state) {
    deno_supported_override_.store(supported_state);
}

void BinaryManager::SetPostDownloadRetryDelayMsForTesting(int delay_ms) {
    post_download_retry_delay_ms_override_.store(delay_ms);
}

void BinaryManager::SetDenoDownloadHookForTesting(DenoDownloadTestHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    deno_download_hook_ = std::move(hook);
}

void BinaryManager::SetDenoHealthCheckHookForTesting(DenoHealthTestHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    deno_health_hook_ = std::move(hook);
}

ActiveJsRuntime BinaryManager::SelectJsRuntime() {
    ActiveJsRuntime out;
    if (!config_.enable_js_runtime) {
        return out;
    }

    std::string req = config_.js_runtime;
    for (char& ch : req) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    const std::string deno_p = GetDenoPath();
    const std::string qjs_p = GetJsRuntimePath();
    const bool qjs_exists = !qjs_p.empty() && std::filesystem::exists(qjs_p);

    if (req == "quickjs") {
        if (qjs_exists) {
            out.kind = JsRuntimeKind::QuickJs;
            out.path = qjs_p;
        }
        return out;
    }

    if (deno_healthy_.load() && !deno_disabled_for_session_.load() &&
        !deno_p.empty() && std::filesystem::exists(deno_p)) {
        out.kind = JsRuntimeKind::Deno;
        out.path = deno_p;
        out.version = GetDenoVersion();
        return out;
    }

    if (req == "deno") {
        bool warn_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!deno_unavailable_warned_) {
                deno_unavailable_warned_ = true;
                warn_now = true;
            }
        }
        if (warn_now) {
            Logger::Instance().Warn(
                IsDenoSupported()
                    ? "Configured extractor.js_runtime='deno' is currently unavailable; using QuickJS fallback."
                    : "Configured extractor.js_runtime='deno' is not supported on this host architecture; using QuickJS fallback.");
        }
    }

    if (qjs_exists) {
        out.kind = JsRuntimeKind::QuickJs;
        out.path = qjs_p;
    }
    return out;
}

std::string BinaryManager::GetActiveJsRuntimeLabel() {
    ActiveJsRuntime rt = SelectJsRuntime();
    if (rt.kind == JsRuntimeKind::Deno) {
        return rt.version.empty() ? "deno" : ("deno " + rt.version);
    }
    if (rt.kind == JsRuntimeKind::QuickJs) {
        return "quickjs";
    }
    return "none";
}

void BinaryManager::RecordDenoSuccess() {
    consecutive_deno_failures_.store(0);
}

void BinaryManager::RecordDenoRuntimeFailure() {
    int fails = consecutive_deno_failures_.fetch_add(1) + 1;
    if (fails >= 3 && !deno_disabled_for_session_.exchange(true)) {
        Logger::Instance().Warn(
            "Disabling Deno JS runtime for the remainder of this session after 3 consecutive runtime failures; using QuickJS.");
    }
}

void BinaryManager::EnsureBinaries() {
    struct BgGuard {
        BinaryManager* self;
        ~BgGuard() { self->MarkBackgroundDone(); }
    } bg_guard{this};

    std::error_code ec;
    std::filesystem::create_directories(config_.bin_dir, ec);

    if (!EnsureYtDlp()) {
        Logger::Instance().Error("Failed to provision yt-dlp extractor binary. Platform stream resolution will be unavailable.");
        ready_.store(false);
        provisioning_failed_.store(true);
        MarkSettled();
        return;
    }

    if (config_.enable_js_runtime) {
        if (!EnsureJsRuntime()) {
            Logger::Instance().Warn("Failed to provision QuickJS runtime; some YouTube streams may have limited formats.");
        }
        // If Deno is already installed from a prior run (and didn't fail health check for this pinned version),
        // run a fast health check before settling so startup logs reflect the active runtime accurately.
        std::string req = config_.js_runtime;
        for (char& ch : req) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::string deno_p = GetDenoPath();
        if (req != "quickjs" && IsDenoSupported() && !deno_p.empty() && std::filesystem::exists(deno_p)) {
            std::filesystem::path state_file = std::filesystem::path(config_.bin_dir) / "extractor_state.json";
            nlohmann::json state = LoadJsonObjectOrEmpty(state_file);
            if (state.value("deno_failed_version", "") != kPinnedDenoVersion) {
                ProbeDenoHealth(state_file.string(), 3000);
            }
        }
    }

    provisioning_failed_.store(false);
    ready_.store(true);
    bool has_callback = false;
    {
        std::lock_guard<std::mutex> lock(settle_mutex_);
        has_callback = static_cast<bool>(on_settled_);
    }
    if (!has_callback && config_.log_extractor_status) {
        Logger::Instance().Info(
            "Audio extractor subsystem ready (JS runtime: " + GetActiveJsRuntimeLabel() + ").");
    }
    MarkSettled();

    if (config_.enable_js_runtime && !cancel_flag_.load()) {
        EnsureDenoBackground();
    }

    if (config_.auto_update_extractor && !cancel_flag_.load()) {
        UpdateYtDlpIfDue();
    }
}

bool BinaryManager::DownloadAndInstallYtDlpTag(const std::string& tag, bool is_update, std::string& out_installed_path) {
    std::filesystem::path bin_dir(config_.bin_dir);
    std::string err;

    // Prefer official onedir archive (avoids PyInstaller ~1-1.5s self-extraction delay per call)
    // when a system zip extractor exists, falling back cleanly to the single-file asset.
    std::vector<std::string> dummy_prefix;
    bool dummy_py = false;
    bool has_zip_tool = !FindSystemZipExtractor(dummy_prefix, dummy_py).empty();
    if (has_zip_tool && !cancel_flag_.load()) {
        std::string zip_asset = GetYtDlpOnedirZipAssetName();
        std::string zip_url = GetYtDlpDownloadUrlForTag(tag, zip_asset);
        if (config_.log_extractor_status) {
            Logger::Instance().Info(
                std::string(is_update ? "Downloading yt-dlp update (" : "Downloading stream extractor (yt-dlp ") +
                tag + ") onedir archive from " + zip_url + " ...");
        }
        std::filesystem::path zip_stage = bin_dir / (zip_asset + ".stage");
        std::filesystem::path extract_stage = bin_dir / "yt-dlp-onedir.stage";
        std::filesystem::path target_dir = bin_dir / "yt-dlp-onedir";
        std::filesystem::path bak_dir = bin_dir / "yt-dlp-onedir.bak";
        std::error_code ec;
        std::filesystem::remove_all(extract_stage, ec);

        if (HttpClient::DownloadToFile(zip_url, zip_stage.string(), err, {}, 120000, &cancel_flag_)) {
            if (VerifyYtDlpChecksum(zip_stage.string(), tag, zip_asset, &cancel_flag_, err)) {
                if (ExtractZipArchive(zip_stage.string(), extract_stage.string(), &cancel_flag_, err)) {
                    std::filesystem::remove(zip_stage, ec);
                    std::string exe_rel = FindExtractedYtDlpExe(extract_stage);
                    if (!exe_rel.empty()) {
                        std::string exe_name = std::filesystem::path(exe_rel).filename().string();
                        bool swapped = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            bool prev_ready = ready_.exchange(false);
                            std::filesystem::remove_all(bak_dir, ec);
                            ec.clear();
                            bool moved_old = false;
                            if (std::filesystem::exists(target_dir)) {
                                std::filesystem::rename(target_dir, bak_dir, ec);
                                moved_old = !ec;
                            }
                            if (!ec || !std::filesystem::exists(target_dir)) {
                                ec.clear();
                                std::filesystem::rename(extract_stage, target_dir, ec);
                                if (!ec) {
                                    swapped = true;
                                    ytdlp_path_ = (target_dir / exe_name).string();
                                    MakeExecutable(ytdlp_path_);
                                    out_installed_path = ytdlp_path_;
                                    if (moved_old) {
                                        std::filesystem::remove_all(bak_dir, ec);
                                    }
                                } else if (moved_old) {
                                    std::filesystem::rename(bak_dir, target_dir, ec);
                                }
                            }
                            ready_.store(prev_ready);
                        }
                        if (swapped) {
                            return true;
                        }
                        Logger::Instance().Warn("Failed to swap extracted yt-dlp onedir directory; falling back to single-file binary.");
                    } else {
                        Logger::Instance().Warn("Extracted yt-dlp archive missing executable; falling back to single-file binary.");
                    }
                } else {
                    std::filesystem::remove(zip_stage, ec);
                    Logger::Instance().Warn("yt-dlp onedir extraction failed (" + err + "); falling back to single-file binary.");
                }
            } else {
                std::filesystem::remove(zip_stage, ec);
                Logger::Instance().Warn("yt-dlp onedir checksum verification failed (" + err + "); skipping update.");
                return false;
            }
        } else {
            std::filesystem::remove(zip_stage, ec);
        }
        std::filesystem::remove_all(extract_stage, ec);
    }

#ifndef _WIN32
    {
        HostArch arch = DetectHostArch();
        if (arch == HostArch::X86 || arch == HostArch::Armv7) {
            Logger::Instance().Error(
                "yt-dlp does not provide a standalone single-file binary for this 32-bit Linux architecture; "
                "auto-download disabled. Install a compatible yt-dlp binary manually into " +
                (bin_dir / GetYtDlpFilename()).string() + " or run on a 64-bit Linux kernel.");
            return false;
        }
    }
#endif

    if (cancel_flag_.load()) return false;
    std::string single_path = (bin_dir / GetYtDlpFilename()).string();
    std::string asset_name = GetYtDlpAssetName();
    std::string url = GetYtDlpDownloadUrlForTag(tag, asset_name);
    if (config_.log_extractor_status) {
        Logger::Instance().Info(
            std::string(is_update ? "Downloading yt-dlp update (" : "Downloading stream extractor (yt-dlp ") +
            tag + ") from " + url + " ...");
    }

    std::string stage_path = single_path + ".stage";
    if (!HttpClient::DownloadToFile(url, stage_path, err, {}, 120000, &cancel_flag_)) {
        if (!is_update) {
            Logger::Instance().Error("yt-dlp download failed: " + err);
        }
        return false;
    }

    if (!VerifyYtDlpChecksum(stage_path, tag, asset_name, &cancel_flag_, err)) {
        std::error_code ec;
        std::filesystem::remove(stage_path, ec);
        if (is_update) {
            Logger::Instance().Warn("Skipped yt-dlp update due to checksum verification failure: " + err);
        } else {
            Logger::Instance().Error("yt-dlp integrity verification failed: " + err);
        }
        return false;
    }

    bool swapped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool prev_ready = ready_.exchange(false);
        std::string bak_path = single_path + ".bak";
        std::error_code ec;
        std::filesystem::remove(bak_path, ec);
        ec.clear();
        bool moved_old = false;
        if (std::filesystem::exists(single_path)) {
            std::filesystem::rename(single_path, bak_path, ec);
            moved_old = !ec;
        }
        if (!ec || !std::filesystem::exists(single_path)) {
            ec.clear();
            std::filesystem::rename(stage_path, single_path, ec);
            if (!ec) {
                swapped = true;
                ytdlp_path_ = single_path;
                MakeExecutable(ytdlp_path_);
                out_installed_path = ytdlp_path_;
                if (moved_old) {
                    std::filesystem::remove(bak_path, ec);
                }
            } else {
                if (moved_old) {
                    std::filesystem::rename(bak_path, single_path, ec);
                }
                std::filesystem::remove(stage_path, ec);
            }
        } else {
            std::filesystem::remove(stage_path, ec);
        }
        ready_.store(prev_ready);
    }
    if (!swapped) {
        Logger::Instance().Warn("Failed to install verified yt-dlp binary.");
    }
    return swapped;
}

bool BinaryManager::EnsureYtDlp() {
    std::string path = GetYtDlpPath();
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 100) {
        MakeExecutable(path);
        return true;
    }

    if (!config_.auto_download_extractor) {
        Logger::Instance().Error("yt-dlp binary missing and extractor.auto_download is disabled: " + path);
        return false;
    }

    if (cancel_flag_.load()) return false;
    std::string err;
    std::string tag = QueryLatestYtDlpTag(&cancel_flag_, err);
    if (tag.empty()) {
        Logger::Instance().Error("Failed to query latest yt-dlp release tag: " + err);
        return false;
    }

    std::string installed;
    if (!DownloadAndInstallYtDlpTag(tag, false, installed)) {
        return false;
    }

    std::filesystem::path state_file = std::filesystem::path(config_.bin_dir) / "extractor_state.json";
    nlohmann::json state = LoadJsonObjectOrEmpty(state_file);
    state["ytdlp_tag"] = tag;
    state["last_update_check"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    SaveJsonObject(state_file, state);

    if (config_.log_extractor_status) {
        Logger::Instance().Info("yt-dlp " + tag + " downloaded and SHA-256 verified at " + installed);
    }
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

    if (cancel_flag_.load()) return false;
    JsRuntimeAsset asset = GetJsRuntimeAsset();
    if (config_.log_extractor_status) {
        Logger::Instance().Info(
            std::string("Downloading pinned JS runtime (QuickJS-ng ") + kPinnedQuickJsVersion +
            ") from " + asset.url + " ...");
    }

    std::string err;
    std::string stage_path = path + ".stage";
    if (!HttpClient::DownloadToFile(asset.url, stage_path, err, {}, 120000, &cancel_flag_)) {
        std::error_code ec;
        std::filesystem::remove(stage_path, ec);
        Logger::Instance().Warn("QuickJS-ng download failed: " + err);
        return false;
    }

    std::string actual_hash = ComputeFileSha256Hex(stage_path);
    if (actual_hash != asset.sha256) {
        std::error_code ec;
        std::filesystem::remove(stage_path, ec);
        Logger::Instance().Error(
            std::string("QuickJS-ng SHA-256 verification failed for ") + asset.name +
            " (expected " + asset.sha256 + ", got " + actual_hash + "); refusing to install.");
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(stage_path, path, ec);
    if (ec) {
        std::filesystem::remove(stage_path, ec);
        Logger::Instance().Warn("Failed to install verified QuickJS-ng binary: " + ec.message());
        return false;
    }

    MakeExecutable(path);
    if (config_.log_extractor_status) {
        Logger::Instance().Info("QuickJS-ng downloaded and SHA-256 verified at " + path);
    }
    return true;
}

bool BinaryManager::ProbeDenoHealth(const std::string& state_file_path, int timeout_ms, bool* out_timed_out) {
    if (out_timed_out) {
        *out_timed_out = false;
    }
    const std::string deno_p = GetDenoPath();
    if (deno_p.empty() || !std::filesystem::exists(deno_p)) {
        deno_healthy_.store(false);
        return false;
    }

    DenoHealthTestHook health_hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        health_hook = deno_health_hook_;
    }

    std::string out_ver;
    std::string out_err;
    std::string stdout_s, stderr_s;
    bool ok = false;
    if (health_hook) {
        ok = health_hook(deno_p, out_ver, out_err);
    } else {
        if (ExecuteSubprocess(deno_p, {"--version"}, stdout_s, stderr_s, timeout_ms, &cancel_flag_, nullptr)) {
            out_ver = ParseDenoVersionOutput(stdout_s);
            ok = !out_ver.empty();
            if (!ok) {
                out_err = "Unrecognized deno --version output";
            }
        } else {
            out_err = stderr_s.empty() ? "deno --version exited with error" : TrimAscii(stderr_s.substr(0, 200));
        }
    }

    if (cancel_flag_.load()) {
        return false;
    }

    std::filesystem::path state_path(state_file_path);
    nlohmann::json state = LoadJsonObjectOrEmpty(state_path);

    if (ok) {
        out_ver = StripLeadingV(out_ver);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deno_version_ = out_ver;
        }
        deno_healthy_.store(true);
        if (state.contains("deno_failed_version") || state.contains("deno_failed_reason")) {
            state.erase("deno_failed_version");
            state.erase("deno_failed_reason");
            SaveJsonObject(state_path, state);
        }
        return true;
    }

    deno_healthy_.store(false);
    auto contains_ci = [](const std::string& hay, const char* needle) {
        std::string lower = hay;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return lower.find(needle) != std::string::npos;
    };
    const bool timed_out =
        stderr_s == "Extractor timed out" ||
        contains_ci(out_err, "timed out") ||
        contains_ci(out_err, "timeout");
    if (out_timed_out) {
        *out_timed_out = timed_out;
    }
    if (timed_out) {
        Logger::Instance().Warn(
            "Deno health check timed out after " + std::to_string(timeout_ms) +
            " ms; using QuickJS for this session (will retry on next startup).");
        return false;
    }

    state["deno_failed_version"] = kPinnedDenoVersion;
    state["deno_failed_reason"] = out_err;
    SaveJsonObject(state_path, state);
    Logger::Instance().Warn(
        "Deno health check failed (" + (out_err.empty() ? std::string("unknown error") : out_err) +
        "); using QuickJS fallback.");
    return false;
}

bool BinaryManager::ProbeDenoHealthPostDownload(const std::string& state_file_path) {
    bool timed_out = false;
    if (ProbeDenoHealth(state_file_path, 20000, &timed_out)) {
        return true;
    }
    if (!timed_out || cancel_flag_.load()) {
        return false;
    }
    if (config_.log_extractor_status) {
        Logger::Instance().Info(
            "Deno post-download health check timed out; retrying once in 30 s in background...");
    }
    const int override_ms = post_download_retry_delay_ms_override_.load();
    const int retry_delay_ms = (override_ms >= 0) ? override_ms : 30000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_delay_ms);
    while (!cancel_flag_.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        auto remain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(remain_ms, 50)));
    }
    if (cancel_flag_.load()) {
        return false;
    }
    return ProbeDenoHealth(state_file_path, 20000);
}

void BinaryManager::EnsureDenoBackground() {
    if (!config_.enable_js_runtime || cancel_flag_.load()) {
        return;
    }
    std::string req = config_.js_runtime;
    for (char& ch : req) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (req == "quickjs") {
        return;
    }
    if (!IsDenoSupported()) {
        return;
    }

    std::filesystem::path state_file = std::filesystem::path(config_.bin_dir) / "extractor_state.json";
    nlohmann::json state = LoadJsonObjectOrEmpty(state_file);
    if (state.value("deno_failed_version", "") == kPinnedDenoVersion) {
        return;
    }

    const std::string deno_p = GetDenoPath();
    if (!deno_p.empty() && std::filesystem::exists(deno_p)) {
        MakeExecutable(deno_p);
        if (!deno_healthy_.load() || GetDenoVersion().empty()) {
            if (!ProbeDenoHealth(state_file.string(), 10000)) {
                return;
            }
        }
        const std::string installed_ver = StripLeadingV(GetDenoVersion());
        const std::string pinned_ver = StripLeadingV(kPinnedDenoVersion);
        if (installed_ver == pinned_ver) {
            return;
        }
        if (!config_.auto_download_extractor) {
            return;
        }
        if (config_.log_extractor_status) {
            Logger::Instance().Info(
                "Installed Deno (" + (installed_ver.empty() ? std::string("unknown") : installed_ver) +
                ") differs from pinned " + pinned_ver + "; updating in background while keeping current Deno active.");
        }
    }

    if (!config_.auto_download_extractor) {
        return;
    }

    DenoDownloadTestHook dl_hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dl_hook = deno_download_hook_;
    }

    if (dl_hook) {
        std::string hook_err;
        if (!dl_hook(deno_p, kPinnedDenoVersion, &cancel_flag_, hook_err)) {
            if (!cancel_flag_.load()) {
                Logger::Instance().Warn("Deno background download failed: " + hook_err);
            }
            return;
        }
        if (cancel_flag_.load()) return;
        MakeExecutable(deno_p);
        if (ProbeDenoHealthPostDownload(state_file.string()) && config_.log_extractor_status) {
            Logger::Instance().Info(
                "Deno " + GetDenoVersion() + " ready in background at " + deno_p);
        }
        return;
    }

    std::vector<std::string> zip_pfx;
    bool zip_py = false;
    if (FindSystemZipExtractor(zip_pfx, zip_py).empty()) {
        bool warn_zip = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!deno_missing_zip_warned_) {
                deno_missing_zip_warned_ = true;
                warn_zip = true;
            }
        }
        if (warn_zip) {
#ifdef _WIN32
            Logger::Instance().Warn(
                "No system zip extractor (tar.exe) found to unpack Deno; continuing with QuickJS runtime.");
#else
            Logger::Instance().Warn(
                "No system zip extractor found to unpack Deno; install unzip (e.g. 'sudo apt install unzip') "
                "or continuing with QuickJS runtime.");
#endif
        }
        return;
    }

    DenoRuntimeAsset asset = GetDenoRuntimeAsset(deno_supported_override_.load());
    if (!asset.supported || !asset.url || !asset.sha256) {
        return;
    }

    if (config_.log_extractor_status) {
        Logger::Instance().Info(
            std::string("Downloading pinned JS runtime (Deno ") + kPinnedDenoVersion +
            ") in background from " + asset.url + " ...");
    }

    std::filesystem::path bin_dir(config_.bin_dir);
    std::filesystem::path zip_stage = bin_dir / "deno.zip.stage";
    std::filesystem::path extract_stage = bin_dir / "deno.stage";
    std::filesystem::path target_dir = bin_dir / "deno";
    std::filesystem::path bak_dir = bin_dir / "deno.bak";

    std::string err;
    if (!HttpClient::DownloadToFile(asset.url, zip_stage.string(), err, {}, 300000, &cancel_flag_)) {
        std::error_code ec;
        std::filesystem::remove(zip_stage, ec);
        if (!cancel_flag_.load()) {
            Logger::Instance().Warn("Deno background download failed: " + err + "; continuing with QuickJS.");
        }
        return;
    }

    const std::string actual_hash = ComputeFileSha256Hex(zip_stage.string());
    if (actual_hash != asset.sha256) {
        std::error_code ec;
        std::filesystem::remove(zip_stage, ec);
        Logger::Instance().Error(
            std::string("Deno SHA-256 verification failed for ") + asset.name +
            " (expected " + asset.sha256 + ", got " + actual_hash + "); refusing to install.");
        return;
    }

    std::error_code ec;
    std::filesystem::remove_all(extract_stage, ec);
    ec.clear();
    if (!ExtractZipArchive(zip_stage.string(), extract_stage.string(), &cancel_flag_, err)) {
        std::filesystem::remove(zip_stage, ec);
        std::filesystem::remove_all(extract_stage, ec);
        if (!cancel_flag_.load()) {
            Logger::Instance().Warn("Failed to extract Deno zip archive: " + err + "; continuing with QuickJS.");
        }
        return;
    }
    std::filesystem::remove(zip_stage, ec);

    std::filesystem::path staged_bin = extract_stage / GetDenoFilename();
    ec.clear();
    if (!std::filesystem::exists(staged_bin, ec) || ec || std::filesystem::file_size(staged_bin, ec) < 1024ULL * 1024ULL) {
        std::filesystem::remove_all(extract_stage, ec);
        Logger::Instance().Warn("Extracted Deno archive did not contain expected binary; continuing with QuickJS.");
        return;
    }

    std::filesystem::remove_all(bak_dir, ec);
    ec.clear();
    bool moved_old = false;
    if (std::filesystem::exists(target_dir, ec)) {
        ec.clear();
        std::filesystem::rename(target_dir, bak_dir, ec);
        if (ec) {
            std::filesystem::remove_all(extract_stage, ec);
            Logger::Instance().Warn(
                "Failed to replace previous Deno directory (existing Deno may be in use); keeping current Deno until next restart: " +
                ec.message());
            return;
        }
        moved_old = true;
    }

    ec.clear();
    std::filesystem::rename(extract_stage, target_dir, ec);
    if (ec) {
        if (moved_old) {
            std::error_code r_ec;
            std::filesystem::rename(bak_dir, target_dir, r_ec);
        }
        std::filesystem::remove_all(extract_stage, ec);
        Logger::Instance().Warn("Failed to install Deno directory atomically: " + ec.message());
        return;
    }
    std::filesystem::remove_all(bak_dir, ec);

    MakeExecutable(deno_p);
    if (config_.log_extractor_status) {
        Logger::Instance().Info(
            std::string("Deno ") + kPinnedDenoVersion + " downloaded and SHA-256 verified at " + deno_p);
    }
    ProbeDenoHealthPostDownload(state_file.string());
}

void BinaryManager::UpdateYtDlpIfDue() {
    std::filesystem::path state_file = std::filesystem::path(config_.bin_dir) / "extractor_state.json";
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json state = LoadJsonObjectOrEmpty(state_file);
    std::int64_t last_check = state.value("last_update_check", std::int64_t{0});
    std::string current_tag = state.value("ytdlp_tag", "");

    if (DecideYtDlpUpdateAction({now_epoch, last_check, current_tag, ""}) == YtDlpUpdateAction::SkipRecentlyChecked) {
        return;
    }

    // First run without a stored tag: probe installed binary via `--version`
    std::string ytdlp_path = GetYtDlpPath();
    if (current_tag.empty() && !ytdlp_path.empty() && std::filesystem::exists(ytdlp_path)) {
        std::string ver_out, ver_err;
        if (ExecuteSubprocess(ytdlp_path, {"--version"}, ver_out, ver_err, 5000, &cancel_flag_, nullptr)) {
            std::string probed = TrimAscii(ver_out);
            if (!probed.empty() && probed.size() <= 64) {
                current_tag = probed;
                state["ytdlp_tag"] = current_tag;
            }
        }
    }

    // Persist last_update_check while preserving ytdlp_tag and all other fields
    state["last_update_check"] = now_epoch;
    SaveJsonObject(state_file, state);

    if (cancel_flag_.load()) return;

    // Query GitHub latest release tag once and use tag-pinned URLs for both binary & SHA2-256SUMS
    std::string tag_err;
    std::string latest_tag = QueryLatestYtDlpTag(&cancel_flag_, tag_err);
    if (latest_tag.empty() || cancel_flag_.load()) {
        return;
    }

    auto action = DecideYtDlpUpdateAction({now_epoch, last_check, current_tag, latest_tag});
    if (action == YtDlpUpdateAction::DownloadAndUpdate && !cancel_flag_.load()) {
        if (config_.log_extractor_status) {
            Logger::Instance().Info(
                "New yt-dlp version available (" + (current_tag.empty() ? "unknown" : current_tag) +
                " -> " + latest_tag + "), updating in background...");
        }
        std::string installed;
        if (DownloadAndInstallYtDlpTag(latest_tag, true, installed)) {
            state["ytdlp_tag"] = latest_tag;
            SaveJsonObject(state_file, state);
            if (config_.log_extractor_status) {
                Logger::Instance().Info("yt-dlp updated and SHA-256 verified to " + latest_tag);
            }
        }
    }
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
