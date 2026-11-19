#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/audio/transcoder.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/extractor/browser_cookies.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/network/http_client.hpp"
#include "sampaudiostream/network/http_server.hpp"
#include "sampaudiostream/pawn/name_hook_decision.hpp"
#include "sampaudiostream/runtime/platform_host.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define TEST_CHECK(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "[FAIL] Check failed: " #cond " at line " << __LINE__ << "\n";      \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

std::atomic<const char*> g_current_test{"(startup)"};
std::atomic<const char*> g_current_phase{"(init)"};
std::atomic<std::int64_t> g_test_start_ms{0};
std::atomic<bool> g_tests_done{false};

#define TEST_PHASE(name)                                                                      \
    do {                                                                                      \
        g_current_phase.store((name), std::memory_order_release);                             \
        std::cout << "[PHASE] " << (name) << "\n";                                            \
    } while (0)

using namespace sampaudiostream;

namespace {

std::string GetSelfExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return "";
        }
        if (n < buf.size() - 1) {
            int needed = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
            if (needed <= 0) {
                return std::filesystem::path(std::wstring(buf.data(), n)).string();
            }
            std::string utf8(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), utf8.data(), needed, nullptr, nullptr);
            return utf8;
        }
        if (buf.size() >= 32768) {
            return "";
        }
        buf.resize(buf.size() * 2, L'\0');
    }
#else
    std::vector<char> buf(4096, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) {
        return "";
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
#endif
}

bool TryHandleChildMode(int argc, char** argv, int& out_exit_code) {
    if (argc >= 3 && std::strcmp(argv[1], "--child-stderr") == 0) {
        std::size_t remaining = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
#ifdef _WIN32
        _setmode(_fileno(stderr), _O_BINARY);
#endif
        std::array<char, 4096> chunk{};
        chunk.fill('E');
        while (remaining > 0) {
            const std::size_t take = (std::min)(remaining, chunk.size());
            std::fwrite(chunk.data(), 1, take, stderr);
            remaining -= take;
        }
        std::fflush(stderr);
        out_exit_code = 0;
        return true;
    }
    if (argc >= 5 && std::strcmp(argv[1], "--child-stderr-lines") == 0) {
        const std::size_t line_count = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
        const std::size_t line_len = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
        const char* final_line = argv[4];
#ifdef _WIN32
        _setmode(_fileno(stderr), _O_BINARY);
#endif
        std::string line(line_len, 'X');
        line.push_back('\n');
        for (std::size_t i = 0; i < line_count; ++i) {
            std::fwrite(line.data(), 1, line.size(), stderr);
        }
        if (final_line && *final_line) {
            std::fwrite(final_line, 1, std::strlen(final_line), stderr);
            std::fwrite("\n", 1, 1, stderr);
        }
        std::fflush(stderr);
        out_exit_code = 0;
        return true;
    }
    return false;
}

bool RunSubprocessWithDiag(
    const char* label,
    const std::string& exe_path,
    const std::vector<std::string>& args,
    std::string& sub_out,
    std::string& sub_err,
    int timeout_ms,
    const std::vector<std::pair<std::string, std::string>>* envs = nullptr,
    const std::function<void(const std::string&)>& on_stderr_line = nullptr,
    bool log_child_diag = false) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = ExecuteSubprocess(
        exe_path,
        args,
        sub_out,
        sub_err,
        timeout_ms,
        nullptr,
        nullptr,
        envs,
        on_stderr_line,
        log_child_diag);
    if (!ok) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::string err_preview;
        err_preview.reserve(200);
        for (unsigned char ch : sub_err) {
            if (err_preview.size() >= 200) break;
            err_preview.push_back((ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : ' ');
        }
        std::cerr << "[SUBPROCESS FAIL] " << label
                  << " elapsed_ms=" << elapsed_ms
                  << " out_bytes=" << sub_out.size()
                  << " err_bytes=" << sub_err.size()
                  << " err_preview=\"" << err_preview << "\"\n";
    }
    return ok;
}

void ConfigureTestSocketTimeouts(asio::ip::tcp::socket& sock, int timeout_ms = 5000) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

#ifdef _WIN32
void WriteHangMinidump() {
    wchar_t exe_path[MAX_PATH]{};
    DWORD len = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path dump_path =
        (len > 0 && len < MAX_PATH)
            ? std::filesystem::path(exe_path).parent_path() / L"sampaudiostream_tests_hang.dmp"
            : std::filesystem::path(L"sampaudiostream_tests_hang.dmp");
    HANDLE hFile = ::CreateFileW(
        dump_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "[HANG] Failed to create minidump file (err=" << ::GetLastError() << ")\n";
        return;
    }
    MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
    BOOL ok = ::MiniDumpWriteDump(
        ::GetCurrentProcess(),
        ::GetCurrentProcessId(),
        hFile,
        dump_type,
        nullptr,
        nullptr,
        nullptr);
    if (ok) {
        std::cerr << "[HANG] Wrote minidump: " << dump_path.string() << "\n";
    } else {
        std::cerr << "[HANG] MiniDumpWriteDump failed (err=" << ::GetLastError() << ")\n";
    }
    ::CloseHandle(hFile);
}
#endif

void StopTestAcceptor(asio::ip::tcp::acceptor& acc, std::uint16_t port) {
    // Wake a blocking accept() on Linux/POSIX by opening and closing one loopback connection.
    try {
        asio::io_context wake_io;
        asio::ip::tcp::socket s(wake_io);
        std::error_code ec;
        s.connect({asio::ip::address_v4::loopback(), port}, ec);
        s.close(ec);
    } catch (...) {
    }
    std::error_code ec;
#ifndef _WIN32
    ::shutdown(acc.native_handle(), SHUT_RDWR);
#endif
    acc.close(ec);
}

void CloseTestSockets(std::vector<std::shared_ptr<asio::ip::tcp::socket>>& sockets, std::mutex& mtx) {
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> to_close;
    {
        std::lock_guard<std::mutex> lk(mtx);
        to_close.swap(sockets);
    }
    for (auto& s : to_close) {
        if (!s) continue;
        std::error_code ec;
        s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s->close(ec);
    }
}

} // namespace

void TestRingBuffer() {
    AudioStreamBuffer buf(16);
    std::uint8_t data1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf.Write(data1, sizeof(data1));
    TEST_CHECK(buf.TotalWritten() == 10);

    std::uint64_t off = 0;
    std::uint8_t out[16]{};
    std::size_t n = buf.Read(off, out, 6, 0);
    TEST_CHECK(n == 6);
    TEST_CHECK(off == 6);
    TEST_CHECK(out[0] == 1 && out[5] == 6);

    // Overwrite wrap-around beyond capacity 16
    std::uint8_t data2[14] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    buf.Write(data2, sizeof(data2));
    TEST_CHECK(buf.TotalWritten() == 24);
    TEST_CHECK(buf.EarliestAvailableOffset() == 8);

    // Reader at old offset 6 should snap forward to 8
    n = buf.Read(off, out, 16, 0);
    TEST_CHECK(n == 16);
    TEST_CHECK(off == 24);
    TEST_CHECK(out[0] == 9 && out[15] == 24);

    // Oversized write (size >= capacity) at non-zero total_written_ must align modular positions
    std::uint8_t oversized[20];
    for (int i = 0; i < 20; ++i) oversized[i] = static_cast<std::uint8_t>(100 + i);
    buf.Write(oversized, sizeof(oversized));
    TEST_CHECK(buf.TotalWritten() == 44);
    TEST_CHECK(buf.EarliestAvailableOffset() == 28);
    n = buf.Read(off, out, 16, 0);
    TEST_CHECK(n == 16);
    TEST_CHECK(off == 44);
    for (int i = 0; i < 16; ++i) {
        TEST_CHECK(out[i] == static_cast<std::uint8_t>(104 + i));
    }

    // PrepareNextTrack keeps total_written_ strictly monotonic while clearing EOF
    buf.MarkEof();
    TEST_CHECK(buf.IsEof());
    buf.PrepareNextTrack();
    TEST_CHECK(!buf.IsEof());
    TEST_CHECK(buf.TotalWritten() == 44);
    std::cout << "[PASS] TestRingBuffer\n";
}

void TestMetadataCache() {
    MetadataCache cache(2, 60);
    TrackMetadata t1;
    t1.title = "Song 1";
    TrackMetadata t2;
    t2.title = "Song 2";
    TrackMetadata t3;
    t3.title = "Song 3";

    cache.PutTrack("k1", t1);
    cache.PutTrack("k2", t2);
    TEST_CHECK(cache.GetTrack("k1").has_value()); // k1 becomes MRU, k2 becomes LRU
    cache.PutTrack("k3", t3);                // evicts k2
    TEST_CHECK(!cache.GetTrack("k2").has_value());
    TEST_CHECK(cache.GetTrack("k1")->title == "Song 1");
    TEST_CHECK(cache.GetTrack("k3")->title == "Song 3");
    std::cout << "[PASS] TestMetadataCache\n";
}

void TestUrlClassifier() {
    TEST_CHECK(StreamResolver::LooksLikeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    TEST_CHECK(StreamResolver::LooksLikeUrl("http://radio.example.com:8000/stream.mp3"));
    TEST_CHECK(!StreamResolver::LooksLikeUrl("Linkin Park Numb"));

    TEST_CHECK(StreamResolver::IsKnownDirectAudioExtension("http://site.com/music.mp3?token=123"));
    TEST_CHECK(StreamResolver::IsKnownDirectAudioExtension("https://site.com/audio.OGG"));
    TEST_CHECK(!StreamResolver::IsKnownDirectAudioExtension("https://www.youtube.com/watch?v=abc"));

    // Domain suffix matching (must not match substrings like radiox.com for x.com)
    TEST_CHECK(StreamResolver::IsKnownPlatformUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    TEST_CHECK(StreamResolver::IsKnownPlatformUrl("https://x.com/user/status/1"));
    TEST_CHECK(StreamResolver::IsKnownPlatformUrl("https://mobile.x.com/user/status/1"));
    TEST_CHECK(!StreamResolver::IsKnownPlatformUrl("http://radiox.com/live.mp3"));
    TEST_CHECK(!StreamResolver::IsKnownPlatformUrl("https://box.com/shared/audio.mp3"));
    TEST_CHECK(!StreamResolver::IsKnownPlatformUrl("https://notyoutube.com/stream.ogg"));

    // Playlist parsers (.pls and .m3u)
    std::string pls_body =
        "[playlist]\r\n"
        "NumberOfEntries=2\r\n"
        "File1=http://radio.example.com:8000/stream\r\n"
        "Title1=Example Radio\r\n";
    TEST_CHECK(StreamResolver::ParsePlaylistFirstStreamUrl(pls_body, "http://radio.example.com/listen.pls") ==
           "http://radio.example.com:8000/stream");

    std::string m3u_body =
        "#EXTM3U\n"
        "#EXTINF:-1,Chill Radio\n"
        "/live/chill.mp3\n";
    TEST_CHECK(StreamResolver::ParsePlaylistFirstStreamUrl(m3u_body, "https://radio.example.com/list/chill.m3u") ==
           "https://radio.example.com/live/chill.mp3");

    std::cout << "[PASS] TestUrlClassifier\n";
}

void TestConfigManager() {
    std::string temp_cfg = "build_test_config.json";
    std::error_code ec;
    std::filesystem::remove(temp_cfg, ec);

    PluginConfig cfg = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(cfg.mp3_bitrate_kbps == 128);
    TEST_CHECK(cfg.hook_native_audio == true);
    TEST_CHECK(std::filesystem::exists(temp_cfg));

    cfg.http_port = 8899;
    TEST_CHECK(ConfigManager::Save(cfg, temp_cfg));
    PluginConfig loaded = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(loaded.http_port == 8899);

    // Malformed JSON must not be overwritten and must fall back safely
    {
        std::ofstream bad_ofs(temp_cfg, std::ios::trunc);
        bad_ofs << "{ invalid json !";
    }
    PluginConfig bad_loaded = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(bad_loaded.mp3_bitrate_kbps == 128);
    {
        std::ifstream verify_bad(temp_cfg);
        std::string raw;
        std::getline(verify_bad, raw);
        TEST_CHECK(raw == "{ invalid json !");
    }

    // Out-of-range sanitization: bitrate & sample rate snap to nearest valid,
    // other numeric ranges fall back to defaults cleanly
    bad_loaded.mp3_bitrate_kbps = 999;
    bad_loaded.mp3_sample_rate = 11025;
    bad_loaded.max_concurrent_streams = 9999;
    bad_loaded.worker_threads = 0;
    bad_loaded.pawn_text_encoding = "invalid_enc";
    ConfigManager::Sanitize(bad_loaded);
    TEST_CHECK(bad_loaded.mp3_bitrate_kbps == 320);
    TEST_CHECK(bad_loaded.mp3_sample_rate == 32000);
    TEST_CHECK(bad_loaded.max_concurrent_streams == 64);
    TEST_CHECK(bad_loaded.worker_threads == 4);
    TEST_CHECK(bad_loaded.pawn_text_encoding == "cp1252");

    // Signed integer validation & out-of-range fallback from JSON (negative & > 65535 port)
    {
        std::ofstream oor_ofs(temp_cfg, std::ios::trunc);
        oor_ofs << R"({"network":{"http_port":70000},"audio":{"max_concurrent_streams":-5,"transcoder_burst_ms":999999},"scripting":{"pawn_text_encoding":"cp1252"}})";
    }
    PluginConfig oor_loaded = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(oor_loaded.http_port == 0);
    TEST_CHECK(oor_loaded.max_concurrent_streams == 64);
    TEST_CHECK(oor_loaded.transcoder_burst_ms == 6000);
    TEST_CHECK(oor_loaded.pawn_text_encoding == "cp1252");
    TEST_CHECK(loaded.yt_dlp_cache_max_age_days == 7);

    // yt_dlp_cache_max_age_days validation: 0 disables pruning; 1..90 valid; >90 or <0 falls back to 7
    {
        std::ofstream age_ofs(temp_cfg, std::ios::trunc);
        age_ofs << R"({"extractor":{"yt_dlp_cache_max_age_days":0}})";
    }
    PluginConfig age_zero = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(age_zero.yt_dlp_cache_max_age_days == 0);
    {
        std::ofstream age_bad_ofs(temp_cfg, std::ios::trunc);
        age_bad_ofs << R"({"extractor":{"yt_dlp_cache_max_age_days":91}})";
    }
    PluginConfig age_oor = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(age_oor.yt_dlp_cache_max_age_days == 7);

    // UTF-8 -> CP1252 conversion test ("Café — €10")
    std::string utf8_sample = "Caf\xC3\xA9 \xE2\x80\x94 \xE2\x82\xAC" "10";
    std::string cp_out = ConvertTextForPawn(utf8_sample, "cp1252");
    TEST_CHECK(cp_out == std::string("Caf\xE9 \x97 \x80" "10"));
    TEST_CHECK(ConvertTextForPawn(utf8_sample, "utf8") == utf8_sample);

    // server.cfg parser test
    std::string temp_scfg = "build_test_server.cfg";
    {
        std::ofstream scfs(temp_scfg, std::ios::trunc);
        scfs << "# comment\nport 7788\nbind 10.0.0.5\n";
    }
    ServerCfgInfo scfg = ConfigManager::LoadServerCfg(temp_scfg);
    TEST_CHECK(scfg.has_port && scfg.port == 7788);
    TEST_CHECK(scfg.has_bind && scfg.bind_ip == "10.0.0.5");
    std::filesystem::remove(temp_scfg, ec);

    std::filesystem::remove(temp_cfg, ec);
    std::cout << "[PASS] TestConfigManager\n";
}

void TestHttpServer() {
    HttpStreamingServer srv;
    std::string err;
    TEST_CHECK(srv.Start("127.0.0.1", 0, err));
    TEST_CHECK(srv.IsRunning());
    TEST_CHECK(srv.BoundPort() > 0);

    auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
    srv.RegisterEndpoint(StreamEndpointInfo{"abc123", "Test Track", "Tester", 128, false, 0, buf});
    srv.UnregisterEndpoint("abc123");
    srv.Stop();
    TEST_CHECK(!srv.IsRunning());
    std::cout << "[PASS] TestHttpServer\n";
}

void TestThreadPoolAndScheduler() {
    ThreadPool pool(1, 256 * 1024);
    std::atomic<int> executed{0};
    std::atomic<bool> gate{false};

    pool.Post([&]() {
        while (!gate.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        executed.fetch_add(1);
    });
    for (int i = 0; i < 10; ++i) {
        pool.Post([&]() { executed.fetch_add(1); });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    gate.store(true);
    pool.Stop();
    // Pending tasks must have been discarded on Stop()
    TEST_CHECK(executed.load() < 11);

    TranscodeScheduler sched;
    sched.Start(4, 256 * 1024);
    std::atomic<int> jobs_done{0};
    for (int i = 0; i < 8; ++i) {
        TEST_CHECK(sched.Enqueue([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            jobs_done.fetch_add(1);
        }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sched.StopAndJoin();
    TEST_CHECK(jobs_done.load() == 8);
    TEST_CHECK(!sched.Enqueue([&]() {}));
    std::cout << "[PASS] TestThreadPoolAndScheduler\n";
}

struct MockAudioEvent {
    PlayerId player_id;
    bool play;
    std::string url;
    Vector3 pos{};
    float distance{0.0f};
    bool use_pos{false};
};

struct MockProcessingEvent {
    PlayerId player_id{INVALID_PLAYER_ID};
    StreamId stream_id{INVALID_STREAM_ID};
    std::string url;
    bool from_cache{false};
};

struct MockErrorEvent {
    StreamId stream_id{INVALID_STREAM_ID};
    std::int32_t error_code{0};
    std::string message;
};

struct MockChannelDestroyedEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::int32_t reason{0};
};

struct MockSearchResultsEvent {
    PlayerId player_id{INVALID_PLAYER_ID};
    SearchId search_id{INVALID_SEARCH_ID};
    std::int32_t total_found{0};
    std::int32_t error_code{0};
    std::string error_message;
};

struct MockChannelTrackStartEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::string title;
    std::string artist;
    std::int32_t duration{0};
    std::string url;
};

struct MockChannelErrorEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::int32_t error_code{0};
    std::string error_message;
};

struct MockChannelRestoredEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::string name;
    std::string creator_name;
};

struct MockSourceChangeEvent {
    PlayerId player_id{INVALID_PLAYER_ID};
    std::int32_t source_type{0};
    std::int32_t source_id{-1};
};

struct MockPlaylistQueuedEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::int32_t added{0};
    std::int32_t error_code{0};
    std::string error_message;
};

struct MockWaitingEvent {
    PlayerId player_id{INVALID_PLAYER_ID};
    StreamId stream_id{INVALID_STREAM_ID};
    std::int32_t wait_ms{0};
};

struct MockChannelWaitingEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::int32_t wait_ms{0};
};

struct MockLiveTitleEvent {
    PlayerId player_id{INVALID_PLAYER_ID};
    StreamId stream_id{INVALID_STREAM_ID};
    std::string title;
};

struct MockChannelLiveTitleEvent {
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::string title;
};

class MockPlatformHost final : public IPlatformHost {
public:
    mutable std::mutex mtx;
    std::vector<PlayerSnapshot> players;
    std::unordered_map<PlayerId, std::string> player_names;
    std::vector<MockAudioEvent> events;
    std::vector<StreamId> ready_stream_ids;
    std::vector<MockErrorEvent> error_events;
    std::vector<MockProcessingEvent> processing_events;
    std::vector<MockChannelDestroyedEvent> destroyed_channels;
    std::vector<MockSearchResultsEvent> search_results_events;
    std::vector<MockChannelTrackStartEvent> channel_track_starts;
    std::vector<ChannelId> queue_ended_channels;
    std::vector<MockChannelErrorEvent> channel_errors;
    std::vector<MockChannelRestoredEvent> restored_channels;
    std::vector<MockSourceChangeEvent> source_changes;
    std::vector<MockSourceChangeEvent> no_connect_events;
    std::vector<MockPlaylistQueuedEvent> playlist_queued_events;
    std::vector<MockWaitingEvent> waiting_events;
    std::vector<MockChannelWaitingEvent> channel_waiting_events;
    std::vector<MockLiveTitleEvent> live_title_events;
    std::vector<MockChannelLiveTitleEvent> channel_live_title_events;
    std::vector<std::string> callback_names;
    mutable std::thread::id expected_host_thread{std::this_thread::get_id()};
    mutable std::atomic<bool> get_ip_off_main_thread{false};
    mutable std::atomic<bool> spatial_query_off_main_thread{false};

    void Log(LogSeverity, const std::string&) override {}
    std::int32_t GetServerPort() const override { return 7777; }
    std::string GetServerBindIp() const override { return "127.0.0.1"; }
    bool TracksDisconnectEvents() const override { return false; }

    bool IsPlayerConnected(PlayerId player_id) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return true;
        }
        return false;
    }

    std::vector<PlayerId> GetConnectedPlayers() const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<PlayerId> ids;
        for (const auto& p : players) ids.push_back(p.id);
        return ids;
    }

    std::vector<PlayerSnapshot> GetConnectedPlayerSnapshots() const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        return players;
    }

    bool GetPlayerPos(PlayerId player_id, Vector3& out_pos) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) {
                out_pos = p.pos;
                return true;
            }
        }
        return false;
    }

    std::int32_t GetPlayerVirtualWorld(PlayerId player_id) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return p.world_id;
        }
        return 0;
    }

    std::int32_t GetPlayerInterior(PlayerId player_id) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return p.interior_id;
        }
        return 0;
    }

    std::unordered_map<std::int32_t, Vector3> vehicles;
    std::int32_t GetPlayerVehicleId(PlayerId) const override { return 0; }
    bool GetVehiclePos(std::int32_t veh_id, Vector3& out_pos) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            spatial_query_off_main_thread.store(true);
        }
        std::lock_guard<std::mutex> lock(mtx);
        auto it = vehicles.find(veh_id);
        if (it == vehicles.end()) return false;
        out_pos = it->second;
        return true;
    }
    std::string player_ip_override{"127.0.0.1"};
    std::string GetPlayerIp(PlayerId) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            get_ip_off_main_thread.store(true);
        }
        return player_ip_override;
    }
    std::string GetPlayerName(PlayerId player_id) const override {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = player_names.find(player_id);
        return (it != player_names.end()) ? it->second : std::string();
    }

    void SendPlayAudioStream(PlayerId player_id, const std::string& url, const Vector3& pos, float distance, bool use_pos) override {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back({player_id, true, url, pos, distance, use_pos});
    }

    void SendStopAudioStream(PlayerId player_id) override {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back({player_id, false, ""});
    }

    void DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) override {
        {
            std::lock_guard<std::mutex> lock(mtx);
            callback_names.push_back(name);
        }
        if (name == "OnAudioStreamProcessing" && args.size() >= 3) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* sid = std::get_if<std::int32_t>(&args[1]);
            auto* url = std::get_if<std::string>(&args[2]);
            bool cached = (args.size() >= 4 && std::get_if<std::int32_t>(&args[3]) && *std::get_if<std::int32_t>(&args[3]) != 0);
            if (pid && sid && url) {
                std::lock_guard<std::mutex> lock(mtx);
                processing_events.push_back({*pid, *sid, *url, cached});
            }
        } else if (name == "OnAudioStreamReady" && args.size() >= 2) {
            if (auto* sid = std::get_if<std::int32_t>(&args[1])) {
                std::lock_guard<std::mutex> lock(mtx);
                ready_stream_ids.push_back(*sid);
            }
        } else if (name == "OnAudioStreamError" && args.size() >= 4) {
            auto* sid = std::get_if<std::int32_t>(&args[1]);
            auto* code = std::get_if<std::int32_t>(&args[2]);
            auto* msg = std::get_if<std::string>(&args[3]);
            if (sid && code && msg) {
                std::lock_guard<std::mutex> lock(mtx);
                error_events.push_back({*sid, *code, *msg});
            }
        } else if (name == "OnAudioStreamChannelDestroyed" && args.size() >= 2) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* reason = std::get_if<std::int32_t>(&args[1]);
            if (cid && reason) {
                std::lock_guard<std::mutex> lock(mtx);
                destroyed_channels.push_back({*cid, *reason});
            }
        } else if (name == "OnAudioStreamSearchResults" && args.size() >= 5) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* sid = std::get_if<std::int32_t>(&args[1]);
            auto* total = std::get_if<std::int32_t>(&args[2]);
            auto* code = std::get_if<std::int32_t>(&args[3]);
            auto* msg = std::get_if<std::string>(&args[4]);
            if (pid && sid && total && code && msg) {
                std::lock_guard<std::mutex> lock(mtx);
                search_results_events.push_back({*pid, *sid, *total, *code, *msg});
            }
        } else if (name == "OnAudioStreamChannelTrackStart" && args.size() >= 5) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* title = std::get_if<std::string>(&args[1]);
            auto* artist = std::get_if<std::string>(&args[2]);
            auto* dur = std::get_if<std::int32_t>(&args[3]);
            auto* url = std::get_if<std::string>(&args[4]);
            if (cid && title && artist && dur && url) {
                std::lock_guard<std::mutex> lock(mtx);
                channel_track_starts.push_back({*cid, *title, *artist, *dur, *url});
            }
        } else if (name == "OnAudioStreamChannelQueueEnd" && args.size() >= 1) {
            if (auto* cid = std::get_if<std::int32_t>(&args[0])) {
                std::lock_guard<std::mutex> lock(mtx);
                queue_ended_channels.push_back(*cid);
            }
        } else if (name == "OnAudioStreamChannelError" && args.size() >= 3) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* code = std::get_if<std::int32_t>(&args[1]);
            auto* msg = std::get_if<std::string>(&args[2]);
            if (cid && code && msg) {
                std::lock_guard<std::mutex> lock(mtx);
                channel_errors.push_back({*cid, *code, *msg});
            }
        } else if (name == "OnAudioStreamChannelRestored" && args.size() >= 3) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* cname = std::get_if<std::string>(&args[1]);
            auto* creator = std::get_if<std::string>(&args[2]);
            if (cid && cname && creator) {
                std::lock_guard<std::mutex> lock(mtx);
                restored_channels.push_back({*cid, *cname, *creator});
            }
        } else if (name == "OnPlayerAudioStreamSourceChange" && args.size() >= 3) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* stype = std::get_if<std::int32_t>(&args[1]);
            auto* sid = std::get_if<std::int32_t>(&args[2]);
            if (pid && stype && sid) {
                std::lock_guard<std::mutex> lock(mtx);
                source_changes.push_back({*pid, *stype, *sid});
            }
        } else if (name == "OnPlayerAudioStreamNoConnect" && args.size() >= 3) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* stype = std::get_if<std::int32_t>(&args[1]);
            auto* sid = std::get_if<std::int32_t>(&args[2]);
            if (pid && stype && sid) {
                std::lock_guard<std::mutex> lock(mtx);
                no_connect_events.push_back({*pid, *stype, *sid});
            }
        } else if (name == "OnAudioStreamChannelPlaylistQueued" && args.size() >= 4) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* added = std::get_if<std::int32_t>(&args[1]);
            auto* code = std::get_if<std::int32_t>(&args[2]);
            auto* msg = std::get_if<std::string>(&args[3]);
            if (cid && added && code && msg) {
                std::lock_guard<std::mutex> lock(mtx);
                playlist_queued_events.push_back({*cid, *added, *code, *msg});
            }
        } else if (name == "OnAudioStreamWaiting" && args.size() >= 3) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* sid = std::get_if<std::int32_t>(&args[1]);
            auto* wms = std::get_if<std::int32_t>(&args[2]);
            if (pid && sid && wms) {
                std::lock_guard<std::mutex> lock(mtx);
                waiting_events.push_back({*pid, *sid, *wms});
            }
        } else if (name == "OnAudioStreamChannelTrackWaiting" && args.size() >= 2) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* wms = std::get_if<std::int32_t>(&args[1]);
            if (cid && wms) {
                std::lock_guard<std::mutex> lock(mtx);
                channel_waiting_events.push_back({*cid, *wms});
            }
        } else if (name == "OnAudioStreamLiveTitleChange" && args.size() >= 3) {
            auto* pid = std::get_if<std::int32_t>(&args[0]);
            auto* sid = std::get_if<std::int32_t>(&args[1]);
            auto* title = std::get_if<std::string>(&args[2]);
            if (pid && sid && title) {
                std::lock_guard<std::mutex> lock(mtx);
                live_title_events.push_back({*pid, *sid, *title});
            }
        } else if (name == "OnAudioStreamChannelLiveTitleChange" && args.size() >= 2) {
            auto* cid = std::get_if<std::int32_t>(&args[0]);
            auto* title = std::get_if<std::string>(&args[1]);
            if (cid && title) {
                std::lock_guard<std::mutex> lock(mtx);
                channel_live_title_events.push_back({*cid, *title});
            }
        }
    }
};

void PumpEngineTicks(AudioEngine& engine, int count = 12) {
    for (int i = 0; i < count; ++i) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void TestAudioEngineArbitrationAndDisconnect() {
    // Local loopback server serving /global_radio.pls -> /radio_stream.mp3 and /channel_live.wav
    asio::io_context ioc;
    asio::ip::tcp::acceptor acc(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    std::uint16_t pls_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex cw_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> cw_sockets;
    std::vector<std::thread> client_workers;
    std::thread pls_srv([&]() {
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(ioc);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> lk(cw_mtx);
            cw_sockets.push_back(sock);
            client_workers.emplace_back([sock, pls_port, &stop_srv]() {
                asio::streambuf sbuf;
                std::error_code io_ec;
                asio::read_until(*sock, sbuf, "\r\n\r\n", io_ec);
                if (io_ec) return;
                std::istream is(&sbuf);
                std::string m, path;
                is >> m >> path;
                if (path == "/channel_live.wav") {
                    std::vector<std::uint8_t> hdr_wav(44, 0);
                    auto w32 = [&](std::size_t o, std::uint32_t v) {
                        hdr_wav[o] = v & 0xFF; hdr_wav[o+1] = (v>>8)&0xFF;
                        hdr_wav[o+2] = (v>>16)&0xFF; hdr_wav[o+3] = (v>>24)&0xFF;
                    };
                    std::memcpy(hdr_wav.data(), "RIFF", 4);
                    w32(4, 36 + 48000 * 4 * 5);
                    std::memcpy(hdr_wav.data() + 8, "WAVEfmt ", 8);
                    w32(16, 16);
                    hdr_wav[20] = 1; hdr_wav[22] = 2; // PCM stereo
                    w32(24, 48000); w32(28, 48000 * 4);
                    hdr_wav[32] = 4; hdr_wav[34] = 16;
                    std::memcpy(hdr_wav.data() + 36, "data", 4);
                    w32(40, 48000 * 4 * 5);

                    std::string http_hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                           std::to_string(44 + 48000 * 4 * 5) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(http_hdr), io_ec);
                    asio::write(*sock, asio::buffer(hdr_wav), io_ec);
                    std::vector<std::uint8_t> pcm_chunk(48000 * 4 / 10, 0); // 100ms stereo silence
                    for (int k = 0; k < 50 && !stop_srv.load() && !io_ec; ++k) {
                        asio::write(*sock, asio::buffer(pcm_chunk), io_ec);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    return;
                }
                std::string body = "[playlist]\r\nNumberOfEntries=1\r\nFile1=http://127.0.0.1:" +
                                   std::to_string(pls_port) + "/radio_stream.mp3\r\n";
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/x-scpls\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(*sock, asio::buffer(hdr), io_ec);
                asio::write(*sock, asio::buffer(body), io_ec);
            });
        }
    });

    MockPlatformHost host;
    host.players.push_back({1, {0.0f, 0.0f, 0.0f}, 0, 0});

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.spatial_update_interval_ms = 10;
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    std::string global_pls_url = "http://127.0.0.1:" + std::to_string(pls_port) + "/global_radio.pls";
    std::string expected_radio_url = "http://127.0.0.1:" + std::to_string(pls_port) + "/radio_stream.mp3";
    std::string channel_wav_url = "http://127.0.0.1:" + std::to_string(pls_port) + "/channel_live.wav";

    // 1. Start Global spatial stream (live passthrough .pls resolved locally)
    StreamOptions gopts;
    gopts.use_position = true;
    gopts.position = {0.0f, 0.0f, 0.0f};
    gopts.distance = 50.0f;
    StreamId gsid = engine.PlayAudioStream(global_pls_url, gopts);
    TEST_CHECK(gsid != INVALID_STREAM_ID);
    PumpEngineTicks(engine, 15);
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url == expected_radio_url);
        host.events.clear();
    }

    // 2. Join Radio Channel (Channel > Global): overrides Global cleanly
    ChannelId ch = engine.CreateChannel("Rock");
    TEST_CHECK(ch != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.PlayChannel(ch, channel_wav_url));
    PumpEngineTicks(engine, 15);
    TEST_CHECK(engine.JoinChannel(1, ch));
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url.find("/channel/") != std::string::npos);
        host.events.clear();
    }

    // 3. Player exits and re-enters Global spatial zone while Channel is playing:
    // SendStopAudioStream must NOT be sent for leaving lower-priority Global!
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        host.players[0].pos = {500.0f, 500.0f, 0.0f};
    }
    PumpEngineTicks(engine, 5);
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(host.events.empty());
        host.players[0].pos = {0.0f, 0.0f, 0.0f};
    }
    PumpEngineTicks(engine, 5);
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(host.events.empty());
    }

    // 4. Play Personal stream (Personal > Channel > Global)
    StreamId psid = engine.PlayAudioStreamForPlayer(1, "http://127.0.0.1/personal_song.mp3", StreamOptions{});
    TEST_CHECK(psid != INVALID_STREAM_ID);
    PumpEngineTicks(engine, 15);
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url == "http://127.0.0.1/personal_song.mp3");
        host.events.clear();
    }

    // 5. Pause Personal stream -> stops client audio WITHOUT falling back to Channel/Global
    TEST_CHECK(engine.PauseAudioStreamForPlayer(1, true));
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events.back().play == false);
        host.events.clear();
    }

    // 6. Stop Personal stream -> restores Channel automatically!
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url.find("/channel/") != std::string::npos);
        host.events.clear();
    }

    // 7. Leave Channel while inside Global spatial range -> restores Global automatically!
    TEST_CHECK(engine.LeaveChannel(1));
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url == expected_radio_url);
        host.events.clear();
    }

    // 8. Disconnect snapshot detection on SA-MP (TracksDisconnectEvents() == false):
    // Player 1 disappears -> Tick() cleans up all state cleanly without calling SendPlay/SendStop
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        host.players.clear();
    }
    PumpEngineTicks(engine, 5);
    TEST_CHECK(engine.GetPlayerStreamId(1) == INVALID_STREAM_ID);
    TEST_CHECK(engine.GetPlayerChannelId(1) == INVALID_CHANNEL_ID);
    {
        std::lock_guard<std::mutex> lock(host.mtx);
        TEST_CHECK(host.events.empty());
    }

    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, pls_port);
    if (pls_srv.joinable()) pls_srv.join();
    CloseTestSockets(cw_sockets, cw_mtx);
    for (auto& cw : client_workers) {
        if (cw.joinable()) cw.join();
    }
    std::cout << "[PASS] TestAudioEngineArbitrationAndDisconnect\n";
}

void TestSpatialPerformanceBenchmark() {
    // Serve a live .pls radio endpoint locally so 10 global spatial streams stay live during benchmark
    asio::io_context ioc;
    asio::ip::tcp::acceptor acc(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    std::uint16_t pls_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::thread pls_srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(ioc);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            std::string body = "[playlist]\r\nNumberOfEntries=1\r\nFile1=http://127.0.0.1:" +
                               std::to_string(pls_port) + "/bench_stream.mp3\r\n";
            std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/x-scpls\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::write(sock, asio::buffer(hdr), ec);
            asio::write(sock, asio::buffer(body), ec);
        }
    });

    MockPlatformHost host;
    host.players.reserve(500);
    for (int i = 0; i < 500; ++i) {
        float coord = static_cast<float>(i * 10);
        host.players.push_back({i, {coord, 0.0f, 0.0f}, 0, 0});
    }

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.auto_update_extractor = false;
    cfg.spatial_update_interval_ms = 0;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    std::string pls_url = "http://127.0.0.1:" + std::to_string(pls_port) + "/bench.pls";
    for (int i = 0; i < 10; ++i) {
        StreamOptions opts;
        opts.use_position = true;
        opts.position = {static_cast<float>(i * 200), 0.0f, 0.0f};
        opts.distance = 35.0f;
        TEST_CHECK(engine.PlayAudioStream(pls_url, opts) != INVALID_STREAM_ID);
    }
    for (int i = 0; i < 30; ++i) {
        TEST_CHECK(engine.PlayAudioStreamForPlayer(i, "http://127.0.0.1/p.mp3", StreamOptions{}) != INVALID_STREAM_ID);
    }

    PumpEngineTicks(engine, 20);

    constexpr int kIterations = 50;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        engine.Tick();
    }
    auto t1 = std::chrono::steady_clock::now();
    double avg_us = static_cast<double>(
                        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) /
                    kIterations;
    std::cout << "[BENCH] 500 players x 10 spatial + 30 personal mean Tick(): " << avg_us << " us\n";
    TEST_CHECK(avg_us < 2000.0);

    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, pls_port);
    if (pls_srv.joinable()) pls_srv.join();
    std::cout << "[PASS] TestSpatialPerformanceBenchmark\n";
}

void TestRound60Features() {
    std::error_code fec;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sampaudiostream_r60_test";
    std::filesystem::remove_all(temp_dir, fec);
    std::filesystem::create_directories(temp_dir / "bin", fec);

    auto make_cfg = [&]() {
        PluginConfig c;
        c.bind_ip = "127.0.0.1";
        c.public_ip = "127.0.0.1";
        c.http_port = 0;
        c.base_dir = temp_dir.string();
        c.bin_dir = (temp_dir / "bin").string();
        c.auto_download_extractor = false;
        c.auto_update_extractor = false;
        c.enable_js_runtime = false;
        c.allow_private_network_urls = true;
        c.persist_channels = true;
        c.pawn_text_encoding = "utf8";
        c.max_queue_per_channel = 50;
        return c;
    };

    // 1. Config schema includes audio.normalize_volume (bool, default false)
    TEST_PHASE("r60_config");
    {
        PluginConfig def_cfg;
        TEST_CHECK(!def_cfg.normalize_volume);
        const auto fields = ConfigManager::GetUnifiedSchemaFields();
        bool found_norm = false;
        for (const auto& f : fields) {
            if (f.full_key == "audio.normalize_volume") {
                found_norm = true;
                break;
            }
        }
        TEST_CHECK(found_norm);

        const std::string cfg_path = (temp_dir / "norm_cfg.json").string();
        PluginConfig loaded = ConfigManager::LoadOrCreate(cfg_path);
        TEST_CHECK(!loaded.normalize_volume);
        std::ifstream ifs(cfg_path);
        std::string raw_json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_CHECK(raw_json.find("\"normalize_volume\": false") != std::string::npos);

        PluginConfig saved = loaded;
        saved.normalize_volume = true;
        TEST_CHECK(ConfigManager::Save(saved, cfg_path));
        PluginConfig reloaded = ConfigManager::LoadOrCreate(cfg_path);
        TEST_CHECK(reloaded.normalize_volume);
    }

    // 2. Queue item labels: sanitization, UTF-8 128-byte boundary, persistence & legacy JSON load, shuffle
    TEST_PHASE("r60_queue_labels_and_shuffle");
    {
#ifdef _WIN32
        const std::filesystem::path fake_ext = temp_dir / "bin" / "yt-dlp.exe";
#else
        const std::filesystem::path fake_ext = temp_dir / "bin" / "yt-dlp";
#endif
        {
            std::ofstream ofs(fake_ext, std::ios::binary);
            ofs << std::string(128 * 1024, 'x');
        }
#ifndef _WIN32
        std::filesystem::permissions(
            fake_ext,
            std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::add,
            fec);
#endif
        MockPlatformHost host;
        AudioEngine engine(host, make_cfg());
        TEST_CHECK(engine.Initialize());
        TEST_CHECK(engine.BinariesForTesting().WaitUntilSettled(std::chrono::seconds(2)));

        const ChannelId cid = engine.CreateChannel("R60 Radio", INVALID_PLAYER_ID, "", ChannelDestroyMode::Manual, 0);
        TEST_CHECK(cid != INVALID_CHANNEL_ID);

        // Start a dummy active track so subsequent queue pushes remain in queue for inspection/shuffle
        // First queue item becomes active current_input via PlayChannelLocked, preserving its label too.
        TEST_CHECK(engine.QueueChannel(cid, "http://127.0.0.1:9/seed.mp3", "http://127.0.0.1:9/seed.mp3", "Seed\tLine\r\nTrack"));

        // 127 ASCII bytes + 3-byte UTF-8 note (E2 99 AA) -> truncated safely to 127 bytes without half-codepoint
        const std::string label_boundary = std::string(127, 'A') + "\xE2\x99\xAA" + "XYZ";
        TEST_CHECK(engine.QueueChannel(cid, "http://127.0.0.1:9/t1.mp3", "", label_boundary));
        TEST_CHECK(engine.InsertChannelQueueItem(cid, 0, "http://127.0.0.1:9/t0.mp3", "", "Tab\tCR\rLF\nTitle"));

        std::string url0, lbl0, url1, lbl1;
        TEST_CHECK(engine.GetChannelQueueItem(cid, 0, url0));
        TEST_CHECK(engine.GetChannelQueueItemLabel(cid, 0, lbl0));
        TEST_CHECK(url0 == "http://127.0.0.1:9/t0.mp3");
        TEST_CHECK(lbl0 == "Tab CR LF Title");

        TEST_CHECK(engine.GetChannelQueueItem(cid, 1, url1));
        TEST_CHECK(engine.GetChannelQueueItemLabel(cid, 1, lbl1));
        TEST_CHECK(url1 == "http://127.0.0.1:9/t1.mp3");
        TEST_CHECK(lbl1.size() == 127);
        TEST_CHECK(lbl1 == std::string(127, 'A'));

        std::string bad_lbl;
        TEST_CHECK(!engine.GetChannelQueueItemLabel(cid, 99, bad_lbl));
        TEST_CHECK(!engine.GetChannelQueueItemLabel(999999, 0, bad_lbl));

        // Add more items and test ShuffleChannelQueue preserves URL<->label pairing and never touches invalid channel
        TEST_CHECK(!engine.ShuffleChannelQueue(999999));
        for (int k = 2; k <= 7; ++k) {
            TEST_CHECK(engine.QueueChannel(
                cid,
                "http://127.0.0.1:9/t" + std::to_string(k) + ".mp3",
                "",
                "Label #" + std::to_string(k)));
        }

        std::vector<std::pair<std::string, std::string>> before_items;
        for (std::size_t i = 0; i < 8; ++i) {
            std::string u, l;
            TEST_CHECK(engine.GetChannelQueueItem(cid, i, u));
            TEST_CHECK(engine.GetChannelQueueItemLabel(cid, i, l));
            before_items.emplace_back(u, l);
        }

        bool order_changed = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            TEST_CHECK(engine.ShuffleChannelQueue(cid));
            std::vector<std::pair<std::string, std::string>> after_items;
            for (std::size_t i = 0; i < 8; ++i) {
                std::string u, l;
                TEST_CHECK(engine.GetChannelQueueItem(cid, i, u));
                TEST_CHECK(engine.GetChannelQueueItemLabel(cid, i, l));
                after_items.emplace_back(u, l);
            }
            if (after_items != before_items) {
                order_changed = true;
            }
            std::sort(before_items.begin(), before_items.end());
            std::sort(after_items.begin(), after_items.end());
            TEST_CHECK(before_items == after_items);
            if (order_changed) break;
        }
        TEST_CHECK(order_changed);

        // Playlist import populates labels from flat-playlist entry titles
        engine.ResolverForTesting().SetYtDlpTestHook([](
            const std::vector<std::string>&,
            std::string& out_stdout,
            std::string&,
            int,
            const std::atomic<bool>*) {
            out_stdout = R"({"entries":[
                {"id":"plVid000001","title":"First Playlist Song"},
                {"id":"plVid000002","title":"Second\tPlaylist\nSong"}
            ]})";
            return true;
        });
        TEST_CHECK(engine.QueueChannelPlaylist(cid, "https://www.youtube.com/playlist?list=R60PL", 10));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            engine.Tick();
            ChannelInfo ci{};
            if (engine.GetChannelInfo(cid, ci) && ci.queue_count >= 10) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::string pl_u0, pl_l0, pl_u1, pl_l1;
        TEST_CHECK(engine.GetChannelQueueItem(cid, 8, pl_u0));
        TEST_CHECK(engine.GetChannelQueueItemLabel(cid, 8, pl_l0));
        TEST_CHECK(pl_u0.find("plVid000001") != std::string::npos);
        TEST_CHECK(pl_l0 == "First Playlist Song");
        TEST_CHECK(engine.GetChannelQueueItem(cid, 9, pl_u1));
        TEST_CHECK(engine.GetChannelQueueItemLabel(cid, 9, pl_l1));
        TEST_CHECK(pl_l1 == "Second Playlist Song");

        engine.Shutdown();

        // Persistence round-trip restores labels on queue items and current_input
        {
            MockPlatformHost host2;
            AudioEngine engine2(host2, make_cfg());
            TEST_CHECK(engine2.Initialize());
            std::string r_u, r_l;
            TEST_CHECK(engine2.GetChannelQueueItem(cid, 8, r_u));
            TEST_CHECK(engine2.GetChannelQueueItemLabel(cid, 8, r_l));
            TEST_CHECK(r_l == "First Playlist Song");
            engine2.Shutdown();
        }

        // Legacy channel JSON without label_hex / pawn_label_hex loads cleanly with empty label
        {
            const std::filesystem::path legacy_path = temp_dir / "channels" / "channel_777.json";
            std::filesystem::create_directories(legacy_path.parent_path(), fec);
            std::ofstream lfs(legacy_path, std::ios::binary | std::ios::trunc);
            lfs << R"({
  "format_version": 1,
  "id": 777,
  "name": "Legacy Channel",
  "has_creator": false,
  "destroy_mode": 0,
  "destroy_timeout_seconds": 0,
  "volume": 100.0,
  "paused": false,
  "loop_mode": 0,
  "current_input": {"utf8_url": "", "pawn_url_hex": ""},
  "was_playing": false,
  "queue": [{"utf8_url": "http://127.0.0.1:9/legacy.mp3", "pawn_url_hex": ""}],
  "spatial": {"use_position": false, "x": 0.0, "y": 0.0, "z": 0.0, "distance": 50.0, "world_id": -1, "interior_id": -1, "auto_listen": true}
})";
            lfs.close();

            MockPlatformHost host3;
            AudioEngine engine3(host3, make_cfg());
            TEST_CHECK(engine3.Initialize());
            std::string leg_url, leg_lbl = "nonempty";
            TEST_CHECK(engine3.GetChannelQueueItem(777, 0, leg_url));
            TEST_CHECK(leg_url == "http://127.0.0.1:9/legacy.mp3");
            TEST_CHECK(engine3.GetChannelQueueItemLabel(777, 0, leg_lbl));
            TEST_CHECK(leg_lbl.empty());
            engine3.Shutdown();
        }
    }

    // 3. Volume normalization pipeline tests
    TEST_PHASE("r60_volume_normalization");
    {
        auto make_sine_chunks = [](double target_rms_dbfs, int seconds, int sample_rate = 48000, int freq_hz = 440) {
            const double rms_lin = std::pow(10.0, target_rms_dbfs / 20.0);
            const double amp = rms_lin * std::sqrt(2.0) * 32768.0;
            const std::size_t chunk_frames = 4096;
            const std::size_t total_frames = static_cast<std::size_t>(seconds) * static_cast<std::size_t>(sample_rate);
            std::vector<std::vector<std::int16_t>> chunks;
            std::size_t generated = 0;
            constexpr double kTwoPi = 6.28318530717958647692;
            while (generated < total_frames) {
                const std::size_t n = std::min(chunk_frames, total_frames - generated);
                std::vector<std::int16_t> c(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const double t = static_cast<double>(generated + i) / static_cast<double>(sample_rate);
                    const double v = std::clamp(std::sin(kTwoPi * freq_hz * t) * amp, -32768.0, 32767.0);
                    c[i] = static_cast<std::int16_t>(std::lrint(v));
                }
                chunks.push_back(std::move(c));
                generated += n;
            }
            return chunks;
        };

        auto measure_tail_rms_db = [](const std::vector<std::int16_t>& stereo_pcm, std::size_t tail_frames) {
            const std::size_t total_frames = stereo_pcm.size() / 2;
            const std::size_t count = std::min(tail_frames, total_frames);
            const std::size_t start_frame = total_frames - count;
            double sum_sq = 0.0;
            for (std::size_t i = start_frame; i < total_frames; ++i) {
                const double l = static_cast<double>(stereo_pcm[i * 2]) / 32768.0;
                const double r = static_cast<double>(stereo_pcm[i * 2 + 1]) / 32768.0;
                sum_sq += 0.5 * (l * l + r * r);
            }
            const double mean_sq = sum_sq / static_cast<double>(count);
            return (mean_sq > 1e-12) ? (10.0 * std::log10(mean_sq)) : -120.0;
        };

        TranscoderConfig base_cfg;
        base_cfg.target_sample_rate = 48000;
        base_cfg.target_bitrate_kbps = 128;
        base_cfg.pace_realtime = false;

        // 3a: Quiet -22 dBFS sine converges to -16 dBFS +/- 2 dB; -30 dBFS caps gain at +9 dB
        {
            TranscoderConfig cfg = base_cfg;
            cfg.normalize_volume = true;
            std::vector<std::uint8_t> mp3;
            std::vector<std::int16_t> pcm;
            float gain_db = 0.0f;

            const auto quiet_22 = make_sine_chunks(-22.0, 6);
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg, 100.0f, 48000, 1, quiet_22, mp3, &pcm, &gain_db));
            const double tail_db_22 = measure_tail_rms_db(pcm, 48000);
            TEST_CHECK(tail_db_22 >= -18.0 && tail_db_22 <= -14.0);
            TEST_CHECK(gain_db <= 9.001f);

            const auto quiet_30 = make_sine_chunks(-30.0, 8);
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg, 100.0f, 48000, 1, quiet_30, mp3, &pcm, &gain_db));
            TEST_CHECK(gain_db <= 9.001f && gain_db >= 8.5f);
            const double tail_db_30 = measure_tail_rms_db(pcm, 48000);
            TEST_CHECK(tail_db_30 > -23.0 && tail_db_30 < -19.0);
        }

        // 3b: Loud -3 dBFS sine stays <= 0.98 full scale across every sample (including first transient sample)
        {
            TranscoderConfig cfg = base_cfg;
            cfg.normalize_volume = true;
            std::vector<std::uint8_t> mp3;
            std::vector<std::int16_t> pcm;
            float gain_db = 0.0f;
            const auto loud = make_sine_chunks(-3.0, 4);
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg, 100.0f, 48000, 1, loud, mp3, &pcm, &gain_db));
            const double max_allowed = 0.98 * 32768.0;
            double peak = 0.0;
            for (std::int16_t s : pcm) {
                peak = std::max(peak, std::abs(static_cast<double>(s)));
            }
            TEST_CHECK(peak <= max_allowed);
            TEST_CHECK(gain_db < 0.0f);
        }

        // 3c: Silence leaves gain at 0 dB and matches disabled output
        {
            std::vector<std::vector<std::int16_t>> silence_chunks(10, std::vector<std::int16_t>(4800, 0));
            TranscoderConfig cfg_off = base_cfg;
            cfg_off.normalize_volume = false;
            TranscoderConfig cfg_on = base_cfg;
            cfg_on.normalize_volume = true;

            std::vector<std::uint8_t> mp3_off, mp3_on;
            std::vector<std::int16_t> pcm_off, pcm_on;
            float gain_db = 123.0f;
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg_off, 100.0f, 48000, 1, silence_chunks, mp3_off, &pcm_off));
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg_on, 100.0f, 48000, 1, silence_chunks, mp3_on, &pcm_on, &gain_db));
            TEST_CHECK(std::abs(gain_db) < 0.001f);
            TEST_CHECK(pcm_off == pcm_on);
            TEST_CHECK(mp3_off == mp3_on);
        }

        // 3d: Disabled option reproduces exact untouched path (100% mono passthrough matches input sample-for-sample)
        {
            const auto test_in = make_sine_chunks(-22.0, 2);
            TranscoderConfig cfg_off = base_cfg;
            cfg_off.normalize_volume = false;
            std::vector<std::uint8_t> mp3_a, mp3_b;
            std::vector<std::int16_t> pcm_a, pcm_b;
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg_off, 100.0f, 48000, 1, test_in, mp3_a, &pcm_a));
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg_off, 100.0f, 48000, 1, test_in, mp3_b, &pcm_b));
            TEST_CHECK(mp3_a == mp3_b);
            std::size_t idx = 0;
            for (const auto& ch : test_in) {
                for (std::int16_t s : ch) {
                    TEST_CHECK(pcm_a[idx++] == s);
                    TEST_CHECK(pcm_a[idx++] == s);
                }
            }
        }

        // 3e: Asymmetric gain drop (-35 dBFS intro 6s -> -10 dBFS chorus 6s drops fast at 6 dB/s)
        {
            TranscoderConfig cfg = base_cfg;
            cfg.normalize_volume = true;
            auto chunks = make_sine_chunks(-35.0, 6);
            auto chorus = make_sine_chunks(-10.0, 6);
            chunks.insert(chunks.end(), chorus.begin(), chorus.end());

            std::vector<std::uint8_t> mp3;
            std::vector<std::int16_t> pcm;
            float gain_db = 0.0f;
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg, 100.0f, 48000, 1, chunks, mp3, &pcm, &gain_db));
            const double tail_db = measure_tail_rms_db(pcm, 48000);
            TEST_CHECK(tail_db >= -18.0 && tail_db <= -14.0);
            TEST_CHECK(gain_db < 0.0f);
        }

        // 3f: Initial 4s silence does NOT consume the 3s fast phase before 3s of -22 dBFS
        {
            TranscoderConfig cfg = base_cfg;
            cfg.normalize_volume = true;
            std::vector<std::vector<std::int16_t>> chunks(40, std::vector<std::int16_t>(4800, 0)); // 4 s silence @ 48kHz
            auto active = make_sine_chunks(-22.0, 3);
            chunks.insert(chunks.end(), active.begin(), active.end());

            std::vector<std::uint8_t> mp3;
            std::vector<std::int16_t> pcm;
            float gain_db = 0.0f;
            TEST_CHECK(RunNormalizedPipelineForTesting(cfg, 100.0f, 48000, 1, chunks, mp3, &pcm, &gain_db));
            const double tail_db = measure_tail_rms_db(pcm, 48000);
            TEST_CHECK(tail_db >= -18.0 && tail_db <= -14.0);
            TEST_CHECK(gain_db >= 5.0f);
        }
    }

    std::filesystem::remove_all(temp_dir, fec);
    std::cout << "[PASS] TestRound60Features\n";
}

void TestTranscoderWaveAndBadMp4() {
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    // Build a valid 22050Hz mono 16-bit PCM WAV (2205 samples = 100ms)
    std::vector<std::uint8_t> wav_bytes(44 + 2205 * 2, 0);
    auto write_le32 = [&](std::size_t off, std::uint32_t v) {
        wav_bytes[off] = v & 0xFF;
        wav_bytes[off + 1] = (v >> 8) & 0xFF;
        wav_bytes[off + 2] = (v >> 16) & 0xFF;
        wav_bytes[off + 3] = (v >> 24) & 0xFF;
    };
    auto write_le16 = [&](std::size_t off, std::uint16_t v) {
        wav_bytes[off] = v & 0xFF;
        wav_bytes[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(wav_bytes.data(), "RIFF", 4);
    write_le32(4, static_cast<std::uint32_t>(wav_bytes.size() - 8));
    std::memcpy(wav_bytes.data() + 8, "WAVEfmt ", 8);
    write_le32(16, 16);
    write_le16(20, 1);     // PCM
    write_le16(22, 1);     // mono
    write_le32(24, 22050); // 22050 Hz -> LAME resamples to 48000 Hz
    write_le32(28, 22050 * 2);
    write_le16(32, 2);
    write_le16(34, 16);
    std::memcpy(wav_bytes.data() + 36, "data", 4);
    write_le32(40, 2205 * 2);
    for (int i = 0; i < 2205; ++i) {
        std::int16_t s = static_cast<std::int16_t>((i % 50) * 400);
        write_le16(44 + i * 2, static_cast<std::uint16_t>(s));
    }

    // Build progressive MP4 with ftyp + mdat BEFORE moov
    std::vector<std::uint8_t> bad_mp4 = {
        0,0,0,16, 'f','t','y','p', 'M','4','A',' ', 0,0,0,0,
        0,0,0,16, 'm','d','a','t', 1,2,3,4,5,6,7,8
    };

    asio::io_context ioc;
    asio::ip::tcp::acceptor acc(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};

    std::thread srv([&]() {
        for (int reqs = 0; reqs < 2 && !stop_srv.load(); ++reqs) {
            asio::ip::tcp::socket sock(ioc);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            std::istream is(&sbuf);
            std::string m, path;
            is >> m >> path;
            const auto& body = (path == "/bad.mp4") ? bad_mp4 : wav_bytes;
            std::string hdr = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n";
            asio::write(sock, asio::buffer(hdr), ec);
            asio::write(sock, asio::buffer(body), ec);
        }
    });

    // 1. Decode + LAME resample WAV
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/test.wav";
        meta.container_ext = "wav";
        TranscoderConfig cfg;
        cfg.target_sample_rate = 48000;
        cfg.target_bitrate_kbps = 128;
        cfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, cfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(buf->TotalWritten() > 100);
    }

    // 2. Reject non-faststart MP4 (moov after mdat)
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/bad.mp4";
        meta.container_ext = "mp4";
        TranscoderConfig cfg;
        cfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, cfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err));
    }

    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    HttpClient::SetSecurityPolicy({});
    std::cout << "[PASS] TestTranscoderWaveAndBadMp4\n";
}

void TestRingBufferGenerationCancellation() {
    AudioStreamBuffer buf(4096);
    std::uint64_t gen1 = buf.CurrentGeneration();
    const std::uint8_t track1[] = {'O', 'L', 'D', '1'};
    buf.Write(track1, sizeof(track1), gen1);

    // Channel switches track -> PrepareNextTrack advances generation and clears EOF
    std::uint64_t gen2 = buf.PrepareNextTrack();
    TEST_CHECK(gen2 > gen1);

    // Stale cancelled writer from gen1 attempts late write and MarkEof
    const std::uint8_t stale[] = {'S', 'T', 'A', 'L', 'E'};
    buf.Write(stale, sizeof(stale), gen1);
    buf.MarkEof(gen1);
    TEST_CHECK(!buf.IsEof());

    // Reader catches up to end of gen1, then receives new track data from gen2
    std::uint64_t off = 0;
    std::vector<std::uint8_t> out(64, 0);
    std::size_t n1 = buf.Read(off, out.data(), out.size(), 0);
    TEST_CHECK(n1 == sizeof(track1));
    TEST_CHECK(std::memcmp(out.data(), track1, sizeof(track1)) == 0);

    const std::uint8_t track2[] = {'N', 'E', 'W', '2', '!'};
    buf.Write(track2, sizeof(track2), gen2);
    TEST_CHECK(!buf.IsEof());

    std::size_t n2 = buf.Read(off, out.data(), out.size(), 0);
    TEST_CHECK(n2 == sizeof(track2));
    TEST_CHECK(std::memcmp(out.data(), track2, sizeof(track2)) == 0);

    // Also test RemoveWaiter boolean semantics
    auto wid = buf.RegisterWaiter([]() {});
    TEST_CHECK(wid != 0);
    TEST_CHECK(buf.RemoveWaiter(wid) == true);
    TEST_CHECK(buf.RemoveWaiter(wid) == false);

    // Also verify CurrentGenerationStartOffset prevents replaying previous track's tail on live channels
    TEST_CHECK(buf.PrepareNextTrack() > gen2);
    TEST_CHECK(buf.CurrentGenerationStartOffset() == buf.TotalWritten());

    HttpStreamingServer srv;
    std::string err;
    TEST_CHECK(srv.Start("127.0.0.1", 0, err));
    std::uint16_t port = srv.BoundPort();

    auto ch_buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
    std::vector<std::uint8_t> old_tail(1024, 'O');
    ch_buf->Write(old_tail.data(), old_tail.size(), ch_buf->CurrentGeneration());
    std::uint64_t next_gen = ch_buf->PrepareNextTrack();
    TEST_CHECK(ch_buf->CurrentGenerationStartOffset() == old_tail.size());

    StreamEndpointInfo ep;
    ep.token = "chantrans";
    ep.is_live_channel = true;
    ep.live_burst_bytes = 4096;
    ep.buffer = ch_buf;
    srv.RegisterEndpoint(ep);

    std::vector<std::uint8_t> new_track(64, 'N');
    ch_buf->Write(new_track.data(), new_track.size(), next_gen);
    ch_buf->MarkEof(next_gen);

    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
    std::string req = "GET /channel/chantrans.mp3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(sock, asio::buffer(req));

    std::string body;
    std::array<char, 2048> tmp{};
    std::error_code ec;
    while (true) {
        std::size_t n = sock.read_some(asio::buffer(tmp), ec);
        if (n > 0) body.append(tmp.data(), n);
        if (ec) break;
    }
    auto hdr_end = body.find("\r\n\r\n");
    TEST_CHECK(hdr_end != std::string::npos);
    std::string payload = body.substr(hdr_end + 4);
    TEST_CHECK(payload == std::string(new_track.size(), 'N'));
    srv.Stop();

    std::cout << "[PASS] TestRingBufferGenerationCancellation\n";
}

void TestBinaryManagerUpdateDecision() {
    // 1. Checked less than 24h ago -> skip regardless of old tag
    YtDlpUpdateDecisionInput recent;
    recent.now_epoch = 200000;
    recent.last_check_epoch = 190000;
    recent.current_tag = "2024.01.01";
    recent.latest_tag = "2026.11.01";
    TEST_CHECK(DecideYtDlpUpdateAction(recent) == YtDlpUpdateAction::SkipRecentlyChecked);

    // 2. Due for check, latest not yet fetched
    YtDlpUpdateDecisionInput due_check;
    due_check.now_epoch = 200000;
    due_check.last_check_epoch = 100000;
    due_check.current_tag = "2024.01.01";
    due_check.latest_tag = "";
    TEST_CHECK(DecideYtDlpUpdateAction(due_check) == YtDlpUpdateAction::CheckLatestVersion);

    // 3. Due for check, stored ytdlp_tag older than latest -> download & update
    YtDlpUpdateDecisionInput outdated = due_check;
    outdated.latest_tag = "2026.11.01";
    TEST_CHECK(DecideYtDlpUpdateAction(outdated) == YtDlpUpdateAction::DownloadAndUpdate);

    // 4. First run without tag (still unknown) -> download & update when latest differs
    YtDlpUpdateDecisionInput no_tag = outdated;
    no_tag.current_tag = "";
    TEST_CHECK(DecideYtDlpUpdateAction(no_tag) == YtDlpUpdateAction::DownloadAndUpdate);

    // 5. Up to date
    YtDlpUpdateDecisionInput up_to_date = outdated;
    up_to_date.current_tag = "2026.11.01";
    TEST_CHECK(DecideYtDlpUpdateAction(up_to_date) == YtDlpUpdateAction::UpToDate);

    std::cout << "[PASS] TestBinaryManagerUpdateDecision\n";
}

void TestRound2EngineChecks() {
    MockPlatformHost host;
    host.players = {{0, {0.0f, 0.0f, 0.0f}, 0, 0}};

    // 1. Explicit busy http_port must fail Initialize() cleanly instead of falling back to ephemeral port
    asio::io_context blocker_io;
    asio::ip::tcp::acceptor blocker(blocker_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t busy_port = blocker.local_endpoint().port();

    PluginConfig busy_cfg;
    busy_cfg.bind_ip = "127.0.0.1";
    busy_cfg.public_ip = "127.0.0.1";
    busy_cfg.http_port = busy_port;
    busy_cfg.auto_download_extractor = false;

    AudioEngine busy_engine(host, busy_cfg);
    TEST_CHECK(!busy_engine.Initialize());
    blocker.close();

    // 2. SA_PauseChannelAudioStream(id, false) must NOT transition Idle/Stopped channels to Playing
    PluginConfig ok_cfg;
    ok_cfg.bind_ip = "127.0.0.1";
    ok_cfg.public_ip = "127.0.0.1";
    ok_cfg.http_port = 0;
    ok_cfg.auto_download_extractor = false;
    ok_cfg.persist_channels = false;

    AudioEngine engine(host, ok_cfg);
    TEST_CHECK(engine.Initialize());
    ChannelId ch = engine.CreateChannel("test-ch");
    TEST_CHECK(ch != INVALID_CHANNEL_ID);
    TEST_CHECK(!engine.PauseChannel(ch, false));
    TEST_CHECK(engine.GetChannelState(ch) == StreamState::Idle);
    TEST_CHECK(!engine.PauseChannel(ch, true));
    // 4. SearchAudio + CancelSearch stops cleanly without crash
    {
        SearchId sid = engine.SearchAudio(0, "test query", 5);
        TEST_CHECK(sid != INVALID_SEARCH_ID);
        TEST_CHECK(engine.CancelSearch(0));
        TEST_CHECK(!engine.CancelSearch(0));
    }
    engine.Shutdown();

    // 3. Concurrency limit enforces streams + active channels <= max_concurrent_transcoders
    {
        PluginConfig lim_cfg;
        lim_cfg.bind_ip = "127.0.0.1";
        lim_cfg.public_ip = "127.0.0.1";
        lim_cfg.http_port = 0;
        lim_cfg.auto_download_extractor = false;
        lim_cfg.max_concurrent_streams = 10;
        lim_cfg.max_concurrent_transcoders = 1;
        lim_cfg.persist_channels = false;

        AudioEngine lim_engine(host, lim_cfg);
        TEST_CHECK(lim_engine.Initialize());
        ChannelId ch1 = lim_engine.CreateChannel("ch-lim-1");
        ChannelId ch2 = lim_engine.CreateChannel("ch-lim-2");
        TEST_CHECK(lim_engine.PlayChannel(ch1, "https://example.com/slow.mp3"));
        // Active channel holds the sole transcoder slot -> second play & extra personal stream rejected
        TEST_CHECK(!lim_engine.PlayChannel(ch2, "https://example.com/slow2.mp3"));
        TEST_CHECK(!lim_engine.QueueChannel(ch2, "https://example.com/q.mp3"));
        TEST_CHECK(lim_engine.QueueChannel(ch1, "https://example.com/q_ok.mp3"));
        StreamOptions transcoded_opts;
        transcoded_opts.process_direct_audio_files = true;
        TEST_CHECK(lim_engine.PlayAudioStreamForPlayer(0, "https://example.com/s.mp3", transcoded_opts) == INVALID_STREAM_ID);
        TEST_CHECK(lim_engine.StopChannel(ch1));
        TEST_CHECK(lim_engine.DestroyChannel(ch1));
        lim_engine.Shutdown();
    }

    std::cout << "[PASS] TestRound2EngineChecks\n";
}

void TestHttpServerSlowWriterAndDisconnect() {
    HttpStreamingServer srv;
    std::string err;
    TEST_CHECK(srv.Start("127.0.0.1", 0, err));
    std::uint16_t port = srv.BoundPort();

    auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
    StreamEndpointInfo ep;
    ep.token = "slowtoken";
    ep.title = "Slow Stream";
    ep.bitrate_kbps = 128;
    ep.buffer = buf;
    srv.RegisterEndpoint(ep);

    // 1. Client connects BEFORE first byte is written; writer catches up slowly in 3 waves
    {
        asio::io_context io;
        asio::ip::tcp::socket sock(io);
        sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
        std::string req = "GET /stream/slowtoken.mp3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(sock, asio::buffer(req));

        // Wait until connection is registered while buffer is completely empty (0 bytes written!)
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        TEST_CHECK(srv.GetActiveConnectionCount("slowtoken") == 1);

        const std::uint8_t chunk1[] = {'M', 'P', '3', '_', 'A'};
        buf->Write(chunk1, sizeof(chunk1));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        TEST_CHECK(srv.GetActiveConnectionCount("slowtoken") == 1);

        const std::uint8_t chunk2[] = {'B', 'C', 'D'};
        buf->Write(chunk2, sizeof(chunk2));
        buf->MarkEof();

        std::string body;
        std::array<char, 1024> tmp{};
        std::error_code ec;
        while (true) {
            std::size_t n = sock.read_some(asio::buffer(tmp), ec);
            if (n > 0) body.append(tmp.data(), n);
            if (ec) break;
        }
        TEST_CHECK(body.find("200 OK") != std::string::npos);
        TEST_CHECK(body.find("MP3_ABCD") != std::string::npos);
    }

    // 2. Client disconnects while waiting on empty/paused buffer -> server closes waiter promptly
    {
        auto paused_buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        StreamEndpointInfo ep2;
        ep2.token = "pausedtok";
        ep2.buffer = paused_buf;
        srv.RegisterEndpoint(ep2);

        {
            asio::io_context io;
            asio::ip::tcp::socket sock(io);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
            std::string req = "GET /stream/pausedtok.mp3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
            asio::write(sock, asio::buffer(req));
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            TEST_CHECK(srv.GetActiveConnectionCount("pausedtok") == 1);
            sock.close();
        }
        // Poll up to 400ms for StartDisconnectWatcher to detect EOF and close socket
        for (int i = 0; i < 20 && srv.GetActiveConnectionCount("pausedtok") > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(srv.GetActiveConnectionCount("pausedtok") == 0);
    }

    srv.Stop();
    std::cout << "[PASS] TestHttpServerSlowWriterAndDisconnect\n";
}

void TestRound4HttpPlayedOffsetAndStopTeardown() {
    HttpStreamingServer srv;
    std::string err;
    TEST_CHECK(srv.Start("127.0.0.1", 0, err));
    std::uint16_t port = srv.BoundPort();

    // 1. Played offset stays paced by elapsed time even when initial burst is huge
    auto burst_buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
    std::vector<std::uint8_t> chunk1(96 * 1024, 0xAB);
    burst_buf->Write(chunk1.data(), chunk1.size());

    StreamEndpointInfo ep;
    ep.token = "bursttok";
    ep.bitrate_kbps = 128;
    ep.buffer = burst_buf;
    srv.RegisterEndpoint(ep);

    asio::io_context client_io;
    asio::ip::tcp::socket open_sock(client_io);
    open_sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
    std::string req1 = "GET /stream/bursttok.mp3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(open_sock, asio::buffer(req1));

    std::size_t total_rx = 0;
    std::array<char, 8192> rx_buf{};
    while (total_rx < 96 * 1024) {
        total_rx += open_sock.read_some(asio::buffer(rx_buf));
    }

    // Cross the 1000ms throttle window, then deliver next chunk so progress updates past 96 KB
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    std::vector<std::uint8_t> chunk2(16 * 1024, 0xAB);
    burst_buf->Write(chunk2.data(), chunk2.size());
    while (total_rx < 112 * 1024) {
        total_rx += open_sock.read_some(asio::buffer(rx_buf));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::uint64_t served = srv.GetEndpointServedOffset("bursttok");
    std::uint64_t played = srv.GetEndpointPlayedOffset("bursttok");
    TEST_CHECK(served >= 96 * 1024);
    // Over ~1.1s at 128 kbps (~16 KB/s), played estimate must stay well below served burst
    TEST_CHECK(played > 8 * 1024 && played < 40 * 1024);

    // 2. Resumed connection adds initial_offset to elapsed playback estimate
    auto resume_buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
    std::vector<std::uint8_t> resume_data(96 * 1024, 0xCD);
    resume_buf->Write(resume_data.data(), resume_data.size());
    resume_buf->MarkEof();

    StreamEndpointInfo ep2;
    ep2.token = "resumetok";
    ep2.bitrate_kbps = 128;
    ep2.buffer = resume_buf;
    srv.RegisterEndpoint(ep2);

    {
        asio::io_context io2;
        asio::ip::tcp::socket sock2(io2);
        sock2.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
        std::string req2 = "GET /stream/resumetok.mp3?resume=32000 HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(sock2, asio::buffer(req2));
        std::error_code ec;
        while (sock2.read_some(asio::buffer(rx_buf), ec) > 0 && !ec) {
        }
    }
    std::uint64_t r_played = srv.GetEndpointPlayedOffset("resumetok");
    TEST_CHECK(r_played >= 32000);
    TEST_CHECK(r_played < 48000);

    // 3. Stop() while open_sock is still active on live buffer must not deadlock or crash
    TEST_CHECK(srv.GetActiveConnectionCount("bursttok") == 1);
    srv.Stop();
    TEST_CHECK(srv.GetActiveConnectionCount("bursttok") == 0);
    std::cout << "[PASS] TestRound4HttpPlayedOffsetAndStopTeardown\n";
}

void TestRound5ConcurrencyReplacementAndPreResolvePause() {
    // 44-byte RIFF/WAVE header (44100 Hz, mono, 16-bit PCM, 5s = 441000 PCM bytes)
    constexpr std::uint32_t kDataBytes = 441000;
    std::vector<std::uint8_t> wav_hdr(44, 0);
    auto put_le32 = [&](std::size_t off, std::uint32_t v) {
        wav_hdr[off] = v & 0xFF;
        wav_hdr[off + 1] = (v >> 8) & 0xFF;
        wav_hdr[off + 2] = (v >> 16) & 0xFF;
        wav_hdr[off + 3] = (v >> 24) & 0xFF;
    };
    auto put_le16 = [&](std::size_t off, std::uint16_t v) {
        wav_hdr[off] = v & 0xFF;
        wav_hdr[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(wav_hdr.data(), "RIFF", 4);
    put_le32(4, 36 + kDataBytes);
    std::memcpy(wav_hdr.data() + 8, "WAVEfmt ", 8);
    put_le32(16, 16);
    put_le16(20, 1);
    put_le16(22, 1);
    put_le32(24, 44100);
    put_le32(28, 44100 * 2);
    put_le16(32, 2);
    put_le16(34, 16);
    std::memcpy(wav_hdr.data() + 36, "data", 4);
    put_le32(40, kDataBytes);

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t wav_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex conn_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> conn_sockets;
    std::vector<std::thread> conn_threads;

    std::thread accept_thread([&]() {
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);

            std::lock_guard<std::mutex> lk(conn_mtx);
            conn_sockets.push_back(sock);
            conn_threads.emplace_back([&, sock]() {
                asio::streambuf sbuf;
                std::error_code r_ec;
                asio::read_until(*sock, sbuf, "\r\n\r\n", r_ec);
                if (r_ec) return;

                std::istream is(&sbuf);
                std::string method, path;
                is >> method >> path;
                if (method == "HEAD" && path.find("/slow_r6_stream") != std::string::npos) {
                    for (int ms = 0; ms < 1500 && !stop_srv.load(); ms += 25) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                } else if (path.find("/slow") != std::string::npos) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
                if (stop_srv.load()) return;

                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                  std::to_string(44 + kDataBytes) + "\r\nConnection: close\r\n\r\n";
                std::error_code w_ec;
                asio::write(*sock, asio::buffer(hdr), w_ec);
                if (w_ec || method == "HEAD") {
                    sock->close(w_ec);
                    return;
                }
                asio::write(*sock, asio::buffer(wav_hdr), w_ec);
                std::vector<std::uint8_t> pcm_chunk(4410, 0);
                for (std::uint32_t sent = 0; sent < kDataBytes && !stop_srv.load() && !w_ec; sent += static_cast<std::uint32_t>(pcm_chunk.size())) {
                    asio::write(*sock, asio::buffer(pcm_chunk), w_ec);
                    if (w_ec) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                sock->close(w_ec);
            });
        }
    });

    MockPlatformHost host;
    for (PlayerId pid = 0; pid < 3; ++pid) {
        PlayerSnapshot p;
        p.id = pid;
        host.players.push_back(p);
    }

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.max_concurrent_streams = 2;
    cfg.max_concurrent_transcoders = 2;
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    auto pump_until = [&](auto&& pred, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            engine.Tick();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        engine.Tick();
        return pred();
    };

    std::string base = "http://127.0.0.1:" + std::to_string(wav_port);
    StreamOptions opts;
    opts.volume = 50.0f; // Force proxy+transcode scheduler path

    // 1. At limit=2, two personal streams occupy both slots; player 0 can still replace repeatedly
    StreamId s0 = engine.PlayAudioStreamForPlayer(0, base + "/t0.wav", opts);
    StreamId s1 = engine.PlayAudioStreamForPlayer(1, base + "/t1.wav", opts);
    TEST_CHECK(s0 != INVALID_STREAM_ID && s1 != INVALID_STREAM_ID);
    TEST_CHECK(pump_until([&]() {
        TrackMetadata m0, m1;
        StreamState st0 = StreamState::Idle, st1 = StreamState::Idle;
        return engine.GetStreamInfo(s0, m0, st0) && st0 == StreamState::Playing &&
               engine.GetStreamInfo(s1, m1, st1) && st1 == StreamState::Playing;
    }, 2000));

    TEST_CHECK(engine.PlayAudioStreamForPlayer(2, base + "/t2_reject.wav", opts) == INVALID_STREAM_ID);

    for (int i = 0; i < 4; ++i) {
        s0 = engine.PlayAudioStreamForPlayer(0, base + "/t0_rep_" + std::to_string(i) + ".wav", opts);
        TEST_CHECK(s0 != INVALID_STREAM_ID);
        TEST_CHECK(pump_until([&]() {
            TrackMetadata m0;
            StreamState st0 = StreamState::Idle;
            return engine.GetStreamInfo(s0, m0, st0) && st0 == StreamState::Playing;
        }, 2000));
    }

    // 2. At limit=2 with 1 stream + 1 active channel, SkipChannel succeeds & shrinks queue by 1;
    // failed SkipChannel when over limit preserves queued front track.
    TEST_CHECK(engine.StopAudioStreamForPlayer(0));

    ChannelId ch1 = engine.CreateChannel("r5-ch1");
    TEST_CHECK(ch1 != INVALID_CHANNEL_ID);
    // Pump Tick() so StopAudioStreamForPlayer(0)'s cancelled transcoder exits its worker slot cleanly
    TEST_CHECK(pump_until([&]() {
        return engine.PlayChannel(ch1, base + "/ch1_1.wav");
    }, 2000));
    TEST_CHECK(engine.QueueChannel(ch1, base + "/ch1_2.wav"));
    TEST_CHECK(engine.QueueChannel(ch1, base + "/ch1_3.wav"));
    TEST_CHECK(pump_until([&]() {
        return engine.GetChannelState(ch1) == StreamState::Playing;
    }, 2000));

    TEST_CHECK(engine.SkipChannel(ch1));
    TEST_CHECK(pump_until([&]() {
        return engine.GetChannelState(ch1) == StreamState::Playing;
    }, 2000));
    TEST_CHECK(engine.SkipChannel(ch1));
    // Queue had 2 items; 3rd skip stops the active track cleanly, 4th skip returns false (nothing playing)
    TEST_CHECK(engine.SkipChannel(ch1));
    TEST_CHECK(engine.GetChannelState(ch1) == StreamState::Stopped);
    TEST_CHECK(!engine.SkipChannel(ch1));

    // At limit=2 with 2 streams playing, idle ch2 QueueChannel refuses without losing/corrupting state,
    // and succeeds once a slot opens.
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    for (int t = 0; t < 15; ++t) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    StreamId s0_fill = engine.PlayAudioStreamForPlayer(0, base + "/s0_fill.wav", opts);
    TEST_CHECK(s0_fill != INVALID_STREAM_ID);
    s1 = engine.PlayAudioStreamForPlayer(1, base + "/fill.wav", opts);
    TEST_CHECK(s1 != INVALID_STREAM_ID);
    TEST_CHECK(pump_until([&]() {
        TrackMetadata m0, m1;
        StreamState st0 = StreamState::Idle, st1 = StreamState::Idle;
        return engine.GetStreamInfo(s0_fill, m0, st0) && st0 == StreamState::Playing &&
               engine.GetStreamInfo(s1, m1, st1) && st1 == StreamState::Playing;
    }, 2000));

    ChannelId ch2 = engine.CreateChannel("r5-ch2");
    TEST_CHECK(ch2 != INVALID_CHANNEL_ID);
    TEST_CHECK(!engine.QueueChannel(ch2, base + "/keep.wav"));
    TEST_CHECK(engine.StopAudioStreamForPlayer(0));
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    TEST_CHECK(pump_until([&]() {
        return engine.QueueChannel(ch2, base + "/keep.wav"); // Starts immediately once slot opens!
    }, 2000));

    TEST_CHECK(engine.StopChannel(ch1));
    TEST_CHECK(engine.StopChannel(ch2));
    TEST_CHECK(engine.DestroyChannel(ch1));
    TEST_CHECK(engine.DestroyChannel(ch2));
    for (int t = 0; t < 15; ++t) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 3. Pausing personal stream during Resolving stays Paused after resolve finishes; unpause starts playback
    StreamId slow_sid = engine.PlayAudioStreamForPlayer(2, base + "/slow_track.wav", opts);
    TEST_CHECK(slow_sid != INVALID_STREAM_ID);
    TEST_CHECK(engine.PauseAudioStreamForPlayer(2, true));

    // Wait well past the 80ms /slow server header delay and pump Tick()
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    engine.Tick();

    TrackMetadata slow_meta;
    StreamState slow_state = StreamState::Idle;
    TEST_CHECK(engine.GetStreamInfo(slow_sid, slow_meta, slow_state));
    TEST_CHECK(slow_state == StreamState::Paused);

    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }
    TEST_CHECK(engine.PauseAudioStreamForPlayer(2, false));
    TEST_CHECK(pump_until([&]() {
        return engine.GetStreamInfo(slow_sid, slow_meta, slow_state) && slow_state == StreamState::Playing;
    }, 2000));
    bool saw_play_after_unpause = false;
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        for (const auto& ev : host.events) {
            if (ev.player_id == 2 && ev.play && !ev.url.empty()) {
                saw_play_after_unpause = true;
            }
        }
    }
    TEST_CHECK(saw_play_after_unpause);

    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, wav_port);
    if (accept_thread.joinable()) accept_thread.join();
    CloseTestSockets(conn_sockets, conn_mtx);
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lk(conn_mtx);
        to_join.swap(conn_threads);
    }
    for (auto& t : to_join) {
        if (t.joinable()) t.join();
    }
    std::cout << "[PASS] TestRound5ConcurrencyReplacementAndPreResolvePause\n";
}

void TestRound6UnpauseBeforeReady() {
    constexpr std::uint32_t kDataBytes = 441000;
    std::vector<std::uint8_t> wav_hdr(44, 0);
    auto put_le32 = [&](std::size_t off, std::uint32_t v) {
        wav_hdr[off] = v & 0xFF;
        wav_hdr[off + 1] = (v >> 8) & 0xFF;
        wav_hdr[off + 2] = (v >> 16) & 0xFF;
        wav_hdr[off + 3] = (v >> 24) & 0xFF;
    };
    auto put_le16 = [&](std::size_t off, std::uint16_t v) {
        wav_hdr[off] = v & 0xFF;
        wav_hdr[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(wav_hdr.data(), "RIFF", 4);
    put_le32(4, 36 + kDataBytes);
    std::memcpy(wav_hdr.data() + 8, "WAVEfmt ", 8);
    put_le32(16, 16);
    put_le16(20, 1);
    put_le16(22, 1);
    put_le32(24, 44100);
    put_le32(28, 44100 * 2);
    put_le16(32, 2);
    put_le16(34, 16);
    std::memcpy(wav_hdr.data() + 36, "data", 4);
    put_le32(40, kDataBytes);

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t wav_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex conn_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> conn_sockets;
    std::vector<std::thread> conn_threads;

    std::thread accept_thread([&]() {
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);

            std::lock_guard<std::mutex> lk(conn_mtx);
            conn_sockets.push_back(sock);
            conn_threads.emplace_back([&, sock]() {
                asio::streambuf sbuf;
                std::error_code r_ec;
                asio::read_until(*sock, sbuf, "\r\n\r\n", r_ec);
                if (r_ec) return;

                std::istream is(&sbuf);
                std::string method, path;
                is >> method >> path;
                if (method == "HEAD" && path.find("/slow_r6_stream") != std::string::npos) {
                    for (int ms = 0; ms < 1500 && !stop_srv.load(); ms += 25) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                } else if (path.find("/slow") != std::string::npos) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
                if (stop_srv.load()) return;
                if (path.find("/missing") != std::string::npos) {
                    std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    std::error_code w_ec;
                    asio::write(*sock, asio::buffer(nf), w_ec);
                    sock->close(w_ec);
                    return;
                }

                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                  std::to_string(44 + kDataBytes) + "\r\nConnection: close\r\n\r\n";
                std::error_code w_ec;
                asio::write(*sock, asio::buffer(hdr), w_ec);
                if (w_ec || method == "HEAD") {
                    sock->close(w_ec);
                    return;
                }
                asio::write(*sock, asio::buffer(wav_hdr), w_ec);
                std::vector<std::uint8_t> pcm_chunk(4410, 0);
                for (std::uint32_t sent = 0; sent < kDataBytes && !stop_srv.load() && !w_ec; sent += static_cast<std::uint32_t>(pcm_chunk.size())) {
                    asio::write(*sock, asio::buffer(pcm_chunk), w_ec);
                    if (w_ec) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                sock->close(w_ec);
            });
        }
    });

    MockPlatformHost host;
    for (PlayerId pid = 0; pid < 3; ++pid) {
        PlayerSnapshot p;
        p.id = pid;
        host.players.push_back(p);
    }

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.allow_private_network_urls = true;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    std::string base = "http://127.0.0.1:" + std::to_string(wav_port);
    StreamOptions opts;
    opts.volume = 50.0f; // Ensure proxy+transcode path

    // 1. Pause -> Unpause immediately during Resolving must restore Resolving (not Playing)
    // and defer SendPlayAudioStream until OnAudioStreamReady runs once.
    // Use a URL without a known audio extension so StreamResolver must perform a HEAD probe held by the server.
    StreamId sid_res = engine.PlayAudioStreamForPlayer(0, base + "/slow_r6_stream", opts);
    TEST_CHECK(sid_res != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
        host.ready_stream_ids.clear();
    }
    TEST_CHECK(engine.PauseAudioStreamForPlayer(0, true));
    TrackMetadata meta;
    StreamState st = StreamState::Idle;
    TEST_CHECK(engine.GetStreamInfo(sid_res, meta, st) && st == StreamState::Paused);
    TEST_CHECK(engine.PauseAudioStreamForPlayer(0, false));
    TEST_CHECK(engine.GetStreamInfo(sid_res, meta, st) && st == StreamState::Resolving);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.empty());
        TEST_CHECK(host.ready_stream_ids.empty());
    }

    auto pump_until = [&](auto&& pred, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            engine.Tick();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        engine.Tick();
        return pred();
    };

    TEST_CHECK(pump_until([&]() {
        TrackMetadata m;
        StreamState s = StreamState::Idle;
        return engine.GetStreamInfo(sid_res, m, s) && s == StreamState::Playing;
    }, 4000));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        int ready_count = static_cast<int>(std::count(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid_res));
        TEST_CHECK(ready_count == 1);
        bool saw_play = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 0 && ev.play && ev.url.find("/stream/") != std::string::npos) {
                saw_play = true;
            }
        }
        TEST_CHECK(saw_play);
    }

    // 2. Pause -> Unpause while in Buffering (worker set buffer, Tick hasn't fired Ready yet)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
        host.ready_stream_ids.clear();
    }
    StreamId sid_buf = engine.PlayAudioStreamForPlayer(1, base + "/fast_r6.wav", opts);
    TEST_CHECK(sid_buf != INVALID_STREAM_ID);
    auto buf_wait_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < buf_wait_end) {
        if (engine.GetStreamInfo(sid_buf, meta, st) && (st == StreamState::Buffering || st == StreamState::Error)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TEST_CHECK(st == StreamState::Buffering);

    TEST_CHECK(engine.PauseAudioStreamForPlayer(1, true));
    TEST_CHECK(engine.GetStreamInfo(sid_buf, meta, st) && st == StreamState::Paused);
    TEST_CHECK(engine.PauseAudioStreamForPlayer(1, false));
    TEST_CHECK(engine.GetStreamInfo(sid_buf, meta, st) && st == StreamState::Buffering);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.empty());
        TEST_CHECK(host.ready_stream_ids.empty());
    }

    TEST_CHECK(pump_until([&]() {
        TrackMetadata m;
        StreamState s = StreamState::Idle;
        return engine.GetStreamInfo(sid_buf, m, s) && s == StreamState::Playing;
    }, 4000));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        int ready_count = static_cast<int>(std::count(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid_buf));
        TEST_CHECK(ready_count == 1);
        bool saw_play = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 1 && ev.play && ev.url.find("/stream/") != std::string::npos) {
                saw_play = true;
            }
        }
        TEST_CHECK(saw_play);
    }

    // 3. Refuse PauseAudioStreamForPlayer when stream is Error or Stopped
    StreamId sid_err = engine.PlayAudioStreamForPlayer(2, base + "/missing.wav", opts);
    TEST_CHECK(sid_err != INVALID_STREAM_ID);
    auto err_wait_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < err_wait_end) {
        if (engine.GetStreamInfo(sid_err, meta, st) && st == StreamState::Error) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    TEST_CHECK(st == StreamState::Error);
    TEST_CHECK(!engine.PauseAudioStreamForPlayer(2, false));
    TEST_CHECK(!engine.PauseAudioStreamForPlayer(2, true));
    TEST_CHECK(engine.GetStreamInfo(sid_err, meta, st) && st == StreamState::Error);

    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, wav_port);
    if (accept_thread.joinable()) accept_thread.join();
    CloseTestSockets(conn_sockets, conn_mtx);
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lk(conn_mtx);
        to_join.swap(conn_threads);
    }
    for (auto& t : to_join) {
        if (t.joinable()) t.join();
    }
    std::cout << "[PASS] TestRound6UnpauseBeforeReady\n";
}

void TestRound8Fase1MalformedMedia() {
    auto make_box = [](const char fourcc[4], const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> out(8 + body.size());
        std::uint32_t total = static_cast<std::uint32_t>(out.size());
        out[0] = (total >> 24) & 0xFF;
        out[1] = (total >> 16) & 0xFF;
        out[2] = (total >> 8) & 0xFF;
        out[3] = total & 0xFF;
        std::memcpy(out.data() + 4, fourcc, 4);
        if (!body.empty()) {
            std::memcpy(out.data() + 8, body.data(), body.size());
        }
        return out;
    };
    auto append = [](std::vector<std::uint8_t>& dst, const std::vector<std::uint8_t>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
    };

    std::vector<std::uint8_t> ftyp = make_box("ftyp", {'M','4','A',' ', 0,0,0,0, 'M','4','A',' '});

    // stsz fixed-size with count = 0xFFFFFFF0 (sample_size = 128)
    std::vector<std::uint8_t> mal_fixed = ftyp;
    {
        std::vector<std::uint8_t> stsz_body = {
            0,0,0,0,             // version+flags
            0,0,0,128,           // sample_size = 128
            0xFF,0xFF,0xFF,0xF0  // count = 0xFFFFFFF0
        };
        auto stsz = make_box("stsz", stsz_body);
        auto stbl = make_box("stbl", stsz);
        auto minf = make_box("minf", stbl);
        auto mdia = make_box("mdia", minf);
        auto trak = make_box("trak", mdia);
        auto moov = make_box("moov", trak);
        append(mal_fixed, moov);
        append(mal_fixed, make_box("mdat", {1, 2, 3, 4}));
    }

    // stsz table mode with 100 MB sample size entry
    std::vector<std::uint8_t> mal_big = ftyp;
    {
        std::uint32_t big_sz = 100u * 1024u * 1024u;
        std::vector<std::uint8_t> stsz_body = {
            0,0,0,0,
            0,0,0,0,     // sample_size = 0 (table mode)
            0,0,0,1,     // count = 1
            static_cast<std::uint8_t>((big_sz >> 24) & 0xFF),
            static_cast<std::uint8_t>((big_sz >> 16) & 0xFF),
            static_cast<std::uint8_t>((big_sz >> 8) & 0xFF),
            static_cast<std::uint8_t>(big_sz & 0xFF)
        };
        auto stsz = make_box("stsz", stsz_body);
        auto stbl = make_box("stbl", stsz);
        auto minf = make_box("minf", stbl);
        auto mdia = make_box("mdia", minf);
        auto trak = make_box("trak", mdia);
        auto moov = make_box("moov", trak);
        append(mal_big, moov);
        append(mal_big, make_box("mdat", {1, 2, 3, 4}));
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::istream is(&sbuf);
            std::string method, path;
            is >> method >> path;

            const auto& payload = (path.find("big") != std::string::npos) ? mal_big : mal_fixed;
            std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mp4\r\nContent-Length: " +
                              std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
            asio::write(sock, asio::buffer(hdr), ec);
            if (!ec && method != "HEAD") {
                asio::write(sock, asio::buffer(payload), ec);
            }
            sock.close(ec);
        }
    });

    for (const char* path : {"/mal_fixed.m4a", "/mal_big.m4a"}) {
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + path;
        meta.container_ext = "m4a";
        TranscoderConfig cfg;
        cfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, cfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err));
        TEST_CHECK(!err.empty());
    }
    HttpClient::SetSecurityPolicy({});

    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound8Fase1MalformedMedia\n";
}

void TestRound8Fase2Ssrf() {
    // 1. Default policy rejects loopback, RFC 1918, link-local, CGNAT, 0.0.0.0/8, IPv6 private, non-HTTP(S)
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{false, "0.0.0.0", "203.0.113.10", 7778});
    std::string err;
    for (const char* bad_url : {
             "http://127.0.0.1:7778/stream/x.mp3",
             "http://127.0.0.1/test.mp3",
             "http://localhost/test.mp3",
             "http://10.0.0.5/radio.mp3",
             "http://172.16.5.4/radio.mp3",
             "http://192.168.1.50/radio.mp3",
             "http://169.254.169.254/latest/meta-data/",
             "http://100.64.0.1/internal",
             "http://0.0.0.0:8080/a.mp3",
             "http://224.0.0.1/a.mp3",
             "http://[::1]/a.mp3",
             "http://[fc00::1]/a.mp3",
             "http://[fe80::1]/a.mp3",
             "http://user@127.0.0.1/a.mp3",
             "ftp://example.com/a.mp3",
             "gopher://127.0.0.1:6379/_INFO",
             "file:///etc/passwd"
         }) {
        err.clear();
        TEST_CHECK(!HttpClient::ValidateRemoteUrl(bad_url, err));
        TEST_CHECK(err == "URL not allowed");
    }

    // 2. When allow_private_network_urls = true, LAN radios work while plugin self-endpoint stays blocked
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "0.0.0.0", "203.0.113.10", 7778});
    TEST_CHECK(HttpClient::ValidateRemoteUrl("http://192.168.1.50:8000/radio.mp3", err));
    TEST_CHECK(!HttpClient::ValidateRemoteUrl("http://127.0.0.1:7778/stream/x.mp3", err));
    TEST_CHECK(!HttpClient::ValidateRemoteUrl("http://203.0.113.10:7778/stream/x.mp3", err));

    // 3. Redirect chain check: 302 to self-endpoint rejected, >5 redirects rejected, relative redirect works
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::istream is(&sbuf);
            std::string method, path;
            is >> method >> path;

            std::string resp;
            if (path == "/redir_self") {
                resp = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:7778/stream/x.mp3\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else if (path == "/redir_loop") {
                resp = "HTTP/1.1 302 Found\r\nLocation: /redir_loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else if (path == "/redir_ok") {
                resp = "HTTP/1.1 302 Found\r\nLocation: /final_ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else {
                resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            }
            asio::write(sock, asio::buffer(resp), ec);
            sock.close(ec);
        }
    });

    std::string redir_base = "http://127.0.0.1:" + std::to_string(port);
    auto r_self = HttpClient::Get(redir_base + "/redir_self");
    TEST_CHECK(!r_self.Ok());
    TEST_CHECK(r_self.error == "URL not allowed");

    auto r_loop = HttpClient::Get(redir_base + "/redir_loop");
    TEST_CHECK(!r_loop.Ok());
    TEST_CHECK(!r_loop.error.empty());

    auto r_ok = HttpClient::Get(redir_base + "/redir_ok");
    TEST_CHECK(r_ok.Ok() && r_ok.body == "OK");

    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();

    // 4. End-to-end AudioEngine with default allow_private_network_urls=false emits SA_ERR_INVALID_URL
    MockPlatformHost host;
    host.players.push_back(PlayerSnapshot{0, {}, 0, 0});
    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "203.0.113.10";
    cfg.http_port = 0; // defaults to 7778
    cfg.auto_download_extractor = false;
    TEST_CHECK(!cfg.allow_private_network_urls); // Default is false

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    for (const char* url : {
             "http://127.0.0.1:7778/stream/x.mp3",
             "http://169.254.169.254/latest/meta-data/",
             "ftp://127.0.0.1/evil.mp3"
         }) {
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.error_events.clear();
        }
        StreamId sid = engine.PlayAudioStreamForPlayer(0, url, StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        bool got_invalid_url = false;
        while (std::chrono::steady_clock::now() < deadline && !got_invalid_url) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& ev : host.error_events) {
                    if (ev.stream_id == sid &&
                        ev.error_code == static_cast<std::int32_t>(ErrorCode::InvalidUrl) &&
                        ev.message == "URL not allowed") {
                        got_invalid_url = true;
                    }
                }
            }
            if (!got_invalid_url) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST_CHECK(got_invalid_url);
    }
    engine.Shutdown();
    std::cout << "[PASS] TestRound8Fase2Ssrf\n";
}

void TestRound8Fase3Fixes() {
    // 1. Fase 3.3: LoadServerCfg falls back to open.mp config.json (network.port / network.bind)
    {
        std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / "sa_omp_cfg_fallback_test";
        std::filesystem::create_directories(tmp_dir);
        std::filesystem::path orig_cwd = std::filesystem::current_path();
        struct CwdGuard {
            std::filesystem::path prev;
            std::filesystem::path dir;
            ~CwdGuard() {
                std::error_code ec;
                std::filesystem::current_path(prev, ec);
                std::filesystem::remove_all(dir, ec);
            }
        } cwd_guard{orig_cwd, tmp_dir};

        std::filesystem::current_path(tmp_dir);
        {
            std::ofstream ofs(tmp_dir / "config.json");
            ofs << R"({"network":{"port":7790,"bind":"127.0.0.1"}})";
        }
        ServerCfgInfo cfg_info = ConfigManager::LoadServerCfg("missing_server.cfg");
        TEST_CHECK(cfg_info.has_port && cfg_info.port == 7790);
        TEST_CHECK(cfg_info.has_bind && cfg_info.bind_ip == "127.0.0.1");
    }

    // 2. Fase 3.2: Network drop mid-track returns ErrorCode::NetworkError, and Range resume recovers
    std::vector<std::uint8_t> wav_bytes(44 + 4410 * 2, 0); // 200ms 22050Hz mono 16-bit WAV
    auto put_le32 = [&](std::size_t off, std::uint32_t v) {
        wav_bytes[off] = v & 0xFF;
        wav_bytes[off + 1] = (v >> 8) & 0xFF;
        wav_bytes[off + 2] = (v >> 16) & 0xFF;
        wav_bytes[off + 3] = (v >> 24) & 0xFF;
    };
    auto put_le16 = [&](std::size_t off, std::uint16_t v) {
        wav_bytes[off] = v & 0xFF;
        wav_bytes[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(wav_bytes.data(), "RIFF", 4);
    put_le32(4, static_cast<std::uint32_t>(wav_bytes.size() - 8));
    std::memcpy(wav_bytes.data() + 8, "WAVEfmt ", 8);
    put_le32(16, 16);
    put_le16(20, 1);
    put_le16(22, 1);
    put_le32(24, 22050);
    put_le32(28, 22050 * 2);
    put_le16(32, 2);
    put_le16(34, 16);
    std::memcpy(wav_bytes.data() + 36, "data", 4);
    put_le32(40, 4410 * 2);
    for (int i = 0; i < 4410; ++i) {
        std::int16_t s = static_cast<std::int16_t>((i % 60) * 350);
        put_le16(44 + static_cast<std::size_t>(i) * 2, static_cast<std::uint16_t>(s));
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> resume_hits{0};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            std::size_t range_start = 0;
            bool has_range = false;
            auto rpos = raw_req.find("Range: bytes=");
            if (rpos != std::string::npos) {
                has_range = true;
                range_start = static_cast<std::size_t>(std::strtoul(raw_req.c_str() + rpos + 13, nullptr, 10));
            }

            if (path == "/drop_no_range.wav") {
                if (has_range) {
                    std::string err_resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    asio::write(sock, asio::buffer(err_resp), ec);
                } else {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                      std::to_string(wav_bytes.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(sock, asio::buffer(hdr), ec);
                    // Drop TCP halfway after valid header + partial audio
                    asio::write(sock, asio::buffer(wav_bytes.data(), wav_bytes.size() / 2), ec);
                }
            } else if (path == "/drop_with_range.wav") {
                if (!has_range) {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                      std::to_string(wav_bytes.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(sock, asio::buffer(hdr), ec);
                    asio::write(sock, asio::buffer(wav_bytes.data(), 2000), ec);
                } else {
                    resume_hits.fetch_add(1);
                    if (range_start < wav_bytes.size()) {
                        std::size_t rem = wav_bytes.size() - range_start;
                        std::string hdr = "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                          std::to_string(rem) + "\r\nConnection: close\r\n\r\n";
                        asio::write(sock, asio::buffer(hdr), ec);
                        asio::write(sock, asio::buffer(wav_bytes.data() + range_start, rem), ec);
                    }
                }
            } else if (path == "/slow_ready.wav") {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                  std::to_string(wav_bytes.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && method != "HEAD") {
                    asio::write(sock, asio::buffer(wav_bytes), ec);
                }
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/drop_no_range.wav";
        meta.container_ext = "wav";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(ec == ErrorCode::NetworkError);
    }
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/drop_with_range.wav";
        meta.container_ext = "wav";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(resume_hits.load() >= 1);
        TEST_CHECK(buf->TotalWritten() > 100);
    }
    HttpClient::SetSecurityPolicy({});

    // 3. Fase 3.1 + 3.4: StopAudioStreamForPlayer(id, false) detaches cleanly without SendStopAudioStream,
    // and GetPlayerIp is never invoked off the main thread.
    {
        MockPlatformHost host;
        host.players.push_back(PlayerSnapshot{0, {}, 0, 0});
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.allow_private_network_urls = true;

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        StreamOptions opts;
        opts.volume = 50.0f;
        StreamId sid = engine.PlayAudioStreamForPlayer(0, "http://127.0.0.1:" + std::to_string(port) + "/slow_ready.wav", opts);
        TEST_CHECK(sid != INVALID_STREAM_ID);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.events.clear();
        }
        TEST_CHECK(engine.StopAudioStreamForPlayer(0, false));
        TrackMetadata m;
        StreamState s = StreamState::Idle;
        TEST_CHECK(!engine.GetStreamInfo(sid, m, s));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            for (const auto& ev : host.events) {
                TEST_CHECK(ev.play); // No SendStopAudioStream emitted when send_stop == false
            }
        }
        TEST_CHECK(!host.get_ip_off_main_thread.load());
        engine.Shutdown();
    }

    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound8Fase3Fixes\n";
}

void TestRound9EarlyDecoderCloseNotNetworkError() {
    // 1. Valid WAV followed by a 2 MB LIST chunk (> 512 KB PullByteStream buffer)
    constexpr std::uint32_t kPcmSamples = 4410;
    constexpr std::uint32_t kDataSize = kPcmSamples * 2;
    constexpr std::uint32_t kListPayload = 2 * 1024 * 1024;
    std::vector<std::uint8_t> wav_with_list(44 + kDataSize + 8 + kListPayload, 0);
    auto put_le32 = [&](std::size_t off, std::uint32_t v) {
        wav_with_list[off] = v & 0xFF;
        wav_with_list[off + 1] = (v >> 8) & 0xFF;
        wav_with_list[off + 2] = (v >> 16) & 0xFF;
        wav_with_list[off + 3] = (v >> 24) & 0xFF;
    };
    auto put_le16 = [&](std::size_t off, std::uint16_t v) {
        wav_with_list[off] = v & 0xFF;
        wav_with_list[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(wav_with_list.data(), "RIFF", 4);
    put_le32(4, static_cast<std::uint32_t>(wav_with_list.size() - 8));
    std::memcpy(wav_with_list.data() + 8, "WAVEfmt ", 8);
    put_le32(16, 16);
    put_le16(20, 1);
    put_le16(22, 1);
    put_le32(24, 22050);
    put_le32(28, 22050 * 2);
    put_le16(32, 2);
    put_le16(34, 16);
    std::memcpy(wav_with_list.data() + 36, "data", 4);
    put_le32(40, kDataSize);
    for (std::uint32_t i = 0; i < kPcmSamples; ++i) {
        std::int16_t s = static_cast<std::int16_t>((i % 50) * 400);
        put_le16(44 + static_cast<std::size_t>(i) * 2, static_cast<std::uint16_t>(s));
    }
    std::size_t list_off = 44 + kDataSize;
    std::memcpy(wav_with_list.data() + list_off, "LIST", 4);
    put_le32(list_off + 4, kListPayload);

    // 2. Bad non-faststart MP4 with 1 MB payload so downloader blocks in Push when decoder returns false
    std::vector<std::uint8_t> mp4_with_trail(16 + 8 + 1024 * 1024, 0);
    {
        const std::uint8_t ftyp[] = {0,0,0,16, 'f','t','y','p', 'M','4','A',' ', 0,0,0,0};
        std::memcpy(mp4_with_trail.data(), ftyp, 16);
        std::uint32_t mdat_sz = static_cast<std::uint32_t>(8 + 1024 * 1024);
        mp4_with_trail[16] = (mdat_sz >> 24) & 0xFF;
        mp4_with_trail[17] = (mdat_sz >> 16) & 0xFF;
        mp4_with_trail[18] = (mdat_sz >> 8) & 0xFF;
        mp4_with_trail[19] = mdat_sz & 0xFF;
        std::memcpy(mp4_with_trail.data() + 20, "mdat", 4);
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> range_hits{0};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            if (raw_req.find("Range:") != std::string::npos) {
                range_hits.fetch_add(1);
            }
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            const auto& payload = (path.find("mp4") != std::string::npos) ? mp4_with_trail : wav_with_list;
            std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                              std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
            asio::write(sock, asio::buffer(hdr), ec);
            if (!ec) {
                // Write in 32 KB chunks so early Close() interrupts transfer cleanly
                for (std::size_t off = 0; off < payload.size() && !stop_srv.load() && !ec;) {
                    std::size_t n = std::min<std::size_t>(32 * 1024, payload.size() - off);
                    asio::write(sock, asio::buffer(payload.data() + off, n), ec);
                    off += n;
                }
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/list_trail.wav";
        meta.container_ext = "wav";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(buf->TotalWritten() > 100);
        TEST_CHECK(range_hits.load() == 0);
    }
    {
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/early_fail.mp4";
        meta.container_ext = "mp4";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(ec == ErrorCode::TranscodeError);
        TEST_CHECK(range_hits.load() == 0);
    }
    HttpClient::SetSecurityPolicy({});

    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound9EarlyDecoderCloseNotNetworkError\n";
}

void TestRound10HlsLiveToleranceAndVod() {
    // Build a valid MPEG-TS segment containing two 417-byte MPEG-1 Layer III (128kbps 44100Hz) frames
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF;
    mp3_frame[1] = 0xFB;
    mp3_frame[2] = 0x90;
    mp3_frame[3] = 0x64;

    std::vector<std::uint8_t> pes_payload = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
    for (int f = 0; f < 4; ++f) {
        pes_payload.insert(pes_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }

    std::vector<std::uint8_t> ts_segment;
    for (std::size_t off = 0, pkt_idx = 0; off < pes_payload.size(); ++pkt_idx) {
        std::array<std::uint8_t, 188> pkt{};
        pkt.fill(0xFF);
        pkt[0] = 0x47;
        pkt[1] = (pkt_idx == 0 ? 0x41 : 0x01); // PUSI on first packet, PID = 0x0100
        pkt[2] = 0x00;
        std::size_t rem = pes_payload.size() - off;
        if (rem >= 184) {
            pkt[3] = static_cast<std::uint8_t>(0x10 | (pkt_idx & 0x0F));
            std::memcpy(pkt.data() + 4, pes_payload.data() + off, 184);
            off += 184;
        } else {
            // Exact MPEG-TS adaptation stuffing so PES payload has zero trailing garbage bytes
            std::uint8_t adapt_len = static_cast<std::uint8_t>(183 - rem);
            pkt[3] = static_cast<std::uint8_t>(0x30 | (pkt_idx & 0x0F));
            pkt[4] = adapt_len;
            if (adapt_len > 0) pkt[5] = 0x00;
            std::memcpy(pkt.data() + 5 + adapt_len, pes_payload.data() + off, rem);
            off += rem;
        }
        ts_segment.insert(ts_segment.end(), pkt.begin(), pkt.end());
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> live_poll{0};
    std::atomic<int> die_poll{0};
    std::atomic<int> retry_seg_hits{0};
    std::atomic<int> seg9_hits{0};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            auto send_text = [&](int code, const char* ct, const std::string& body) {
                std::string hdr = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Err") +
                                  "\r\nContent-Type: " + ct + "\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && !body.empty()) asio::write(sock, asio::buffer(body), ec);
            };

            if (path == "/live.m3u8") {
                int p = live_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n";
                if (p == 0) {
                    pl << "#EXT-X-MEDIA-SEQUENCE:0\n";
                    for (int s = 1; s <= 10; ++s) {
                        pl << "#EXTINF:1.0,\n/seg_" << s << ".ts\n";
                    }
                } else {
                    pl << "#EXT-X-MEDIA-SEQUENCE:10\n";
                    for (int s = 11; s <= 21; ++s) {
                        pl << "#EXTINF:1.0,\n/seg_" << s << ".ts\n";
                    }
                    pl << "#EXT-X-ENDLIST\n";
                }
                send_text(200, "application/vnd.apple.mpegurl", pl.str());
            } else if (path == "/seg_9.ts") {
                seg9_hits.fetch_add(1);
                send_text(404, "text/plain", "");
            } else if (path == "/perm404.ts") {
                send_text(404, "text/plain", "");
            } else if (path == "/vod_retry.m3u8") {
                std::string pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                                 "#EXTINF:1.0,\n/seg_1.ts\n"
                                 "#EXTINF:1.0,\n/retry_once.ts\n"
                                 "#EXTINF:1.0,\n/seg_2.ts\n"
                                 "#EXT-X-ENDLIST\n";
                send_text(200, "application/vnd.apple.mpegurl", pl);
            } else if (path == "/retry_once.ts") {
                if (retry_seg_hits.fetch_add(1) == 0) {
                    send_text(500, "text/plain", "");
                } else {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: " +
                                      std::to_string(ts_segment.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(sock, asio::buffer(hdr), ec);
                    if (!ec) asio::write(sock, asio::buffer(ts_segment), ec);
                }
            } else if (path == "/vod_fail.m3u8") {
                std::string pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                                 "#EXTINF:1.0,\n/seg_1.ts\n"
                                 "#EXTINF:1.0,\n/perm404.ts\n"
                                 "#EXT-X-ENDLIST\n";
                send_text(200, "application/vnd.apple.mpegurl", pl);
            } else if (path == "/live_die.m3u8") {
                if (die_poll.fetch_add(1) == 0) {
                    std::string pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                                     "#EXTINF:1.0,\n/seg_1.ts\n#EXTINF:1.0,\n/seg_2.ts\n";
                    send_text(200, "application/vnd.apple.mpegurl", pl);
                } else {
                    send_text(503, "text/plain", "");
                }
            } else {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: " +
                                  std::to_string(ts_segment.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(ts_segment), ec);
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    // 1. Live HLS with 1-in-20 failing segment skips cleanly without NetworkError
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/live.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(seg9_hits.load() == 2);
        TEST_CHECK(buf->TotalWritten() > 100);
    }

    // 2. VOD HLS with 1 transient 500 recovers on immediate retry; permanent 404 fails cleanly
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/vod_retry.m3u8";
        meta.container_ext = "m3u8";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(retry_seg_hits.load() >= 2);
        TEST_CHECK(buf->TotalWritten() > 100);
    }
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/vod_fail.m3u8";
        meta.container_ext = "m3u8";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(ec == ErrorCode::NetworkError);
    }

    // 3. Dead live origin fails with NetworkError within ~2s (3 consecutive playlist errors)
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/live_die.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(ec == ErrorCode::NetworkError);
    }

    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound10HlsLiveToleranceAndVod\n";
}

void TestRound11HlsLiveSequenceAndStall() {
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF;
    mp3_frame[1] = 0xFB;
    mp3_frame[2] = 0x90;
    mp3_frame[3] = 0x64;

    std::vector<std::uint8_t> pes_payload = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
    for (int f = 0; f < 4; ++f) {
        pes_payload.insert(pes_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }

    std::vector<std::uint8_t> ts_segment;
    for (std::size_t off = 0, pkt_idx = 0; off < pes_payload.size(); ++pkt_idx) {
        std::array<std::uint8_t, 188> pkt{};
        pkt.fill(0xFF);
        pkt[0] = 0x47;
        pkt[1] = (pkt_idx == 0 ? 0x41 : 0x01);
        pkt[2] = 0x00;
        std::size_t rem = pes_payload.size() - off;
        if (rem >= 184) {
            pkt[3] = static_cast<std::uint8_t>(0x10 | (pkt_idx & 0x0F));
            std::memcpy(pkt.data() + 4, pes_payload.data() + off, 184);
            off += 184;
        } else {
            std::uint8_t adapt_len = static_cast<std::uint8_t>(183 - rem);
            pkt[3] = static_cast<std::uint8_t>(0x30 | (pkt_idx & 0x0F));
            pkt[4] = adapt_len;
            if (adapt_len > 0) pkt[5] = 0x00;
            std::memcpy(pkt.data() + 5 + adapt_len, pes_payload.data() + off, rem);
            off += rem;
        }
        ts_segment.insert(ts_segment.end(), pkt.begin(), pkt.end());
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> dvr_poll{0};
    std::atomic<int> dvr_stale_hits{0};
    std::atomic<int> dvr_edge_hits{0};
    std::atomic<int> dvr_fail180_hits{0};
    std::atomic<int> rewind_early_hits{0};
    std::atomic<int> disc_total_hits{0};
    std::atomic<int> frozen_polls{0};
    std::atomic<int> vod_all_hits{0};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, raw_path;
            iss >> method >> raw_path;
            auto qpos = raw_path.find('?');
            std::string path = (qpos == std::string::npos) ? raw_path : raw_path.substr(0, qpos);

            auto send_text = [&](int code, const char* ct, const std::string& body) {
                std::string hdr = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Err") +
                                  "\r\nContent-Type: " + ct + "\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && !body.empty()) asio::write(sock, asio::buffer(body), ec);
            };
            auto send_ts = [&]() {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: " +
                                  std::to_string(ts_segment.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(ts_segment), ec);
            };

            if (path == "/dvr.m3u8") {
                int p = dvr_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n";
                if (p == 0) {
                    // 80 segments (100..179): live edge should download ONLY 177, 178, 179
                    pl << "#EXT-X-MEDIA-SEQUENCE:100\n";
                    for (int s = 100; s <= 179; ++s) {
                        pl << "#EXTINF:1.0,\n/dvr_" << s << ".ts?tok=p0\n";
                    }
                } else if (p == 1) {
                    // 81 segments (>64 old deque size) + rotating query token: should fetch ONLY 180 (fails 404) & 181
                    pl << "#EXT-X-MEDIA-SEQUENCE:101\n";
                    for (int s = 101; s <= 181; ++s) {
                        pl << "#EXTINF:1.0,\n/dvr_" << s << ".ts?tok=p1\n";
                    }
                } else if (p == 2) {
                    // 180 is still present in window; should NOT be retried; only 182 should fetch
                    pl << "#EXT-X-MEDIA-SEQUENCE:103\n";
                    for (int s = 103; s <= 182; ++s) {
                        pl << "#EXTINF:1.0,\n/dvr_" << s << ".ts?tok=p2\n";
                    }
                } else if (p == 3) {
                    // Encoder restart rewind to seq 0 (6 segments r0..r5): reposition to last 3 (r3..r5)
                    pl << "#EXT-X-MEDIA-SEQUENCE:0\n";
                    for (int s = 0; s <= 5; ++s) {
                        pl << "#EXTINF:1.0,\n/rew_" << s << ".ts\n";
                    }
                } else if (p == 4) {
                    // Unseen discontinuity with 6 new segments (6..11): continue sequentially without skipping
                    pl << "#EXT-X-MEDIA-SEQUENCE:0\n";
                    for (int s = 0; s <= 5; ++s) {
                        pl << "#EXTINF:1.0,\n/rew_" << s << ".ts\n";
                    }
                    pl << "#EXT-X-DISCONTINUITY\n";
                    for (int s = 6; s <= 11; ++s) {
                        pl << "#EXTINF:1.0,\n/disc_" << s << ".ts\n";
                    }
                } else {
                    // Discontinuity tag still present on seq 6 (<= last_seq 11) + 1 new segment 12 + ENDLIST
                    pl << "#EXT-X-MEDIA-SEQUENCE:0\n";
                    for (int s = 0; s <= 5; ++s) {
                        pl << "#EXTINF:1.0,\n/rew_" << s << ".ts\n";
                    }
                    pl << "#EXT-X-DISCONTINUITY\n";
                    for (int s = 6; s <= 12; ++s) {
                        pl << "#EXTINF:1.0,\n/disc_" << s << ".ts\n";
                    }
                    pl << "#EXT-X-ENDLIST\n";
                }
                send_text(200, "application/vnd.apple.mpegurl", pl.str());
            } else if (path == "/dvr_180.ts") {
                dvr_fail180_hits.fetch_add(1);
                send_text(404, "text/plain", "");
            } else if (path.rfind("/dvr_", 0) == 0) {
                int num = std::atoi(path.c_str() + 5);
                if (num < 177) {
                    dvr_stale_hits.fetch_add(1);
                } else {
                    dvr_edge_hits.fetch_add(1);
                }
                send_ts();
            } else if (path.rfind("/rew_", 0) == 0) {
                int num = std::atoi(path.c_str() + 5);
                if (num < 3) rewind_early_hits.fetch_add(1);
                send_ts();
            } else if (path.rfind("/disc_", 0) == 0) {
                disc_total_hits.fetch_add(1);
                send_ts();
            } else if (path == "/frozen.m3u8") {
                frozen_polls.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:50\n";
                for (int s = 50; s <= 54; ++s) {
                    pl << "#EXTINF:1.0,\n/f_" << s << ".ts\n";
                }
                send_text(200, "application/vnd.apple.mpegurl", pl.str());
            } else if (path == "/vod_all.m3u8") {
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:10\n";
                for (int s = 0; s < 6; ++s) {
                    pl << "#EXTINF:1.0,\n/va_" << s << ".ts\n";
                }
                pl << "#EXT-X-ENDLIST\n";
                send_text(200, "application/vnd.apple.mpegurl", pl.str());
            } else if (path.rfind("/va_", 0) == 0) {
                vod_all_hits.fetch_add(1);
                send_ts();
            } else {
                send_ts();
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    // 1. Live DVR window (>64 segments), rotating URL tokens, skipped seq not retried, rewind & discontinuity
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/dvr.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 50;
        auto buf = std::make_shared<AudioStreamBuffer>(512 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(dvr_stale_hits.load() == 0);
        TEST_CHECK(dvr_edge_hits.load() == 5); // 177, 178, 179, 181, 182
        TEST_CHECK(dvr_fail180_hits.load() == 2); // 1 retry then permanently skipped
        TEST_CHECK(rewind_early_hits.load() == 0); // rewind jumped to last 3 (r3..r5)
        TEST_CHECK(disc_total_hits.load() == 7); // discontinuity downloads 6..12 sequentially without skipping or repeating
        TEST_CHECK(buf->TotalWritten() > 100);
    }

    // 2. Frozen live playlist without errors ends cleanly (normal EOF -> OnAudioStreamFinished, no NetworkError)
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/frozen.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 150;
        tcfg.hls_live_stall_timeout_ms = 1200;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(ec == ErrorCode::None);
        TEST_CHECK(frozen_polls.load() >= 4);
        TEST_CHECK(buf->TotalWritten() > 100);
    }

    // 3. VOD (#EXT-X-ENDLIST) downloads ALL segments from index 0
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/vod_all.m3u8";
        meta.container_ext = "m3u8";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(vod_all_hits.load() == 6);
        TEST_CHECK(buf->TotalWritten() > 100);
    }

    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound11HlsLiveSequenceAndStall\n";
}

void TestRound12HlsDiscontinuityAndPmtSwitch() {
    auto make_ts_segment = [](std::uint16_t pmt_pid, std::uint8_t pmt_ver, std::uint16_t audio_pid) {
        std::vector<std::uint8_t> seg;
        // PAT packet (PID 0x0000)
        std::array<std::uint8_t, 188> pat{};
        pat.fill(0xFF);
        pat[0] = 0x47; pat[1] = 0x40; pat[2] = 0x00; pat[3] = 0x10;
        pat[4] = 0x00; // pointer_field
        pat[5] = 0x00; // table_id PAT
        pat[6] = 0xB0; pat[7] = 0x0D; // sec_len = 13
        pat[8] = 0x00; pat[9] = 0x01; // ts_id = 1
        pat[10] = 0xC1; pat[11] = 0x00; pat[12] = 0x00;
        pat[13] = 0x00; pat[14] = 0x01; // program = 1
        pat[15] = static_cast<std::uint8_t>(0xE0 | ((pmt_pid >> 8) & 0x1F));
        pat[16] = static_cast<std::uint8_t>(pmt_pid & 0xFF);
        seg.insert(seg.end(), pat.begin(), pat.end());

        // PMT packet (PID pmt_pid)
        std::array<std::uint8_t, 188> pmt{};
        pmt.fill(0xFF);
        pmt[0] = 0x47;
        pmt[1] = static_cast<std::uint8_t>(0x40 | ((pmt_pid >> 8) & 0x1F));
        pmt[2] = static_cast<std::uint8_t>(pmt_pid & 0xFF);
        pmt[3] = 0x10;
        pmt[4] = 0x00; // pointer_field
        pmt[5] = 0x02; // table_id PMT
        pmt[6] = 0xB0; pmt[7] = 0x12; // sec_len = 18
        pmt[8] = 0x00; pmt[9] = 0x01; // program = 1
        pmt[10] = static_cast<std::uint8_t>(0xC1 | ((pmt_ver & 0x1F) << 1));
        pmt[11] = 0x00; pmt[12] = 0x00;
        pmt[13] = static_cast<std::uint8_t>(0xE0 | ((audio_pid >> 8) & 0x1F));
        pmt[14] = static_cast<std::uint8_t>(audio_pid & 0xFF);
        pmt[15] = 0xF0; pmt[16] = 0x00; // prog_info_len = 0
        pmt[17] = 0x03; // MPEG Audio
        pmt[18] = static_cast<std::uint8_t>(0xE0 | ((audio_pid >> 8) & 0x1F));
        pmt[19] = static_cast<std::uint8_t>(audio_pid & 0xFF);
        pmt[20] = 0xF0; pmt[21] = 0x00; // es_info_len = 0
        seg.insert(seg.end(), pmt.begin(), pmt.end());

        // 4 MP3 frames wrapped in PES on audio_pid
        std::vector<std::uint8_t> mp3_frame(417, 0);
        mp3_frame[0] = 0xFF; mp3_frame[1] = 0xFB; mp3_frame[2] = 0x90; mp3_frame[3] = 0x64;
        std::vector<std::uint8_t> pes = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
        for (int f = 0; f < 4; ++f) pes.insert(pes.end(), mp3_frame.begin(), mp3_frame.end());

        for (std::size_t off = 0, pkt_idx = 0; off < pes.size(); ++pkt_idx) {
            std::array<std::uint8_t, 188> pkt{};
            pkt.fill(0xFF);
            pkt[0] = 0x47;
            pkt[1] = static_cast<std::uint8_t>((pkt_idx == 0 ? 0x40 : 0x00) | ((audio_pid >> 8) & 0x1F));
            pkt[2] = static_cast<std::uint8_t>(audio_pid & 0xFF);
            std::size_t rem = pes.size() - off;
            if (rem >= 184) {
                pkt[3] = static_cast<std::uint8_t>(0x10 | (pkt_idx & 0x0F));
                std::memcpy(pkt.data() + 4, pes.data() + off, 184);
                off += 184;
            } else {
                std::uint8_t adapt_len = static_cast<std::uint8_t>(183 - rem);
                pkt[3] = static_cast<std::uint8_t>(0x30 | (pkt_idx & 0x0F));
                pkt[4] = adapt_len;
                if (adapt_len > 0) pkt[5] = 0x00;
                std::memcpy(pkt.data() + 5 + adapt_len, pes.data() + off, rem);
                off += rem;
            }
            seg.insert(seg.end(), pkt.begin(), pkt.end());
        }
        return seg;
    };

    const auto ts_main = make_ts_segment(0x1000, 0, 0x0100);
    const auto ts_ad = make_ts_segment(0x1001, 1, 0x0102);
    const auto ts_back = make_ts_segment(0x1000, 2, 0x0103);

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> poll_idx{0};
    std::mutex log_mtx;
    std::vector<int> fetched_seqs;

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            auto send_text = [&](int code, const char* ct, const std::string& body) {
                std::string hdr = "HTTP/1.1 " + std::to_string(code) + " OK\r\nContent-Type: " + ct +
                                  "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && !body.empty()) asio::write(sock, asio::buffer(body), ec);
            };
            auto send_bin = [&](const std::vector<std::uint8_t>& data) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: " +
                                  std::to_string(data.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(data), ec);
            };

            if (path == "/twitch_ad.m3u8") {
                int p = poll_idx.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n";
                if (p == 0) {
                    // Start live edge at 102, 103, 104
                    pl << "#EXT-X-MEDIA-SEQUENCE:100\n#EXT-X-DISCONTINUITY-SEQUENCE:0\n";
                    for (int s = 100; s <= 104; ++s) pl << "#EXTINF:2.0,\n/s_" << s << ".ts\n";
                } else if (p == 1) {
                    // Ad break starts at 105 with #EXT-X-DISCONTINUITY -> must NOT skip 105..106 or repeat anything
                    pl << "#EXT-X-MEDIA-SEQUENCE:102\n#EXT-X-DISCONTINUITY-SEQUENCE:0\n";
                    for (int s = 102; s <= 104; ++s) pl << "#EXTINF:2.0,\n/s_" << s << ".ts\n";
                    pl << "#EXT-X-DISCONTINUITY\n";
                    for (int s = 105; s <= 107; ++s) pl << "#EXTINF:2.0,\n/s_" << s << ".ts\n";
                } else if (p == 2) {
                    // Segment 105 rolled off top -> #EXT-X-DISCONTINUITY-SEQUENCE increments 0->1; ad ends at 108 (#EXT-X-DISCONTINUITY)
                    pl << "#EXT-X-MEDIA-SEQUENCE:106\n#EXT-X-DISCONTINUITY-SEQUENCE:1\n";
                    for (int s = 106; s <= 107; ++s) pl << "#EXTINF:2.0,\n/s_" << s << ".ts\n";
                    pl << "#EXT-X-DISCONTINUITY\n";
                    for (int s = 108; s <= 109; ++s) pl << "#EXTINF:2.0,\n/s_" << s << ".ts\n";
                } else {
                    // Discontinuity rolled off top -> dseq increments 1->2 + final segment 110 + ENDLIST
                    pl << "#EXT-X-MEDIA-SEQUENCE:109\n#EXT-X-DISCONTINUITY-SEQUENCE:2\n";
                    pl << "#EXTINF:2.0,\n/s_109.ts\n";
                    pl << "#EXTINF:2.0,\n/s_110.ts\n";
                    pl << "#EXT-X-ENDLIST\n";
                }
                send_text(200, "application/vnd.apple.mpegurl", pl.str());
            } else if (path.rfind("/s_", 0) == 0) {
                int s = std::atoi(path.c_str() + 3);
                {
                    std::lock_guard<std::mutex> lk(log_mtx);
                    fetched_seqs.push_back(s);
                }
                if (s >= 105 && s <= 107) {
                    send_bin(ts_ad);
                } else if (s >= 108) {
                    send_bin(ts_back);
                } else {
                    send_bin(ts_main);
                }
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    TrackMetadata meta;
    meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/twitch_ad.m3u8";
    meta.container_ext = "m3u8";
    meta.is_live = true;
    TranscoderConfig tcfg;
    tcfg.pace_realtime = false;
    tcfg.hls_poll_interval_ms = 50;
    auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
    std::atomic<bool> cancel{false}, pause{false};
    std::atomic<float> vol{100.0f};
    std::string err;
    ErrorCode ec = ErrorCode::None;
    AudioTranscoder tr(meta, tcfg, buf);
    TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
    TEST_CHECK(err.empty());
    TEST_CHECK(buf->TotalWritten() > 100);

    std::vector<int> expected = {102, 103, 104, 105, 106, 107, 108, 109, 110};
    TEST_CHECK(fetched_seqs == expected);

    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound12HlsDiscontinuityAndPmtSwitch\n";
}

void TestRound13HlsStaleCdnPlaylist() {
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF;
    mp3_frame[1] = 0xFB;
    mp3_frame[2] = 0x90;
    mp3_frame[3] = 0x64;
    std::vector<std::uint8_t> pes_payload = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
    for (int f = 0; f < 4; ++f) {
        pes_payload.insert(pes_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }
    std::vector<std::uint8_t> ts_segment;
    for (std::size_t off = 0, pkt_idx = 0; off < pes_payload.size(); ++pkt_idx) {
        std::array<std::uint8_t, 188> pkt{};
        pkt.fill(0xFF);
        pkt[0] = 0x47;
        pkt[1] = (pkt_idx == 0 ? 0x41 : 0x01);
        pkt[2] = 0x00;
        std::size_t rem = pes_payload.size() - off;
        if (rem >= 184) {
            pkt[3] = static_cast<std::uint8_t>(0x10 | (pkt_idx & 0x0F));
            std::memcpy(pkt.data() + 4, pes_payload.data() + off, 184);
            off += 184;
        } else {
            std::uint8_t adapt_len = static_cast<std::uint8_t>(183 - rem);
            pkt[3] = static_cast<std::uint8_t>(0x30 | (pkt_idx & 0x0F));
            pkt[4] = adapt_len;
            if (adapt_len > 0) pkt[5] = 0x00;
            std::memcpy(pkt.data() + 5 + adapt_len, pes_payload.data() + off, rem);
            off += rem;
        }
        ts_segment.insert(ts_segment.end(), pkt.begin(), pkt.end());
    }

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> cdn_poll{0};
    std::atomic<int> tail_poll{0};
    std::mutex mtx;
    std::vector<std::string> fetched_cdn;
    std::vector<int> fetched_tail;

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);

            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            auto send_text = [&](const std::string& body) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && !body.empty()) asio::write(sock, asio::buffer(body), ec);
            };
            auto send_ts = [&]() {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: " +
                                  std::to_string(ts_segment.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(ts_segment), ec);
            };

            if (path == "/cdn_lag.m3u8") {
                int p = cdn_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n";
                if (p == 0) {
                    pl << "#EXT-X-MEDIA-SEQUENCE:100\n";
                    for (int s = 100; s <= 104; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 1) {
                    pl << "#EXT-X-MEDIA-SEQUENCE:101\n";
                    for (int s = 101; s <= 105; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 2) {
                    // Stale CDN edge response (1 update behind: media_sequence 100, back_seq 104 >= 101)
                    pl << "#EXT-X-MEDIA-SEQUENCE:100\n";
                    for (int s = 100; s <= 104; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 3) {
                    pl << "#EXT-X-MEDIA-SEQUENCE:102\n";
                    for (int s = 102; s <= 106; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 4) {
                    // Stale CDN edge response (1 update behind: media_sequence 101, back_seq 105 >= 102)
                    pl << "#EXT-X-MEDIA-SEQUENCE:101\n";
                    for (int s = 101; s <= 105; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 5) {
                    pl << "#EXT-X-MEDIA-SEQUENCE:103\n";
                    for (int s = 103; s <= 107; ++s) pl << "#EXTINF:2.0,\n/c_" << s << ".ts\n";
                } else if (p == 6) {
                    // Genuine encoder restart (media_sequence drops back to 0): reposition to last 3 (r2..r4)
                    pl << "#EXT-X-MEDIA-SEQUENCE:0\n";
                    for (int s = 0; s <= 4; ++s) pl << "#EXTINF:2.0,\n/r_" << s << ".ts\n";
                } else {
                    pl << "#EXT-X-MEDIA-SEQUENCE:1\n";
                    for (int s = 1; s <= 5; ++s) pl << "#EXTINF:2.0,\n/r_" << s << ".ts\n";
                    pl << "#EXT-X-ENDLIST\n";
                }
                send_text(pl.str());
            } else if (path == "/tail_lag.m3u8") {
                int p = tail_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n";
                if (p == 0) {
                    // Starts at live edge 3, 4, 5
                    for (int s = 0; s <= 5; ++s) pl << "#EXTINF:2.0,\n/t_" << s << ".ts\n";
                } else if (p == 1) {
                    // Stale CDN edge missing the tail segment 5 (returns 0..4 with media_sequence still 0)
                    for (int s = 0; s <= 4; ++s) pl << "#EXTINF:2.0,\n/t_" << s << ".ts\n";
                } else {
                    for (int s = 0; s <= 6; ++s) pl << "#EXTINF:2.0,\n/t_" << s << ".ts\n";
                    pl << "#EXT-X-ENDLIST\n";
                }
                send_text(pl.str());
            } else if (path.rfind("/c_", 0) == 0 || path.rfind("/r_", 0) == 0) {
                std::string name = path.substr(1, path.size() - 4);
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    fetched_cdn.push_back(name);
                }
                send_ts();
            } else if (path.rfind("/t_", 0) == 0) {
                int s = std::atoi(path.c_str() + 3);
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    fetched_tail.push_back(s);
                }
                send_ts();
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/cdn_lag.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 50;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(buf->TotalWritten() > 100);
        std::vector<std::string> expected_cdn = {
            "c_102", "c_103", "c_104", "c_105", "c_106", "c_107",
            "r_2", "r_3", "r_4", "r_5"
        };
        TEST_CHECK(fetched_cdn == expected_cdn);
    }

    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/tail_lag.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 50;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(err.empty());
        TEST_CHECK(buf->TotalWritten() > 100);
        std::vector<int> expected_tail = {3, 4, 5, 6};
        TEST_CHECK(fetched_tail == expected_tail);
    }

    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound13HlsStaleCdnPlaylist\n";
}

void TestRound24LibcurlInstallHint() {
    auto u22 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=ubuntu\nVERSION_ID=\"22.04\"\nPRETTY_NAME=\"Ubuntu 22.04.5 LTS\"\n");
    TEST_CHECK(u22.find("Detected system: Ubuntu 22.04.5 LTS") != std::string::npos);
    TEST_CHECK(u22.find("sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libcurl4:i386") != std::string::npos);
    TEST_CHECK(u22.find("libcurl4t64:i386") == std::string::npos);

    auto u24 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=ubuntu\nVERSION_ID=\"24.04\"\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\n");
    TEST_CHECK(u24.find("libcurl4t64:i386") != std::string::npos);
    TEST_CHECK(u24.find("libcurl4:i386") == std::string::npos);

    auto u24_dpkg = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=ubuntu\nVERSION_ID=\"24.04\"\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\n", true);
    TEST_CHECK(u24_dpkg.find("sudo apt update && sudo apt install libcurl4t64:i386") != std::string::npos);
    TEST_CHECK(u24_dpkg.find("--add-architecture") == std::string::npos);

    auto deb12 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=debian\nVERSION_ID=\"12\"\nPRETTY_NAME=\"Debian GNU/Linux 12 (bookworm)\"\n");
    TEST_CHECK(deb12.find("apt install libcurl4:i386") != std::string::npos);
    TEST_CHECK(deb12.find("libcurl4t64:i386") == std::string::npos);

    auto deb13 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=debian\nVERSION_ID=\"13\"\nPRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\n");
    TEST_CHECK(deb13.find("libcurl4t64:i386") != std::string::npos);

    auto mint = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=linuxmint\r\nID_LIKE=\"ubuntu debian\"\r\nVERSION_ID=\"21.3\"\r\nPRETTY_NAME=\"Linux Mint 21.3\"\r\n");
    TEST_CHECK(mint.find("Detected system: Linux Mint 21.3") != std::string::npos);
    TEST_CHECK(mint.find("libcurl4:i386") != std::string::npos);
    TEST_CHECK(mint.find("libcurl4t64:i386") != std::string::npos);

    auto rocky = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\nVERSION_ID=\"9.4\"\nPRETTY_NAME=\"Rocky Linux 9.4\"\n");
    TEST_CHECK(rocky.find("sudo dnf install libcurl.i686") != std::string::npos);

    auto centos7 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=\"centos\"\nID_LIKE=\"rhel fedora\"\nVERSION_ID=\"7\"\nPRETTY_NAME=\"CentOS Linux 7\"\n");
    TEST_CHECK(centos7.find("sudo yum install libcurl.i686") != std::string::npos);

    auto arch = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=arch\nPRETTY_NAME=\"Arch Linux\"\n");
    TEST_CHECK(arch.find("sudo pacman -S lib32-curl") != std::string::npos);

    auto suse = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=\"opensuse-leap\"\nID_LIKE=\"suse opensuse\"\nVERSION_ID=\"15.5\"\nPRETTY_NAME=\"openSUSE Leap 15.5\"\n");
    TEST_CHECK(suse.find("sudo zypper install libcurl4-32bit") != std::string::npos);

    auto empty_hint = HttpClient::BuildLibcurlInstallHintFromOsRelease("");
    TEST_CHECK(empty_hint.find("unknown Linux distribution") != std::string::npos);
    TEST_CHECK(empty_hint.find("libcurl4:i386") != std::string::npos);
    TEST_CHECK(empty_hint.find("libcurl.i686") != std::string::npos);
    TEST_CHECK(empty_hint.find("lib32-curl") != std::string::npos);
    TEST_CHECK(empty_hint.find("libcurl4-32bit") != std::string::npos);

    // Non-numeric VERSION_ID on Debian/Ubuntu + CRLF + control-char/length sanitization
    std::string long_pretty(140, 'X');
    long_pretty[5] = '\x07';
    auto weird = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "ID=\"debian\"\r\nVERSION_ID=\"sid\"\r\nPRETTY_NAME=\"" + long_pretty + "\"\r\n");
    TEST_CHECK(weird.find("libcurl4:i386") != std::string::npos);
    TEST_CHECK(weird.find("libcurl4t64:i386") != std::string::npos);
    TEST_CHECK(weird.find('\x07') == std::string::npos);
    auto det_pos = weird.find("Detected system: ");
    TEST_CHECK(det_pos != std::string::npos);
    auto det_end = weird.find('\n', det_pos);
    TEST_CHECK((det_end - (det_pos + 17)) == 100);

    // Emit Debian 11 sample output cleanly for verification
    auto deb11 = HttpClient::BuildLibcurlInstallHintFromOsRelease(
        "PRETTY_NAME=\"Debian GNU/Linux 11 (bullseye)\"\nID=debian\nVERSION_ID=\"11\"\n");
    TEST_CHECK(deb11.find("apt install libcurl4:i386") != std::string::npos);
    std::cout << "[INFO] Sample Debian 11 missing-libcurl log:\n" << deb11 << "\n";
    std::cout << "[PASS] TestRound24LibcurlInstallHint\n";
}

// ---------------- Round 27 Tests (Fases 1-10) ----------------

void TestRound27Fase1RawMp3LiveAndFlac() {
    // 1. Minimal CBR MP3 frame (128 kbps, 44.1kHz, padded=0 -> 417 bytes, no ID3v2 tag)
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF;
    mp3_frame[1] = 0xFB;
    mp3_frame[2] = 0x90;
    mp3_frame[3] = 0x64;

    std::vector<std::uint8_t> raw_mp3;
    for (int i = 0; i < 6; ++i) {
        raw_mp3.insert(raw_mp3.end(), mp3_frame.begin(), mp3_frame.end());
    }

    // Same MP3 preceded by a 20-byte ID3v2.3 header
    std::vector<std::uint8_t> id3_mp3 = {
        'I', 'D', '3', 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    id3_mp3.insert(id3_mp3.end(), raw_mp3.begin(), raw_mp3.end());

    // Minimal valid FLAC stream: fLaC + 34-byte STREAMINFO (blocksize=16, 44.1kHz, 1ch, 16-bit) + 1 CONSTANT silence frame
    auto flac_crc8 = [](const std::uint8_t* d, std::size_t n) {
        std::uint8_t crc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            crc ^= d[i];
            for (int b = 0; b < 8; ++b) crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x07) : static_cast<std::uint8_t>(crc << 1);
        }
        return crc;
    };
    auto flac_crc16 = [](const std::uint8_t* d, std::size_t n) {
        std::uint16_t crc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            crc ^= static_cast<std::uint16_t>(d[i]) << 8;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x8005) : static_cast<std::uint16_t>(crc << 1);
        }
        return crc;
    };
    std::vector<std::uint8_t> flac_bytes = {
        'f', 'L', 'a', 'C',
        0x80, 0x00, 0x00, 0x22, // STREAMINFO, is_last=1, length=34
        0x00, 0x10, 0x00, 0x10, // min/max block size = 16
        0x00, 0x00, 0x00,       // min frame size = 0
        0x00, 0x00, 0x00,       // max frame size = 0
        0x0A, 0xC4, 0x40, 0xF0, // 44100 Hz (20 bits) + 1 ch (3 bits=0) + 16 bps (5 bits=15) + total samples upper
        0x00, 0x00, 0x00, 0x10, // total samples = 16
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 // MD5 = 0
    };
    std::vector<std::uint8_t> flac_frame = {
        0xFF, 0xF8, // fixed block size sync
        0x69,       // blocksize=16 (code 6 in bit 4..7 -> wait: use reserved 0x7? Let's encode code 7 = 8-bit N-1 or code 8 = 256!)
        0x08,       // mono, 16-bit signed (4 -> 0x08? Wait: ch=0000(1ch), bps=100(16bit)->0x08)
        0x00        // frame 0
    };
    // Actually use blocksize code 0x06 (8-bit blocksize - 1 follows sample rate -> 15 for 16 samples), sr=0x09 (44.1kHz):
    // byte2 = (0x6 << 4) | 0x9 = 0x69, byte3 = 0x08 (1ch, 16-bit), byte4 = 0x00 (frame 0), byte5 = 15 (blocksize - 1)
    flac_frame = {0xFF, 0xF8, 0x69, 0x08, 0x00, 0x0F};
    flac_frame.push_back(flac_crc8(flac_frame.data(), flac_frame.size()));
    flac_frame.push_back(0x00); // subframe header: padding=0, CONSTANT(0), wasted=0
    flac_frame.push_back(0x00); // constant sample high
    flac_frame.push_back(0x00); // constant sample low
    std::uint16_t fc16 = flac_crc16(flac_frame.data(), flac_frame.size());
    flac_frame.push_back(static_cast<std::uint8_t>((fc16 >> 8) & 0xFF));
    flac_frame.push_back(static_cast<std::uint8_t>(fc16 & 0xFF));
    flac_bytes.insert(flac_bytes.end(), flac_frame.begin(), flac_frame.end());

    TEST_PHASE("test server");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex sock_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;

    std::thread srv([&]() {
        std::vector<std::thread> workers;
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);
            {
                std::lock_guard<std::mutex> lk(sock_mtx);
                sockets.push_back(sock);
            }
            workers.emplace_back([sock, &stop_srv, &raw_mp3, &id3_mp3, &mp3_frame, &flac_bytes]() {
                asio::streambuf sbuf;
                std::error_code io_ec;
                asio::read_until(*sock, sbuf, "\r\n\r\n", io_ec);
                if (io_ec) return;
                std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
                std::istringstream iss(raw_req);
                std::string method, path;
                iss >> method >> path;

                auto send_finite = [&](const char* ct, const std::vector<std::uint8_t>& body) {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(ct) +
                                      "\r\nContent-Length: " + std::to_string(body.size()) +
                                      "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    if (!io_ec) asio::write(*sock, asio::buffer(body), io_ec);
                };

                if (path == "/raw.mp3") {
                    send_finite("audio/mpeg", raw_mp3);
                } else if (path == "/id3.mp3") {
                    send_finite("audio/mpeg", id3_mp3);
                } else if (path == "/radio.mp3") {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: LiveTest\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    while (!io_ec && !stop_srv.load()) {
                        asio::write(*sock, asio::buffer(mp3_frame), io_ec);
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } else if (path == "/sample.flac") {
                    send_finite("audio/flac", flac_bytes);
                }
                sock->close(io_ec);
            });
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    // Case 1: Raw CBR MP3 without ID3v2
    TEST_PHASE("Case 1: raw.mp3");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/raw.mp3";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    // Case 2: MP3 with ID3v2 tag
    TEST_PHASE("Case 2: id3.mp3");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/id3.mp3";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    // Case 3: Infinite live Icecast-style MP3 stream (no Content-Length) without ID3v2
    TEST_PHASE("Case 3: radio.mp3 live");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/radio.mp3";
        meta.is_live = true;
        meta.container_ext = "mp3";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = true;
        tcfg.initial_burst_ms = 2000;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{50.0f}; // Force transcoder decode/encode path instead of raw passthrough
        std::string err;
        std::thread canceler([&]() {
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (std::chrono::steady_clock::now() < end && buf->TotalWritten() < 128) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cancel.store(true);
            HttpClient::PollCancellations();
        });
        AudioTranscoder tr(meta, tcfg, buf);
        tr.Run(cancel, pause, vol, err);
        canceler.join();
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    // Case 4: Non-seekable FLAC stream via HTTP
    TEST_PHASE("Case 4: sample.flac");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/sample.flac";
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    TEST_PHASE("stop acceptor & join server");
    stop_srv.store(true);
    CloseTestSockets(sockets, sock_mtx);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound27Fase1RawMp3LiveAndFlac\n";
}

void TestRound27Fase2HlsFormatsAndSilentWatchdog() {
    auto be32 = [](std::uint32_t v) {
        return std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>((v >> 24) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF),
            static_cast<std::uint8_t>((v >> 8) & 0xFF),
            static_cast<std::uint8_t>(v & 0xFF)
        };
    };
    auto make_box = [&](const char fcc[4], const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> out(8 + body.size());
        auto sz = be32(static_cast<std::uint32_t>(out.size()));
        std::memcpy(out.data(), sz.data(), 4);
        std::memcpy(out.data() + 4, fcc, 4);
        if (!body.empty()) std::memcpy(out.data() + 8, body.data(), body.size());
        return out;
    };
    auto append = [](std::vector<std::uint8_t>& dst, const std::vector<std::uint8_t>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
    };

    // Valid silent AAC-LC 44.1kHz stereo raw frame (6 bytes) and wrapped ADTS frame (13 bytes)
    const std::vector<std::uint8_t> raw_aac_frame = {0x21, 0x10, 0x04, 0x60, 0x8C, 0x1C};
    // ADTS frame length = 7 + 6 = 13: FF F1 50 80 01 BF FC
    const std::vector<std::uint8_t> adts_frame = {
        0xFF, 0xF1, 0x50, 0x80, 0x01, 0xBF, 0xFC,
        0x21, 0x10, 0x04, 0x60, 0x8C, 0x1C
    };

    // ID3v2.3 packed-audio segment (exact RFC 8216 / YouTube itag 234 format: ID3v2 + ADTS frames)
    std::vector<std::uint8_t> id3_adts_seg = {
        'I', 'D', '3', 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };
    id3_adts_seg.resize(26, 0);
    for (int i = 0; i < 4; ++i) append(id3_adts_seg, adts_frame);

    // fMP4 init segment (ftyp + moov -> trak -> mdia -> minf -> stbl -> stsd -> mp4a -> esds ASC 0x12 0x10)
    std::vector<std::uint8_t> ftyp_body = {'i','s','o','5', 0,0,0,1, 'i','s','o','5'};
    std::vector<std::uint8_t> esds_body = {
        0,0,0,0,
        0x03, 0x19, 0x00, 0x01, 0x00,
        0x04, 0x11, 0x40, 0x15, 0,0,0, 0,0,0,0, 0,0,0,0,
        0x05, 0x02, 0x12, 0x10,
        0x06, 0x01, 0x02
    };
    std::vector<std::uint8_t> mp4a_body(28, 0);
    append(mp4a_body, make_box("esds", esds_body));
    std::vector<std::uint8_t> stsd_body(8, 0);
    stsd_body[7] = 1;
    append(stsd_body, make_box("mp4a", mp4a_body));
    auto stbl = make_box("stbl", make_box("stsd", stsd_body));
    auto minf = make_box("minf", stbl);
    auto mdia = make_box("mdia", minf);
    auto trak = make_box("trak", mdia);
    auto moov = make_box("moov", trak);
    std::vector<std::uint8_t> fmp4_init = make_box("ftyp", ftyp_body);
    append(fmp4_init, moov);

    // fMP4 media segment (moof(tfhd default-sample-size=6 + trun count=2) + mdat(2x raw AAC))
    std::vector<std::uint8_t> tfhd_body = {
        0x00, 0x00, 0x00, 0x10, // flags: default-sample-size-present (0x10)
        0x00, 0x00, 0x00, 0x01, // track_ID = 1
        0x00, 0x00, 0x00, 0x06  // default_sample_size = 6
    };
    std::vector<std::uint8_t> trun_body = {
        0x00, 0x00, 0x00, 0x00, // flags = 0 (use tfhd default sample size)
        0x00, 0x00, 0x00, 0x02  // sample_count = 2
    };
    std::vector<std::uint8_t> traf_body = make_box("tfhd", tfhd_body);
    append(traf_body, make_box("trun", trun_body));
    auto moof = make_box("moof", make_box("traf", traf_body));
    std::vector<std::uint8_t> mdat_body = raw_aac_frame;
    append(mdat_body, raw_aac_frame);
    auto mdat = make_box("mdat", mdat_body);
    std::vector<std::uint8_t> fmp4_seg = moof;
    append(fmp4_seg, mdat);

    TEST_PHASE("test server");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<int> id3_poll{0}, fmp4_poll{0}, init_hits{0};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (ec) continue;
            std::string raw_req(asio::buffers_begin(sbuf.data()), asio::buffers_end(sbuf.data()));
            std::istringstream iss(raw_req);
            std::string method, path;
            iss >> method >> path;

            auto send_bin = [&](const char* ct, const std::vector<std::uint8_t>& body) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(ct) +
                                  "\r\nContent-Length: " + std::to_string(body.size()) +
                                  "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec && !body.empty()) asio::write(sock, asio::buffer(body), ec);
            };
            auto send_text = [&](const std::string& body) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(body), ec);
            };

            if (path == "/master.m3u8") {
                send_text(
                    "#EXTM3U\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=1500000,CODECS=\"avc1.4D401F,mp4a.40.2\"\n"
                    "/video_bad.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\n"
                    "/packed_live.m3u8\n");
            } else if (path == "/packed_live.m3u8") {
                int p = id3_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:" << (10 + p) << "\n"
                   << "#EXTINF:2.0,\n/p_" << (10 + p) << ".ts\n"
                   << "#EXTINF:2.0,\n/p_" << (11 + p) << ".ts\n";
                if (p >= 1) pl << "#EXT-X-ENDLIST\n";
                send_text(pl.str());
            } else if (path.rfind("/p_", 0) == 0) {
                send_bin("application/octet-stream", id3_adts_seg);
            } else if (path == "/fmp4_live.m3u8") {
                int p = fmp4_poll.fetch_add(1);
                std::ostringstream pl;
                pl << "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n"
                   << "#EXT-X-MEDIA-SEQUENCE:" << (20 + p) << "\n"
                   << "#EXT-X-MAP:URI=\"/init.mp4\"\n"
                   << "#EXTINF:2.0,\n/seg_" << (20 + p) << ".m4s\n";
                if (p >= 1) pl << "#EXT-X-ENDLIST\n";
                send_text(pl.str());
            } else if (path == "/init.mp4") {
                init_hits.fetch_add(1);
                send_bin("video/mp4", fmp4_init);
            } else if (path.rfind("/seg_", 0) == 0) {
                send_bin("video/iso.segment", fmp4_seg);
            } else if (path == "/silent_junk.ts") {
                std::vector<std::uint8_t> junk(64, 0xAA);
                send_bin("application/octet-stream", junk);
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    // 1. Live master playlist selecting audio-only variant -> ID3-prefixed ADTS packed audio segments
    TEST_PHASE("Case 1: ID3v2 packed AAC HLS");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/master.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 50;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    // 2. Live HLS fMP4 with #EXT-X-MAP init segment (downloaded once) + moof/mdat AAC segments
    TEST_PHASE("Case 2: fMP4 CMAF HLS");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/fmp4_live.m3u8";
        meta.container_ext = "m3u8";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.hls_poll_interval_ms = 50;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(init_hits.load() == 1);
        TEST_CHECK(buf->TotalWritten() > 0);
    }

    // 3. Zero-decode watchdog triggers error instead of staying silent
    TEST_PHASE("Case 3: zero-decode watchdog");
    {
        TrackMetadata meta;
        meta.direct_media_url = base + "/silent_junk.ts";
        meta.is_live = true;
        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.no_audio_timeout_ms = 250;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(ec != ErrorCode::None);
    }

    TEST_PHASE("stop acceptor & join server");
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound27Fase2HlsFormatsAndSilentWatchdog\n";
}

void TestRound27Fase3DnsVsPolicyBlock() {
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{false, "0.0.0.0", "203.0.113.10", 7778});
    std::string err;
    TEST_CHECK(!HttpClient::ValidateRemoteUrl("http://nonexistent.invalid.sampaudiostream.test/audio.mp3", err));
    TEST_CHECK(err.rfind("DNS resolution failed for ", 0) == 0);
    TEST_CHECK(err != "URL not allowed");

    err.clear();
    TEST_CHECK(!HttpClient::ValidateRemoteUrl("http://169.254.169.254/latest/meta-data/", err));
    TEST_CHECK(err == "URL not allowed");

    err.clear();
    TEST_CHECK(!HttpClient::ValidateRemoteUrl("http://127.0.0.1:7778/stream/a.mp3", err));
    TEST_CHECK(err == "URL not allowed");

    PluginConfig cfg;
    cfg.auto_download_extractor = false;
    BinaryManager bm(cfg);
    MetadataCache cache(16, 60);
    StreamResolver resolver(cfg, bm, cache);
    ResolveResult r = resolver.Resolve("http://nonexistent.invalid.sampaudiostream.test/audio.mp3");
    TEST_CHECK(!r.success);
    TEST_CHECK(r.error == ErrorCode::NetworkError);
    TEST_CHECK(r.error_message.rfind("DNS resolution failed for ", 0) == 0);
    std::cout << "[PASS] TestRound27Fase3DnsVsPolicyBlock\n";
}

void TestRound27Fase4MaxPrefetchMb() {
    PluginConfig cfg;
    TEST_CHECK(cfg.max_prefetch_mb == 16);
    cfg.max_prefetch_mb = 999;
    ConfigManager::Sanitize(cfg);
    TEST_CHECK(cfg.max_prefetch_mb == 16);

    // Serve ~1.2 MB finite MP3 payload (>512 KB) and pause consumption right after startup
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF; mp3_frame[1] = 0xFB; mp3_frame[2] = 0x90; mp3_frame[3] = 0x64;
    std::vector<std::uint8_t> payload;
    for (int i = 0; i < 3000; ++i) {
        payload.insert(payload.end(), mp3_frame.begin(), mp3_frame.end());
    }

    TEST_PHASE("test server");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::atomic<bool> server_finished_write{false};

    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sbuf;
            asio::read_until(sock, sbuf, "\r\n\r\n", ec);
            if (!ec) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: " +
                                  std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(payload), ec);
                if (!ec) server_finished_write.store(true);
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    TrackMetadata meta;
    meta.direct_media_url = "http://127.0.0.1:" + std::to_string(port) + "/long.mp3";
    meta.is_live = false;
    TranscoderConfig tcfg;
    tcfg.pace_realtime = true;
    tcfg.initial_burst_ms = 2000;
    tcfg.max_prefetch_mb = 4; // Allows >512 KB finite download to complete immediately
    auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
    std::atomic<bool> cancel{false}, pause{true}; // Paused immediately so decoder stalls after initial frame
    std::atomic<float> vol{100.0f};
    std::string err;

    TEST_PHASE("run paused prefetch & cancel");
    std::thread watcher([&]() {
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < dl && !server_finished_write.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        cancel.store(true);
        HttpClient::PollCancellations();
    });
    AudioTranscoder tr(meta, tcfg, buf);
    tr.Run(cancel, pause, vol, err);
    watcher.join();

    TEST_CHECK(server_finished_write.load());
    TEST_PHASE("stop acceptor & join server");
    stop_srv.store(true);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestRound27Fase4MaxPrefetchMb\n";
}

void TestRound27Fase5Cp1252AndEmoji() {
    PluginConfig def;
    TEST_CHECK(def.pawn_text_encoding == "cp1252");

    std::string title = "T\xC3\xB4 \xF0\x9F\x94\xB4 LIVE: PR\xC3\x89-LAN\xC3\x87" "AMENTO";
    std::string cp = ConvertTextForPawn(title, "cp1252");
    TEST_CHECK(cp == "T\xF4 LIVE: PR\xC9-LAN\xC7" "AMENTO");
    TEST_CHECK(cp.find('?') == std::string::npos);

    // Strip U+FE0F variation selector and U+200D ZWJ cleanly
    std::string with_vs = "Test \xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D Audio";
    std::string cp_vs = ConvertTextForPawn(with_vs, "cp1252");
    TEST_CHECK(cp_vs == "Test Audio");

    // Explicit UTF-8 mode preserves original bytes
    TEST_CHECK(ConvertTextForPawn(title, "utf8") == title);
    std::cout << "[PASS] TestRound27Fase5Cp1252AndEmoji\n";
}

void TestRound27Fase6SyncHttpShortcut() {
    MockPlatformHost host;
    host.players.push_back({0, {0.0f, 0.0f, 0.0f}, 0, 0});

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.auto_download_extractor = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // Extensionless http:// with default vol=100 and world=-1, interior=-1 must send Play BEFORE native returns
    StreamId sid = engine.PlayAudioStreamForPlayer(0, "http://radio.example.com/stream", StreamOptions{});
    TEST_CHECK(sid != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].play == true);
        TEST_CHECK(host.events[0].player_id == 0);
        TEST_CHECK(host.events[0].url == "http://radio.example.com/stream");
    }
    TrackMetadata meta;
    StreamState st = StreamState::Idle;
    TEST_CHECK(engine.GetStreamInfo(sid, meta, st));
    TEST_CHECK(st == StreamState::Playing);

    PumpEngineTicks(engine, 2);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(std::find(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid) != host.ready_stream_ids.end());
    }

    // HTTPS with known native extension (.mp3) and process_direct_audio_files=false ALSO uses passthrough immediately
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }
    StreamId sid_https_mp3 = engine.PlayAudioStreamForPlayer(0, "https://radio.example.com/stream.mp3", StreamOptions{});
    TEST_CHECK(sid_https_mp3 != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(host.events.back().play == true);
        TEST_CHECK(host.events.back().url == "https://radio.example.com/stream.mp3");
        host.events.clear();
    }

    // Extensionless HTTPS, HLS .m3u8, and .mp3 with process_direct_audio_files=true MUST NOT bypass via sync shortcut
    StreamOptions proc_opts;
    proc_opts.process_direct_audio_files = true;
    StreamId sid_https_proc = engine.PlayAudioStreamForPlayer(0, "https://radio.example.com/song.mp3", proc_opts);
    TEST_CHECK(sid_https_proc != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        for (const auto& ev : host.events) {
            TEST_CHECK(!ev.play);
        }
        host.events.clear();
    }

    StreamId sid_https_unknown = engine.PlayAudioStreamForPlayer(0, "https://unknown-video.example.com/watch?v=123", StreamOptions{});
    TEST_CHECK(sid_https_unknown != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        for (const auto& ev : host.events) {
            TEST_CHECK(!ev.play);
        }
        host.events.clear();
    }

    StreamId sid_https_hls = engine.PlayAudioStreamForPlayer(0, "https://cdn.example.com/live/master.m3u8", StreamOptions{});
    TEST_CHECK(sid_https_hls != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        // Only StopAudioStream if replacing, NEVER immediate Play before resolve
        for (const auto& ev : host.events) {
            TEST_CHECK(!ev.play);
        }
    }

    engine.Shutdown();
    std::cout << "[PASS] TestRound27Fase6SyncHttpShortcut\n";
}

void TestRound27Fase8LanAwareUrls() {
    std::vector<LocalIpv4Interface> ifaces = {
        {"192.168.1.10", 0xC0A8010Au, 0xFFFFFF00u} // 192.168.1.10/24
    };
    TEST_CHECK(AudioEngine::ResolveEndpointHostIp("192.168.1.140", "203.0.113.10", ifaces, true) == "192.168.1.10");
    TEST_CHECK(AudioEngine::ResolveEndpointHostIp("198.51.100.25", "203.0.113.10", ifaces, true) == "203.0.113.10");
    TEST_CHECK(AudioEngine::ResolveEndpointHostIp("10.0.0.5", "203.0.113.10", ifaces, true) == "203.0.113.10");
    TEST_CHECK(AudioEngine::ResolveEndpointHostIp("192.168.1.140", "203.0.113.10", ifaces, false) == "203.0.113.10");

    // Ensure real OS interface discovery executes cleanly
    auto os_ifaces = AudioEngine::DiscoverLocalIpv4Interfaces("");
    (void)os_ifaces;
    std::cout << "[PASS] TestRound27Fase8LanAwareUrls\n";
}

void TestRound27Fase9IcyFraming() {
    HttpStreamingServer srv;
    std::string err;
    TEST_CHECK(srv.Start("127.0.0.1", 0, err, false));

    auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
    std::vector<std::uint8_t> audio_payload(40000);
    for (std::size_t i = 0; i < audio_payload.size(); ++i) {
        audio_payload[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xFF);
    }

    StreamEndpointInfo ep;
    ep.token = "icy_tok";
    ep.title = "T\xC3\xADtulo' Teste"; // Título' Teste -> quote must be stripped
    ep.artist = "Artista";
    ep.bitrate_kbps = 128;
    ep.buffer = buf;
    srv.RegisterEndpoint(ep);

    // 1. Request WITHOUT Icy-MetaData -> exact raw bytes, no icy-metaint
    buf->Write(audio_payload.data(), audio_payload.size());
    buf->MarkEof();
    {
        asio::io_context ioc;
        asio::ip::tcp::socket sock(ioc);
        sock.connect({asio::ip::address_v4::loopback(), srv.BoundPort()});
        std::string req = "GET /stream/icy_tok.mp3 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        asio::write(sock, asio::buffer(req));
        std::string raw;
        std::array<char, 4096> tmp{};
        std::error_code ec;
        while (true) {
            std::size_t n = sock.read_some(asio::buffer(tmp), ec);
            if (n > 0) raw.append(tmp.data(), n);
            if (ec) break;
        }
        auto hdr_end = raw.find("\r\n\r\n");
        TEST_CHECK(hdr_end != std::string::npos);
        std::string hdrs = raw.substr(0, hdr_end);
        std::string body = raw.substr(hdr_end + 4);
        TEST_CHECK(hdrs.find("icy-metaint") == std::string::npos);
        TEST_CHECK(body.size() == audio_payload.size());
        TEST_CHECK(std::memcmp(body.data(), audio_payload.data(), audio_payload.size()) == 0);
    }

    // 2. Request WITH Icy-MetaData: 1 -> icy-metaint: 16000 + intact audio + StreamTitle at byte 16000 & updated title at 32000
    auto buf2 = std::make_shared<AudioStreamBuffer>(128 * 1024);
    ep.buffer = buf2;
    srv.RegisterEndpoint(ep);
    buf2->Write(audio_payload.data(), 20000);

    {
        asio::io_context ioc;
        asio::ip::tcp::socket sock(ioc);
        sock.connect({asio::ip::address_v4::loopback(), srv.BoundPort()});
        std::string req = "GET /stream/icy_tok.mp3 HTTP/1.1\r\nHost: 127.0.0.1\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n";
        asio::write(sock, asio::buffer(req));

        // Update metadata cleanly BEFORE chunk >= 32000 so block 2 carries the new track title
        std::thread feeder([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            srv.UpdateEndpointMetadata("icy_tok", "Faixa Nova", "Banda", buf2);
            buf2->Write(audio_payload.data() + 20000, 20000);
            buf2->MarkEof();
        });

        std::string raw;
        std::array<char, 4096> tmp{};
        std::error_code ec;
        while (true) {
            std::size_t n = sock.read_some(asio::buffer(tmp), ec);
            if (n > 0) raw.append(tmp.data(), n);
            if (ec) break;
        }
        feeder.join();

        auto hdr_end = raw.find("\r\n\r\n");
        TEST_CHECK(hdr_end != std::string::npos);
        std::string hdrs = raw.substr(0, hdr_end + 2);
        TEST_CHECK(hdrs.find("icy-metaint: 16000\r\n") != std::string::npos);

        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(raw.data() + hdr_end + 4);
        std::size_t rem = raw.size() - (hdr_end + 4);

        std::vector<std::uint8_t> recovered;
        std::vector<std::string> meta_blocks;
        std::size_t off = 0;
        while (off < rem) {
            std::size_t take_audio = std::min<std::size_t>(16000, rem - off);
            recovered.insert(recovered.end(), p + off, p + off + take_audio);
            off += take_audio;
            if (take_audio == 16000 && off < rem) {
                std::size_t mlen = static_cast<std::size_t>(p[off]) * 16;
                ++off;
                TEST_CHECK(off + mlen <= rem);
                std::string mtext(reinterpret_cast<const char*>(p + off), mlen);
                while (!mtext.empty() && mtext.back() == '\0') mtext.pop_back();
                meta_blocks.push_back(std::move(mtext));
                off += mlen;
            }
        }
        TEST_CHECK(recovered.size() == audio_payload.size());
        TEST_CHECK(std::memcmp(recovered.data(), audio_payload.data(), audio_payload.size()) == 0);
        TEST_CHECK(meta_blocks.size() == 2);
        TEST_CHECK(meta_blocks[0] == "StreamTitle='Artista - T\xEDtulo Teste';");
        TEST_CHECK(meta_blocks[1] == "StreamTitle='Banda - Faixa Nova';");
    }

    srv.Stop();
    std::cout << "[PASS] TestRound27Fase9IcyFraming\n";
}

void TestRound27Fase10LoggingToggles() {
    PluginConfig def;
    TEST_CHECK(def.log_startup_banner);
    TEST_CHECK(def.log_http_listen);
    TEST_CHECK(def.log_public_ip_detected);
    TEST_CHECK(def.log_extractor_status);

    std::string temp_cfg = "build_test_logging_cfg.json";
    {
        std::ofstream ofs(temp_cfg, std::ios::trunc);
        ofs << R"({"logging":{"startup_banner":false,"http_listen":true,"public_ip_detected":false,"extractor_status":false}})";
    }
    PluginConfig loaded = ConfigManager::LoadOrCreate(temp_cfg);
    TEST_CHECK(!loaded.log_startup_banner);
    TEST_CHECK(loaded.log_http_listen);
    TEST_CHECK(!loaded.log_public_ip_detected);
    TEST_CHECK(!loaded.log_extractor_status);
    std::filesystem::remove(temp_cfg);
    std::cout << "[PASS] TestRound27Fase10LoggingToggles\n";
}

void TestV120ProcessingCallbackAndToggle() {
    MockPlatformHost host;
    host.players = {{7, {0.0f, 0.0f, 0.0f}, 0, 0}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // 1. PlayAudioStreamForPlayer with default notify_processing=true fires OnAudioStreamProcessing before Ready
    StreamOptions opts1;
    opts1.notify_processing = true;
    const std::string direct_url = "http://live.hunter.fm/pop_stream";
    StreamId s1 = engine.PlayAudioStreamForPlayer(7, direct_url, opts1);
    TEST_CHECK(s1 != INVALID_STREAM_ID);

    auto wait_cbs = [&]() {
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (!host.ready_stream_ids.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    wait_cbs();

    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.processing_events.size() == 1);
        TEST_CHECK(host.processing_events[0].player_id == 7);
        TEST_CHECK(host.processing_events[0].stream_id == s1);
        TEST_CHECK(host.processing_events[0].url == direct_url);
        TEST_CHECK(host.processing_events[0].from_cache);
        TEST_CHECK(host.callback_names.size() >= 2);
        TEST_CHECK(host.callback_names[0] == "OnAudioStreamProcessing");
        TEST_CHECK(host.callback_names[1] == "OnAudioStreamReady");
        host.processing_events.clear();
        host.ready_stream_ids.clear();
        host.callback_names.clear();
    }

    // 2. notify_processing=false suppresses OnAudioStreamProcessing completely
    StreamOptions opts2;
    opts2.notify_processing = false;
    StreamId s2 = engine.PlayAudioStreamForPlayer(7, "http://live.hunter.fm/rock_stream", opts2);
    TEST_CHECK(s2 != INVALID_STREAM_ID);
    wait_cbs();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.processing_events.empty());
        TEST_CHECK(!host.ready_stream_ids.empty());
        host.callback_names.clear();
        host.ready_stream_ids.clear();
    }

    // 3. Global PlayAudioStream fires OnAudioStreamProcessing with playerid == PAWN_INVALID_PLAYER_ID (65535)
    StreamOptions opts3;
    opts3.notify_processing = true;
    const std::string yt_url = "https://www.youtube.com/watch?v=ynMk2EwRi4Q";
    StreamId s3 = engine.PlayAudioStream(yt_url, opts3);
    TEST_CHECK(s3 != INVALID_STREAM_ID);
    for (int i = 0; i < 15; ++i) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.processing_events.size() == 1);
        TEST_CHECK(host.processing_events[0].player_id == PAWN_INVALID_PLAYER_ID);
        TEST_CHECK(host.processing_events[0].player_id == 65535);
        TEST_CHECK(host.processing_events[0].stream_id == s3);
        TEST_CHECK(host.processing_events[0].url == yt_url);
        TEST_CHECK(!host.processing_events[0].from_cache);
    }

    engine.Shutdown();
    std::cout << "[PASS] TestV120ProcessingCallbackAndToggle\n";
}

void TestV120SetDistanceAndMp3FrameSync() {
    // 1. Verify AudioStreamBuffer::PeekAt + HttpStreamingServer MP3 frame alignment on ?resume=
    {
        HttpStreamingServer srv;
        std::string err;
        TEST_CHECK(srv.Start("127.0.0.1", 0, err));
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        // 10 bytes junk/mid-frame + two valid 48kHz 128kbps MP3 frames (384 bytes each: FF FB 94 00 ...)
        std::vector<std::uint8_t> data(10, 0x11);
        std::vector<std::uint8_t> frame(384, 0x22);
        frame[0] = 0xFF; frame[1] = 0xFB; frame[2] = 0x94; frame[3] = 0x00;
        data.insert(data.end(), frame.begin(), frame.end());
        data.insert(data.end(), frame.begin(), frame.end());
        buf->Write(data.data(), data.size());
        buf->MarkEof();

        std::uint8_t peek_tmp[4]{};
        TEST_CHECK(buf->PeekAt(10, peek_tmp, 4) == 4);
        TEST_CHECK(peek_tmp[0] == 0xFF && peek_tmp[1] == 0xFB);

        StreamEndpointInfo ep;
        ep.token = "mp3align";
        ep.bitrate_kbps = 128;
        ep.buffer = buf;
        srv.RegisterEndpoint(ep);

        asio::io_context io;
        asio::ip::tcp::socket sock(io);
        sock.connect({asio::ip::address_v4::loopback(), srv.BoundPort()});
        std::string req = "GET /stream/mp3align.mp3?resume=3&v=2 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        asio::write(sock, asio::buffer(req));
        std::string raw;
        std::array<char, 2048> tmp{};
        std::error_code ec;
        while (true) {
            std::size_t n = sock.read_some(asio::buffer(tmp), ec);
            if (n > 0) raw.append(tmp.data(), n);
            if (ec) break;
        }
        auto hdr_end = raw.find("\r\n\r\n");
        TEST_CHECK(hdr_end != std::string::npos);
        std::string body = raw.substr(hdr_end + 4);
        // Skipped mid-frame bytes 3..9 and aligned directly onto frame sync at offset 10
        TEST_CHECK(body.size() == 384 * 2);
        TEST_CHECK(static_cast<std::uint8_t>(body[0]) == 0xFF && static_cast<std::uint8_t>(body[1]) == 0xFB);
        srv.Stop();
    }

    // 2. Native passthrough fast-path + transcoded personal + global distance updates
    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {30.0f, 0.0f, 0.0f}, 0, 0},
        {3, {75.0f, 0.0f, 0.0f}, 0, 0}
    };

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;

    TEST_PHASE("Initialize");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // 2a. RegisterNativePassthroughSession (PlayAudioStreamForPlayer native fast-path) -> SetPlayerAudioStreamDistance
    TEST_PHASE("passthrough SetPlayerAudioStreamDistance");
    StreamOptions nat_opts;
    nat_opts.position = {10.0f, 20.0f, 3.0f};
    nat_opts.distance = 50.0f;
    nat_opts.use_position = true;
    engine.RegisterNativePassthroughSession(1, "http://example.com/song.mp3", nat_opts);
    TEST_CHECK(engine.GetPlayerStreamId(1) != INVALID_STREAM_ID);
    // Player 1 is at {0,0,0}, stream is at {10,20,3} (~22.56m away)
    TEST_CHECK(!engine.SetPlayerAudioStreamDistance(1, -5.0f));
    TEST_CHECK(engine.SetPlayerAudioStreamDistance(1, 25.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].player_id == 1);
        TEST_CHECK(host.events[0].play);
        TEST_CHECK(host.events[0].url == "http://example.com/song.mp3");
        TEST_CHECK(std::abs(host.events[0].pos.x - 10.0f) < 0.01f);
        TEST_CHECK(std::abs(host.events[0].distance - 25.0f) < 0.01f);
        TEST_CHECK(host.events[0].use_pos);
        host.events.clear();
    }
    // Shrinking below ~22.56m must stop, expanding back above restarts
    TEST_CHECK(engine.SetPlayerAudioStreamDistance(1, 15.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(!host.events[0].play);
        host.events.clear();
    }
    TEST_CHECK(engine.SetPlayerAudioStreamDistance(1, 30.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].play);
        TEST_CHECK(std::abs(host.events[0].distance - 30.0f) < 0.01f);
        host.events.clear();
    }
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
        host.ready_stream_ids.clear();
    }

    // 2b. Local HTTP MP3 server for transcoded global SA_PlayAudioStream across multiple players
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF; mp3_frame[1] = 0xFB; mp3_frame[2] = 0x90; mp3_frame[3] = 0x64;
    std::vector<std::uint8_t> mp3_payload;
    for (int i = 0; i < 1500; ++i) {
        mp3_payload.insert(mp3_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }
    TEST_PHASE("test server & global zone.mp3");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t srv_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sb;
            asio::read_until(sock, sb, "\r\n\r\n", ec);
            if (!ec) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: " +
                                  std::to_string(mp3_payload.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(mp3_payload), ec);
            }
            sock.close(ec);
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", engine.GetHttpPort()});
    StreamOptions gopts;
    gopts.position = {0.0f, 0.0f, 0.0f};
    gopts.distance = 40.0f; // covers players 1 (0m) and 2 (30m), excludes 3 (75m)
    gopts.use_position = true;
    gopts.notify_processing = false;
    StreamId gsid = engine.PlayAudioStream("http://127.0.0.1:" + std::to_string(srv_port) + "/zone.mp3", gopts);
    TEST_CHECK(gsid != INVALID_STREAM_ID);

    auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (!host.ready_stream_ids.empty() && host.events.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 2); // players 1 & 2 started with distance 40
        host.events.clear();
    }

    // Shrink distance to 20.0 -> player 2 (30m) must STOP, player 1 (0m) updates to 20.0 without stopping first
    TEST_PHASE("distance shrink & expand");
    TEST_CHECK(engine.SetAudioStreamDistance(gsid, 20.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p2_stopped = false;
        bool p1_updated = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 2 && !ev.play) p2_stopped = true;
            if (ev.player_id == 1 && ev.play && std::abs(ev.distance - 20.0f) < 0.01f) p1_updated = true;
        }
        TEST_CHECK(p2_stopped);
        TEST_CHECK(p1_updated);
        host.events.clear();
    }

    // Expand distance to 80.0 -> player 1 updates to 80.0, players 2 (30m) and 3 (75m) both start with 80.0
    TEST_CHECK(engine.SetAudioStreamDistance(gsid, 80.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        int played_mask = 0;
        for (const auto& ev : host.events) {
            if (ev.play && std::abs(ev.distance - 80.0f) < 0.01f) {
                played_mask |= (1 << ev.player_id);
            }
        }
        TEST_CHECK(played_mask == ((1 << 1) | (1 << 2) | (1 << 3)));
    }

    TEST_PHASE("Shutdown & join server");
    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, srv_port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestV120SetDistanceAndMp3FrameSync\n";
}

void TestV120ProcessDirectAudioFiles() {
    // 1. Direct music file classifier excludes radios/playlists/platforms/queries
    TEST_PHASE("classifier");
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/music/track.mp3"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("https://cdn.example.com/song.OGG?token=123"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/clip.wav"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/hires.flac"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/stream.aac"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/song.m4a"));
    TEST_CHECK(StreamResolver::IsDirectAudioFileUrl("http://example.com/song.opus"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("http://radio.example.com:8000/stream"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("http://radio.example.com/listen.pls"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("http://radio.example.com/station.m3u"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("http://radio.example.com/live.m3u8"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    TEST_CHECK(!StreamResolver::IsDirectAudioFileUrl("daft punk around the world.mp3"));

    TEST_CHECK(StreamResolver::IsHlsPlaylistUrl("https://cdn.example.com/live/master.m3u8?token=abc"));
    TEST_CHECK(!StreamResolver::IsHlsPlaylistUrl("https://cdn.example.com/song.mp3"));
    TEST_CHECK(!StreamResolver::IsHlsPlaylistUrl("http://radio.example.com/station.m3u"));

    // Cached extensionless HTTPS radio must preserve SourceType::DirectAudio in StreamResolver cache lookup
    {
        PluginConfig cache_cfg;
        cache_cfg.auto_download_extractor = false;
        BinaryManager bm(cache_cfg);
        MetadataCache mc(16, 60);
        TrackMetadata direct_radio;
        direct_radio.original_url = "https://probed-radio.example.com/live";
        direct_radio.direct_media_url = "https://probed-radio.example.com/live";
        direct_radio.extractor_name = "direct";
        direct_radio.is_live = true;
        mc.PutTrack(direct_radio.original_url, direct_radio);

        StreamResolver resolver(cache_cfg, bm, mc);
        ResolveResult rr = resolver.Resolve("https://probed-radio.example.com/live");
        TEST_CHECK(rr.success);
        TEST_CHECK(rr.source_type == SourceType::DirectAudio);
        TEST_CHECK(rr.metadata.is_live);
    }

    // Local HTTP test server returning valid MP3 bytes (both /song.mp3, /live, /radio.pls)
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF; mp3_frame[1] = 0xFB; mp3_frame[2] = 0x90; mp3_frame[3] = 0x64;
    std::vector<std::uint8_t> mp3_payload;
    for (int i = 0; i < 600; ++i) {
        mp3_payload.insert(mp3_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }

    TEST_PHASE("test server");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t srv_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex sock_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;
    std::thread srv([&]() {
        std::vector<std::thread> workers;
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);
            {
                std::lock_guard<std::mutex> lk(sock_mtx);
                sockets.push_back(sock);
            }
            workers.emplace_back([sock, &mp3_payload]() {
                std::error_code io_ec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", io_ec);
                if (!io_ec) {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: " +
                                      std::to_string(mp3_payload.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    if (!io_ec) asio::write(*sock, asio::buffer(mp3_payload), io_ec);
                }
                sock->close(io_ec);
            });
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    });

    const std::string base = "http://127.0.0.1:" + std::to_string(srv_port);
    const std::string song_url = base + "/song.mp3";
    const std::string radio_url = base + "/live";
    const std::string pls_url = base + "/radio.pls";

    // 2. Per-call process_direct_audio_files (default false -> passthrough, true -> transcoded on direct music files; live radio/pls stay passthrough)
    {
        MockPlatformHost host;
        host.players = {
            {1, {0.0f, 0.0f, 0.0f}, 0, 0},
            {2, {0.0f, 0.0f, 0.0f}, 0, 0},
            {3, {0.0f, 0.0f, 0.0f}, 0, 0},
            {4, {0.0f, 0.0f, 0.0f}, 0, 0}
        };
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.allow_private_network_urls = true;
        cfg.auto_download_extractor = false;

        TEST_PHASE("Initialize");
        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());

        // Player 1: default process_direct_audio_files == false -> http://.../song.mp3 uses native passthrough
        TEST_PHASE("play passthrough player 1");
        StreamOptions passthrough_opts;
        TEST_CHECK(!passthrough_opts.process_direct_audio_files);
        StreamId p1_sid = engine.PlayAudioStreamForPlayer(1, song_url, passthrough_opts);
        TEST_CHECK(p1_sid != INVALID_STREAM_ID);

        // Player 2: process_direct_audio_files == true -> .mp3 is transcoded into /stream/
        TEST_PHASE("play processed player 2 & wait /stream/");
        StreamOptions process_opts;
        process_opts.process_direct_audio_files = true;
        StreamId song_sid = engine.PlayAudioStreamForPlayer(2, song_url, process_opts);
        TEST_CHECK(song_sid != INVALID_STREAM_ID);

        // Wait for transcoder pipeline to publish local /stream/ URL for player 2
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        bool saw_proxy_url = false;
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& ev : host.events) {
                    if (ev.player_id == 2 && ev.play && ev.url.find("/stream/") != std::string::npos) {
                        saw_proxy_url = true;
                    }
                }
            }
            if (saw_proxy_url) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(saw_proxy_url);

        // Live extensionless radio and .pls playlist stay passthrough even when process_direct_audio_files == true
        TEST_PHASE("radio & .pls");
        StreamId radio_sid = engine.PlayAudioStreamForPlayer(3, radio_url, process_opts);
        StreamId pls_sid = engine.PlayAudioStreamForPlayer(4, pls_url, process_opts);
        TEST_CHECK(radio_sid != INVALID_STREAM_ID);
        TEST_CHECK(pls_sid != INVALID_STREAM_ID);
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            bool p1_passthrough = false;
            bool p3_passthrough = false;
            bool p4_passthrough = false;
            bool saw_processing_song = false;
            for (const auto& ev : host.events) {
                if (ev.player_id == 1 && ev.play && ev.url == song_url) p1_passthrough = true;
                if (ev.player_id == 3 && ev.play && ev.url == radio_url) p3_passthrough = true;
                if (ev.player_id == 4 && ev.play && ev.url == pls_url) p4_passthrough = true;
            }
            for (const auto& pev : host.processing_events) {
                if (pev.player_id == 2 && pev.stream_id == song_sid && pev.url == song_url) {
                    saw_processing_song = true;
                }
            }
            TEST_CHECK(p1_passthrough);
            TEST_CHECK(p3_passthrough);
            TEST_CHECK(p4_passthrough);
            TEST_CHECK(saw_processing_song);
        }

        TEST_PHASE("Shutdown");
        engine.Shutdown();
    }

    TEST_PHASE("stop acceptor & join server");
    stop_srv.store(true);
    CloseTestSockets(sockets, sock_mtx);
    StopTestAcceptor(acc, srv_port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestV120ProcessDirectAudioFiles\n";
}

void TestV120SetPosAndAutoAttach() {
    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {30.0f, 0.0f, 0.0f}, 0, 0},
        {3, {75.0f, 0.0f, 0.0f}, 0, 0},
        {4, {120.0f, 0.0f, 0.0f}, 3, 2}
    };
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.vehicles[55] = {15.0f, 0.0f, 0.0f};
    }

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.allow_private_network_urls = true;
    cfg.auto_download_extractor = false;

    TEST_PHASE("Initialize");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // 1. Personal passthrough started with usepos=false -> SetPlayerAudioStreamPos forces usepos=true and range filtering
    TEST_PHASE("player 1 SetPos & AttachToVehicle");
    StreamId p1_sid = engine.PlayAudioStreamForPlayer(1, "http://radio.example.com/song.mp3", StreamOptions{});
    TEST_CHECK(p1_sid != INVALID_STREAM_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty());
        TEST_CHECK(!host.events.back().use_pos);
        host.events.clear();
    }
    TEST_CHECK(!engine.SetPlayerAudioStreamPos(1, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
    TEST_CHECK(engine.SetPlayerAudioStreamPos(1, 5.0f, 2.0f, 1.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].play);
        TEST_CHECK(host.events[0].use_pos);
        TEST_CHECK(std::abs(host.events[0].pos.x - 5.0f) < 0.01f);
        host.events.clear();
    }
    // Move far out of default 50m radius -> stops; move back inside -> restarts cleanly
    TEST_CHECK(engine.SetPlayerAudioStreamPos(1, 200.0f, 0.0f, 0.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1 && !host.events[0].play);
        host.events.clear();
    }
    TEST_CHECK(engine.SetPlayerAudioStreamPos(1, 10.0f, 0.0f, 0.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1 && host.events[0].play && host.events[0].use_pos);
        host.events.clear();
    }

    // 2. AttachStreamToVehicle when started with usepos=false tracks vehicle coordinates on server and sends 2D (use_pos=false)
    TEST_CHECK(!engine.AttachStreamToVehicle(p1_sid, 999)); // non-existent vehicle rejected
    TEST_CHECK(engine.AttachStreamToVehicle(p1_sid, 55));   // vehicle 55 is at x=15 (within 50m of player 1)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].play && !host.events[0].use_pos);
        host.events.clear();
    }
    // Move player 1 outside 50m of vehicle 55 and verify Stop; then AttachStreamToPlayer to player 4 (world 3, int 2, x=120)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.vehicles[55] = {200.0f, 0.0f, 0.0f};
    }
    engine.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(230));
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p1_stopped = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 1 && !ev.play) p1_stopped = true;
        }
        TEST_CHECK(p1_stopped);
        host.events.clear();
    }
    // Stop player 1 personal stream before testing global stream
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }

    // 3. Global transcoded SA_PlayAudioStream with usepos=false -> SetAudioStreamPos + AttachStreamToVehicle
    std::vector<std::uint8_t> mp3_frame(417, 0);
    mp3_frame[0] = 0xFF; mp3_frame[1] = 0xFB; mp3_frame[2] = 0x90; mp3_frame[3] = 0x64;
    std::vector<std::uint8_t> mp3_payload;
    for (int i = 0; i < 1200; ++i) {
        mp3_payload.insert(mp3_payload.end(), mp3_frame.begin(), mp3_frame.end());
    }
    TEST_PHASE("test server & global.mp3");
    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t srv_port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::thread srv([&]() {
        while (!stop_srv.load()) {
            asio::ip::tcp::socket sock(srv_io);
            std::error_code ec;
            acc.accept(sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(sock);
            asio::streambuf sb;
            asio::read_until(sock, sb, "\r\n\r\n", ec);
            if (!ec) {
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: " +
                                  std::to_string(mp3_payload.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(sock, asio::buffer(hdr), ec);
                if (!ec) asio::write(sock, asio::buffer(mp3_payload), ec);
            }
            sock.close(ec);
        }
    });

    StreamOptions gopts;
    gopts.use_position = false;
    gopts.distance = 40.0f;
    gopts.notify_processing = false;
    StreamId gsid = engine.PlayAudioStream("http://127.0.0.1:" + std::to_string(srv_port) + "/global.mp3", gopts);
    TEST_CHECK(gsid != INVALID_STREAM_ID);

    auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (host.events.size() >= 4) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 4); // usepos=false initially covers all 4 players
        host.events.clear();
    }

    // SetAudioStreamPos(75, 0, 0): sets usepos=true -> keeps player 3 (at x=75), stops 1 (0), 2 (30, >40m? 45m), 4 (world=3, x=120)
    TEST_PHASE("SetAudioStreamPos & AttachStreamToPlayer");
    TEST_CHECK(engine.SetAudioStreamPos(gsid, 75.0f, 0.0f, 0.0f));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        int stopped_mask = 0;
        bool p3_updated = false;
        for (const auto& ev : host.events) {
            if (!ev.play) stopped_mask |= (1 << ev.player_id);
            if (ev.player_id == 3 && ev.play && ev.use_pos && std::abs(ev.pos.x - 75.0f) < 0.01f) {
                p3_updated = true;
            }
        }
        TEST_CHECK(p3_updated);
        TEST_CHECK(stopped_mask == ((1 << 1) | (1 << 2) | (1 << 4)));
        host.events.clear();
    }

    // AttachStreamToPlayer to player 4 (world 3, interior 2, x=120) -> stops 3, starts 4 immediately with 2D attached audio (use_pos=false)!
    TEST_CHECK(engine.AttachStreamToPlayer(gsid, 4));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p3_stopped = false;
        bool p4_started = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 3 && !ev.play) p3_stopped = true;
            if (ev.player_id == 4 && ev.play && !ev.use_pos) {
                p4_started = true;
            }
        }
        TEST_CHECK(p3_stopped);
        TEST_CHECK(p4_started);
    }

    TEST_PHASE("Shutdown & join server");
    engine.Shutdown();
    stop_srv.store(true);
    StopTestAcceptor(acc, srv_port);
    if (srv.joinable()) srv.join();
    std::cout << "[PASS] TestV120SetPosAndAutoAttach\n";
}

void TestV200CompleteChannelSystem() {
    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {0.0f, 0.0f, 0.0f}, 0, 0},
        {7, {0.0f, 0.0f, 0.0f}, 0, 0}
    };
    host.player_names = {
        {1, "Walter"},
        {2, "Alice"},
        {7, "Walter"}
    };

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // 1. Server-owned Manual channel + password + queue + enumeration
    ChannelId ch_srv = engine.CreateChannel("Main Lobby FM");
    TEST_CHECK(ch_srv != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.GetChannelCount() == 1);
    TEST_CHECK(engine.GetChannelIdAt(0) == ch_srv);
    TEST_CHECK(engine.GetChannelIdAt(1) == INVALID_CHANNEL_ID);

    ChannelInfo info{};
    TEST_CHECK(engine.GetChannelInfo(ch_srv, info));
    TEST_CHECK(info.name == "Main Lobby FM");
    TEST_CHECK(info.creator_name == "Server");
    TEST_CHECK(!info.has_password);
    TEST_CHECK(info.destroy_mode == ChannelDestroyMode::Manual);
    TEST_CHECK(info.listener_count == 0);

    // QueueChannel on idle channel starts playing immediately (queue remains empty until a second track is added)
    TEST_CHECK(engine.QueueChannel(ch_srv, "https://example.com/q1.mp3"));
    TEST_CHECK(engine.GetChannelState(ch_srv) == StreamState::Resolving);
    TEST_CHECK(engine.QueueChannel(ch_srv, "https://example.com/q2.mp3"));
    TEST_CHECK(engine.QueueChannel(ch_srv, "https://example.com/q3.mp3"));
    std::string qitem;
    TEST_CHECK(engine.GetChannelQueueItem(ch_srv, 0, qitem) && qitem == "https://example.com/q2.mp3");
    TEST_CHECK(engine.GetChannelQueueItem(ch_srv, 1, qitem) && qitem == "https://example.com/q3.mp3");
    TEST_CHECK(!engine.GetChannelQueueItem(ch_srv, 2, qitem));
    TEST_CHECK(engine.GetChannelInfo(ch_srv, info) && info.queue_count == 2);
    TEST_CHECK(engine.ClearChannelQueue(ch_srv));
    TEST_CHECK(engine.GetChannelInfo(ch_srv, info) && info.queue_count == 0);
    TEST_CHECK(engine.StopChannel(ch_srv));

    // Password protection: Verify + Join rejection/acceptance + removal
    TEST_CHECK(engine.SetChannelPassword(ch_srv, "secret123"));
    TEST_CHECK(engine.VerifyChannelPassword(ch_srv, "secret123"));
    TEST_CHECK(!engine.VerifyChannelPassword(ch_srv, "wrong"));
    TEST_CHECK(!engine.JoinChannel(2, ch_srv, ""));
    TEST_CHECK(!engine.JoinChannel(2, ch_srv, "wrong"));
    TEST_CHECK(!engine.IsPlayerInChannel(2, ch_srv));
    TEST_CHECK(engine.JoinChannel(2, ch_srv, "secret123"));
    TEST_CHECK(engine.IsPlayerInChannel(2, ch_srv));
    TEST_CHECK(engine.GetChannelInfo(ch_srv, info) && info.listener_count == 1 && info.has_password);

    // Clear password -> anyone joins without password
    TEST_CHECK(engine.SetChannelPassword(ch_srv, ""));
    TEST_CHECK(engine.VerifyChannelPassword(ch_srv, ""));
    TEST_CHECK(engine.JoinChannel(1, ch_srv));
    TEST_CHECK(engine.GetChannelInfo(ch_srv, info) && info.listener_count == 2 && !info.has_password);

    // 2. Creator disconnect immediate destroy (OnDisconnect)
    ChannelId ch_disc = engine.CreateChannel("Walter Live", 1, "dj99", ChannelDestroyMode::OnDisconnect, 0);
    TEST_CHECK(ch_disc != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.GetChannelInfo(ch_disc, info));
    TEST_CHECK(info.creator_name == "Walter");
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.has_password);
    TEST_CHECK(engine.JoinChannel(2, ch_disc, "dj99"));

    // Disconnect player 1 -> ch_disc must be destroyed immediately with reason CreatorDisconnected!
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 1; }), host.players.end());
        host.destroyed_channels.clear();
    }
    engine.Tick(); // detects creator disconnect -> starts async OnAudioStreamChannelDestroyed dispatch
    TEST_CHECK(!engine.GetChannelInfo(ch_disc, info));
    TEST_CHECK(engine.GetPlayerChannelId(2) == INVALID_CHANNEL_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.destroyed_channels.empty()); // Asynchronous: must NOT fire before next Tick
    }
    engine.Tick(); // delivers queued OnAudioStreamChannelDestroyed on the next tick
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.destroyed_channels.empty());
        TEST_CHECK(host.destroyed_channels[0].channel_id == ch_disc);
        TEST_CHECK(host.destroyed_channels[0].reason == static_cast<std::int32_t>(ChannelDestroyReason::CreatorDisconnected));
        host.destroyed_channels.clear();
    }

    // 3. Timeout destroy mode: timer stays reset while creator online, runs only offline, resets on reclaim, expires after offline timeout
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({1, {0.0f, 0.0f, 0.0f}, 0, 0});
    }
    ChannelId ch_to = engine.CreateChannel("Walter Timeout Room", 1, "", ChannelDestroyMode::Timeout, 2);
    TEST_CHECK(ch_to != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(info.creator_name == "Walter");
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.destroy_timeout_seconds == 2);
    TEST_CHECK(info.destroy_remaining_seconds == 2);

    // Wait 1.5s while creator is STILL online -> timer MUST stay reset at 2s and channel MUST NOT expire!
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.destroy_remaining_seconds == 2);

    // Creator disconnects -> timer starts counting down while offline, creator name stays "Walter"
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 1; }), host.players.end());
    }
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(!info.creator_online);
    TEST_CHECK(info.creator_name == "Walter");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(info.destroy_remaining_seconds <= 1); // ticked down while offline!

    // Creator reconnects as player 7 and reclaims via SetChannelCreator -> timer resets back to 2s!
    TEST_CHECK(engine.SetChannelCreator(ch_to, 7));
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.destroy_remaining_seconds == 2);

    // Creator 7 disconnects cleanly -> after timeout (1s short timeout via SetChannelDestroyPolicy), channel auto-destroys!
    TEST_CHECK(engine.SetChannelDestroyPolicy(ch_to, ChannelDestroyMode::Timeout, 1));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 7; }), host.players.end());
        host.destroyed_channels.clear();
    }
    engine.Tick(); // detects creator 7 disconnect -> starts 1s countdown
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    engine.Tick(); // CleanupExpiredChannels destroys channel & queues async callback
    TEST_CHECK(!engine.GetChannelInfo(ch_to, info));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.destroyed_channels.empty());
    }
    engine.Tick(); // next tick delivers TimeoutExpired
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.destroyed_channels.empty());
        TEST_CHECK(host.destroyed_channels[0].channel_id == ch_to);
        TEST_CHECK(host.destroyed_channels[0].reason == static_cast<std::int32_t>(ChannelDestroyReason::TimeoutExpired));
        host.destroyed_channels.clear();
    }

    // Manual destroy fires Manual reason asynchronously on the next tick
    TEST_CHECK(engine.DestroyChannel(ch_srv));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.destroyed_channels.empty());
    }
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.destroyed_channels.empty());
        TEST_CHECK(host.destroyed_channels[0].channel_id == ch_srv);
        TEST_CHECK(host.destroyed_channels[0].reason == static_cast<std::int32_t>(ChannelDestroyReason::Manual));
    }

    engine.Shutdown();
    std::cout << "[PASS] TestV200CompleteChannelSystem\n";
}

static void TestV200CreatorReconnectRenameAndPolicyGuards() {
    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {0.0f, 0.0f, 0.0f}, 0, 0}
    };
    host.player_names = {
        {1, "Walter"},
        {2, "Alice"}
    };

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_PHASE("Initialize");
    TEST_CHECK(engine.Initialize());
    ChannelInfo info{};

    // Problem 4: server-owned channel (INVALID_PLAYER_ID) refuses Timeout/OnDisconnect & stays alive
    TEST_PHASE("server-owned & revert");
    ChannelId ch_srv_req = engine.CreateChannel("Server Timeout Try", INVALID_PLAYER_ID, "", ChannelDestroyMode::Timeout, 1);
    TEST_CHECK(ch_srv_req != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.GetChannelInfo(ch_srv_req, info));
    TEST_CHECK(info.destroy_mode == ChannelDestroyMode::Manual);
    TEST_CHECK(!engine.SetChannelDestroyPolicy(ch_srv_req, ChannelDestroyMode::Timeout, 1));
    TEST_CHECK(!engine.SetChannelDestroyPolicy(ch_srv_req, ChannelDestroyMode::OnDisconnect, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_srv_req, info)); // Still alive!

    // Removing creator via SetChannelCreator(INVALID_PLAYER_ID) reverts Timeout back to Manual
    ChannelId ch_revert = engine.CreateChannel("Revert Room", 1, "", ChannelDestroyMode::Timeout, 1);
    TEST_CHECK(ch_revert != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.SetChannelCreator(ch_revert, INVALID_PLAYER_ID));
    TEST_CHECK(engine.GetChannelInfo(ch_revert, info));
    TEST_CHECK(info.destroy_mode == ChannelDestroyMode::Manual);
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_revert, info));
    TEST_CHECK(engine.DestroyChannel(ch_revert));
    TEST_CHECK(engine.DestroyChannel(ch_srv_req));
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.destroyed_channels.clear();
    }

    // (a) Creator stays online on Timeout (2s) -> countdown stays reset at 2s even past 2s
    TEST_PHASE("creator online timeout");
    ChannelId ch_to = engine.CreateChannel("Walter Timeout", 1, "", ChannelDestroyMode::Timeout, 2);
    TEST_CHECK(ch_to != INVALID_CHANNEL_ID);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.destroy_remaining_seconds == 2);

    // (b) Creator exits, SAME playerid (1) connects with DIFFERENT name -> countdown continues and destroys channel
    TEST_PHASE("different name timeout");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 1; }), host.players.end());
    }
    engine.OnPlayerDisconnect(1);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({1, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[1] = "Stranger";
    }
    engine.OnPlayerConnect(1);
    TEST_CHECK(engine.GetChannelInfo(ch_to, info));
    TEST_CHECK(!info.creator_online);
    std::this_thread::sleep_for(std::chrono::milliseconds(2150));
    engine.Tick();
    TEST_CHECK(!engine.GetChannelInfo(ch_to, info));

    // (c) & (d) Creator exits and reconnects on DIFFERENT playerid (9) with matching name (case-insensitive) -> resets countdown;
    //     subsequent second disconnect gives a fresh N=2s countdown instead of expiring from the first disconnect.
    TEST_PHASE("rejoin different id");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.player_names[1] = "Walter";
    }
    ChannelId ch_rejoin = engine.CreateChannel("Rejoin Room", 1, "", ChannelDestroyMode::Timeout, 2);
    TEST_CHECK(ch_rejoin != INVALID_CHANNEL_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 1; }), host.players.end());
    }
    engine.OnPlayerDisconnect(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({9, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[9] = "wAlTeR";
    }
    engine.OnPlayerConnect(9);
    TEST_CHECK(engine.GetChannelInfo(ch_rejoin, info));
    TEST_CHECK(info.creator_online);
    TEST_CHECK(info.creator_player_id == 9);
    TEST_CHECK(info.destroy_remaining_seconds == 2);
    // Past original 2s total while creator 9 stays connected -> must still survive!
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_rejoin, info));

    // (d) Second disconnect starts a fresh 2s timer: 1.0s later still alive, 2.2s later destroyed!
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 9; }), host.players.end());
    }
    engine.OnPlayerDisconnect(9);
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_rejoin, info));
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    engine.Tick();
    TEST_CHECK(!engine.GetChannelInfo(ch_rejoin, info));

    // (e) Reconnect in Manual mode restores creator_online so OnDisconnect triggers on the next disconnect
    TEST_PHASE("manual->on_disconnect");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({3, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[3] = "Carlos";
    }
    ChannelId ch_manual_to_disc = engine.CreateChannel("Manual->Disc", 3, "", ChannelDestroyMode::Manual, 0);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 3; }), host.players.end());
    }
    engine.OnPlayerDisconnect(3);
    TEST_CHECK(engine.GetChannelInfo(ch_manual_to_disc, info));
    TEST_CHECK(!info.creator_online);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({12, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[12] = "Carlos";
    }
    engine.OnPlayerConnect(12);
    TEST_CHECK(engine.GetChannelInfo(ch_manual_to_disc, info) && info.creator_online && info.creator_player_id == 12);
    TEST_CHECK(engine.SetChannelDestroyPolicy(ch_manual_to_disc, ChannelDestroyMode::OnDisconnect, 0));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 12; }), host.players.end());
    }
    engine.OnPlayerDisconnect(12);
    TEST_CHECK(!engine.GetChannelInfo(ch_manual_to_disc, info));

    // (f) & (g) Tag login & temporary intermediate names:
    TEST_PHASE("tag & login rename");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({4, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[4] = "Walter";
    }
    ChannelId ch_tag = engine.CreateChannel("Tag Room", 4, "", ChannelDestroyMode::Timeout, 2);
    // (g) Temporary numeric name "12345" keeps online link, then "[DKS]Walter" updates creator_name cleanly
    engine.OnPlayerNameChanged(4, "12345");
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && info.creator_online && info.creator_name == "12345");
    engine.OnPlayerNameChanged(4, "[DKS]Walter");
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && info.creator_online && info.creator_name == "[DKS]Walter");

    // Disconnect -> reconnect as plain "Walter" first (must NOT match!) -> rename on login to "[DKS]Walter" (matches & resets!)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(std::remove_if(host.players.begin(), host.players.end(),
            [](const PlayerSnapshot& p) { return p.id == 4; }), host.players.end());
        host.player_names[4] = "[DKS]Walter";
    }
    engine.OnPlayerDisconnect(4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.push_back({15, {0.0f, 0.0f, 0.0f}, 0, 0});
        host.player_names[15] = "Walter";
    }
    engine.OnPlayerConnect(15);
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && !info.creator_online); // bare "Walter" does not match "[DKS]Walter"
    engine.OnPlayerNameChanged(15, "[DKS]Walter");
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && info.creator_online && info.creator_player_id == 15);
    TEST_CHECK(info.destroy_remaining_seconds == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2150));
    engine.Tick();
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info)); // still alive because login rename matched!

    // (h) SetPlayerName hook return-value decision helper: only ret == 1 updates AudioEngine
    TEST_PHASE("SetPlayerName hook & Shutdown");
    using DummyAmx = void*;
    using DummyCell = std::int32_t;
    using FakeOrigFn = DummyCell (*)(DummyAmx, DummyCell*);
    auto ret_zero = [](DummyAmx, DummyCell*) -> DummyCell { return 0; };
    auto ret_neg = [](DummyAmx, DummyCell*) -> DummyCell { return -1; };
    auto ret_one = [](DummyAmx, DummyCell*) -> DummyCell { return 1; };

    TEST_CHECK((ExecuteSetPlayerNameHook<DummyAmx, DummyCell, FakeOrigFn>(&engine, nullptr, nullptr, nullptr, 15, "IgnoredNull") == 0));
    TEST_CHECK((ExecuteSetPlayerNameHook<DummyAmx, DummyCell, FakeOrigFn>(&engine, ret_zero, nullptr, nullptr, 15, "IgnoredZero") == 0));
    TEST_CHECK((ExecuteSetPlayerNameHook<DummyAmx, DummyCell, FakeOrigFn>(&engine, ret_neg, nullptr, nullptr, 15, "IgnoredNeg") == -1));
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && info.creator_name == "[DKS]Walter");

    TEST_CHECK((ExecuteSetPlayerNameHook<DummyAmx, DummyCell, FakeOrigFn>(&engine, ret_one, nullptr, nullptr, 15, "[VIP]Walter") == 1));
    TEST_CHECK(engine.GetChannelInfo(ch_tag, info) && info.creator_name == "[VIP]Walter");

    engine.Shutdown();
    std::cout << "[PASS] TestV200CreatorReconnectRenameAndPolicyGuards\n";
}

static void TestV200YtDlpCachePruning() {
    const auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path base_dir =
        std::filesystem::temp_directory_path() / ("sampaudiostream_cache_prune_" + unique_suffix);

    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    // Non-existent cache directory is a safe no-op and must NOT be created.
    TEST_CHECK(StreamResolver::PruneYtDlpCacheDir(base_dir.string()) == 0);
    TEST_CHECK(!std::filesystem::exists(base_dir, ec));

    const std::filesystem::path sig_dir = base_dir / "youtube-sigfuncs";
    const std::filesystem::path solver_dir = base_dir / "challenge-solver";
    const std::filesystem::path unknown_dir = base_dir / "custom-unknown";
    TEST_CHECK(std::filesystem::create_directories(sig_dir, ec) && !ec);
    TEST_CHECK(std::filesystem::create_directories(solver_dir, ec) && !ec);
    TEST_CHECK(std::filesystem::create_directories(unknown_dir, ec) && !ec);

    auto write_sized_file_with_age = [](const std::filesystem::path& p, std::size_t bytes, std::chrono::seconds age) {
        {
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            std::string payload(bytes, 'x');
            ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        }
        std::error_code tec;
        const auto ts = std::filesystem::file_time_type::clock::now() - age;
        std::filesystem::last_write_time(p, ts, tec);
        TEST_CHECK(!tec);
    };

    // >4 recent files (< 7d) in allowlisted youtube-sigfuncs must ALL be kept; >7d files deleted.
    std::vector<std::filesystem::path> fresh_sig_files;
    for (int i = 0; i < 6; ++i) {
        auto p = sig_dir / ("fresh_" + std::to_string(i) + ".json");
        write_sized_file_with_age(p, 100, std::chrono::minutes(10 + i * 20));
        fresh_sig_files.push_back(p);
    }
    const auto expired_1 = sig_dir / "expired_8d.json";
    const auto expired_2 = sig_dir / "expired_14d.json";
    write_sized_file_with_age(expired_1, 100, std::chrono::hours(24 * 8));
    write_sized_file_with_age(expired_2, 100, std::chrono::hours(24 * 14));

    // Unmanaged subdirectories (challenge-solver, unknown) must remain untouched regardless of age.
    const auto solver_core = solver_dir / "core.json";
    const auto custom_old = unknown_dir / "legacy.json";
    write_sized_file_with_age(solver_core, 200, std::chrono::hours(24 * 30));
    write_sized_file_with_age(custom_old, 200, std::chrono::hours(24 * 30));

    // max_age == 0 disables pruning completely.
    std::unordered_set<std::string> unknown_logged;
    TEST_CHECK(StreamResolver::PruneYtDlpCacheDir(base_dir.string(), std::chrono::seconds(0)) == 0);
    TEST_CHECK(std::filesystem::exists(expired_1));
    TEST_CHECK(std::filesystem::exists(expired_2));

    // Prune with 7-day max age and large cap: removes the 2 expired sigfunc files, keeps all 6 fresh + unmanaged subdirs.
    const std::size_t pruned_age = StreamResolver::PruneYtDlpCacheDir(
        base_dir.string(),
        std::chrono::hours(24 * 7),
        StreamResolver::kYtDlpCacheMaxTotalBytes,
        std::chrono::hours(1),
        &unknown_logged);
    TEST_CHECK(pruned_age == 2);
    TEST_CHECK(!std::filesystem::exists(expired_1));
    TEST_CHECK(!std::filesystem::exists(expired_2));
    for (const auto& p : fresh_sig_files) {
        TEST_CHECK(std::filesystem::exists(p));
    }
    TEST_CHECK(std::filesystem::exists(solver_core));
    TEST_CHECK(std::filesystem::exists(custom_old));
    TEST_CHECK(unknown_logged.count("challenge-solver") == 1);
    TEST_CHECK(unknown_logged.count("custom-unknown") == 1);

    // Size-cap safety net test: add 3 files in youtube-sigfuncs (5h, 3h, 20m) of 500 bytes each ->
    // cap=800 bytes must remove oldest >= 1h files (5h and 3h, plus any older fresh_* >= 1h) while protecting < 1h (20m).
    const std::filesystem::path cap_dir = base_dir / "cap_test";
    const std::filesystem::path cap_sig = cap_dir / "youtube-sigfuncs";
    TEST_CHECK(std::filesystem::create_directories(cap_sig, ec) && !ec);
    const auto cap_old_5h = cap_sig / "a_5h.json";
    const auto cap_old_3h = cap_sig / "b_3h.json";
    const auto cap_young_20m = cap_sig / "c_20m.json";
    write_sized_file_with_age(cap_old_5h, 500, std::chrono::hours(5));
    write_sized_file_with_age(cap_old_3h, 500, std::chrono::hours(3));
    write_sized_file_with_age(cap_young_20m, 500, std::chrono::minutes(20));

    // Total 1500 bytes > 800 cap -> removes 5h then 3h (now 500 <= 800), leaving 20m protected.
    const std::size_t pruned_cap = StreamResolver::PruneYtDlpCacheDir(
        cap_dir.string(),
        std::chrono::hours(24 * 7),
        800,
        std::chrono::hours(1),
        nullptr);
    TEST_CHECK(pruned_cap == 2);
    TEST_CHECK(!std::filesystem::exists(cap_old_5h));
    TEST_CHECK(!std::filesystem::exists(cap_old_3h));
    TEST_CHECK(std::filesystem::exists(cap_young_20m));

    // Unmanaged folders must NOT inflate the cap counter against youtube-sigfuncs:
    // unmanaged subdir (2000 bytes > 800 cap) + youtube-sigfuncs (2h old, 200 bytes <= 800 cap) -> NOTHING deleted!
    const std::filesystem::path iso_dir = base_dir / "cap_isolation";
    const std::filesystem::path iso_sig = iso_dir / "youtube-sigfuncs";
    const std::filesystem::path iso_unmanaged = iso_dir / "challenge-solver";
    TEST_CHECK(std::filesystem::create_directories(iso_sig, ec) && !ec);
    TEST_CHECK(std::filesystem::create_directories(iso_unmanaged, ec) && !ec);
    const auto iso_sig_2h = iso_sig / "valid_2h.json";
    const auto iso_big_unmanaged = iso_unmanaged / "big_solver.json";
    write_sized_file_with_age(iso_sig_2h, 200, std::chrono::hours(2));
    write_sized_file_with_age(iso_big_unmanaged, 2000, std::chrono::hours(5));
    const std::size_t pruned_iso = StreamResolver::PruneYtDlpCacheDir(
        iso_dir.string(),
        std::chrono::hours(24 * 7),
        800,
        std::chrono::hours(1),
        nullptr);
    TEST_CHECK(pruned_iso == 0);
    TEST_CHECK(std::filesystem::exists(iso_sig_2h));
    TEST_CHECK(std::filesystem::exists(iso_big_unmanaged));

    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[PASS] TestV200YtDlpCachePruning\n";
}

static void TestV200EncodingPassthroughVolumeAndChannelOps() {
    // 1. Problem 1: bidirectional CP1252 <-> UTF-8 conversion + undefined 5-byte handling + ICY normalization
    const std::string cp1252_heart = "cora\xE7\xE3o";
    const std::string utf8_heart = "cora\xC3\xA7\xC3\xA3o";
    TEST_CHECK(ConvertTextFromPawn(cp1252_heart, "cp1252") == utf8_heart);
    TEST_CHECK(ConvertTextFromPawn(utf8_heart, "utf8") == utf8_heart);
    TEST_CHECK(ConvertTextForPawn(utf8_heart, "cp1252") == cp1252_heart);

    // Undefined CP1252 bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) are stripped cleanly; all other 251 bytes round-trip 1:1.
    auto is_undef_cp1252 = [](unsigned int b) {
        return b == 0x81u || b == 0x8Du || b == 0x8Fu || b == 0x90u || b == 0x9Du;
    };
    for (unsigned int b = 0; b <= 0xFFu; ++b) {
        if (b == 0) continue; // NUL is not part of Pawn AMX C-strings
        std::string single(1, static_cast<char>(static_cast<unsigned char>(b)));
        std::string utf8 = ConvertTextFromPawn(single, "cp1252");
        if (is_undef_cp1252(b)) {
            TEST_CHECK(utf8.empty());
        } else {
            TEST_CHECK(ConvertTextForPawn(utf8, "cp1252") == single);
        }
    }

    // ICY StreamTitle normalization: valid UTF-8 unchanged; invalid UTF-8 treated as CP1252 -> UTF-8
    const std::string icy_cp1252 = "R\xE1" "dio Cora\xE7\xE3o";
    const std::string icy_utf8 = "R\xC3\xA1" "dio Cora\xC3\xA7\xC3\xA3o";
    TEST_CHECK(NormalizeExternalTextToUtf8(icy_cp1252) == icy_utf8);
    TEST_CHECK(NormalizeExternalTextToUtf8(icy_utf8) == icy_utf8);

    // 2. Engine integration for Problems 1, 2, 3, 5, 6
    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "Walter"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.pawn_text_encoding = "cp1252";
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // Problem 1 echo: PlayAudioStreamForPlayer with CP1252 pawn_raw echoes verbatim to OnAudioStreamProcessing
    StreamOptions popts;
    popts.notify_processing = true;
    StreamId psid = engine.PlayAudioStreamForPlayer(1, utf8_heart, popts, cp1252_heart);
    TEST_CHECK(psid != INVALID_STREAM_ID);
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.processing_events.empty());
        TEST_CHECK(host.processing_events.back().stream_id == psid);
        TEST_CHECK(host.processing_events.back().url == cp1252_heart);
    }
    engine.StopAudioStreamForPlayer(1);

    // Problem 2: passthrough stream rejects SetPlayerAudioVolume and preserves direct URL across reconcile
    StreamOptions direct_opts;
    direct_opts.notify_processing = false;
    direct_opts.volume = 100.0f;
    StreamId direct_sid = engine.PlayAudioStreamForPlayer(1, "https://cdn.example.com/radio.mp3", direct_opts);
    TEST_CHECK(direct_sid != INVALID_STREAM_ID);
    TEST_CHECK(!engine.SetPlayerAudioVolume(1, 50.0f));
    TEST_CHECK(!engine.SetStreamVolume(direct_sid, 50.0f));
    TEST_CHECK(engine.PauseAudioStreamForPlayer(1, true));
    TEST_CHECK(engine.PauseAudioStreamForPlayer(1, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        std::string last_url;
        for (const auto& ev : host.events) {
            if (ev.play && ev.player_id == 1) last_url = ev.url;
        }
        TEST_CHECK(last_url == "https://cdn.example.com/radio.mp3");
    }
    engine.StopAudioStreamForPlayer(1);

    // Problem 3 & 1 queue echo: QueueChannel on idle starts playback; SkipChannel on empty queue stops playback
    ChannelId ch = engine.CreateChannel("M\xC3\xBAsica Mix", 1); // UTF-8 stored (converted from Pawn on boundary)
    TEST_CHECK(ch != INVALID_CHANNEL_ID);
    ChannelInfo cinfo{};
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo));
    TEST_CHECK(cinfo.name == "M\xFAsica Mix"); // converted back to CP1252 on output

    TEST_CHECK(engine.JoinChannel(1, ch));
    TEST_CHECK(engine.QueueChannel(ch, "https://example.com/track1.mp3", "https://example.com/track1.mp3"));
    TEST_CHECK(engine.GetChannelState(ch) == StreamState::Resolving); // idle channel started immediately!
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo) && cinfo.queue_count == 0);

    // Second queue item sits in queue and preserves Pawn raw encoding via GetChannelQueueItem
    const std::string pawn_q = "m\xFAsica rom\xE2ntica";
    TEST_CHECK(engine.QueueChannel(ch, ConvertTextFromPawn(pawn_q, "cp1252"), pawn_q));
    std::string fetched_q;
    TEST_CHECK(engine.GetChannelQueueItem(ch, 0, fetched_q) && fetched_q == pawn_q);

    // Skip advances to queued item
    TEST_CHECK(engine.SkipChannel(ch));
    TEST_CHECK(engine.GetChannelState(ch) == StreamState::Resolving);
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo) && cinfo.queue_count == 0);

    // Skip with empty queue stops active playback, preserves listener subscription, and returns true
    TEST_CHECK(engine.SkipChannel(ch));
    TEST_CHECK(engine.GetChannelState(ch) == StreamState::Stopped);
    TEST_CHECK(engine.IsPlayerInChannel(1, ch));
    // Skip when nothing is playing returns false
    TEST_CHECK(!engine.SkipChannel(ch));

    // Problem 5: Reset notifies remaining loaded scripts immediately with OnAudioStreamChannelDestroyed (without MainThreadDispatcher)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.destroyed_channels.clear();
    }
    engine.Reset();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.destroyed_channels.size() == 1);
        TEST_CHECK(host.destroyed_channels[0].channel_id == ch);
        TEST_CHECK(host.destroyed_channels[0].reason == static_cast<std::int32_t>(ChannelDestroyReason::Manual));
    }

    // Round 31 (Ajuste 2): SearchAudio failure delivers error via OnAudioStreamSearchResults(..., 0, err_code, err_msg) with zero OnAudioStreamError
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.error_events.clear();
        host.search_results_events.clear();
        host.callback_names.clear();
    }
    SearchId search_id = engine.SearchAudio(1, "coração", 5);
    TEST_CHECK(search_id != INVALID_SEARCH_ID);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool saw_search_done = false;
    while (std::chrono::steady_clock::now() < deadline) {
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.search_results_events.empty()) {
                saw_search_done = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_CHECK(saw_search_done);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.error_events.empty());
        TEST_CHECK(std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamError") == host.callback_names.end());
        TEST_CHECK(host.search_results_events.size() == 1);
        const auto& ev = host.search_results_events[0];
        TEST_CHECK(ev.player_id == 1);
        TEST_CHECK(ev.search_id == search_id);
        TEST_CHECK(ev.total_found == 0);
        TEST_CHECK(ev.error_code == static_cast<std::int32_t>(ErrorCode::ExtractorNotReady));
        TEST_CHECK(!ev.error_message.empty());
    }

    engine.Shutdown();
    std::cout << "[PASS] TestV200EncodingPassthroughVolumeAndChannelOps\n";
}

static void TestRound31SkipChannelConcurrencyPreservesQueue() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    std::atomic<bool> stop_srv{false};
    std::atomic<bool> saw_blocker_get{false};
    std::atomic<bool> release_blocker{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    auto make_wav_header = [](std::uint32_t data_sz) {
        std::string hdr(44, '\0');
        std::memcpy(hdr.data(), "RIFF", 4);
        std::uint32_t riff_sz = 36u + data_sz;
        std::memcpy(hdr.data() + 4, &riff_sz, 4);
        std::memcpy(hdr.data() + 8, "WAVEfmt ", 8);
        std::uint32_t fmt_sz = 16;
        std::uint16_t audio_fmt = 1, chans = 1;
        std::uint32_t rate = 16000, byte_rate = 32000;
        std::uint16_t align = 2, bps = 16;
        std::memcpy(hdr.data() + 16, &fmt_sz, 4);
        std::memcpy(hdr.data() + 20, &audio_fmt, 2);
        std::memcpy(hdr.data() + 22, &chans, 2);
        std::memcpy(hdr.data() + 24, &rate, 4);
        std::memcpy(hdr.data() + 28, &byte_rate, 4);
        std::memcpy(hdr.data() + 32, &align, 2);
        std::memcpy(hdr.data() + 34, &bps, 2);
        std::memcpy(hdr.data() + 36, "data", 4);
        std::memcpy(hdr.data() + 40, &data_sz, 4);
        return hdr;
    };

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, port, &stop_srv, &saw_blocker_get, &release_blocker, &make_wav_header]() {
                asio::error_code rec;
                asio::streambuf sbuf;
                asio::read_until(*sock, sbuf, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sbuf);
                std::string method, path;
                is >> method >> path;

                auto write_resp = [&](const std::string& ctype, const std::string& body) {
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: " + ctype + "\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\n"
                        "Connection: close\r\n\r\n" +
                        (method == "HEAD" ? std::string() : body);
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                };

                if (path.find("/bypass.m3u") != std::string::npos) {
                    write_resp("audio/x-mpegurl", "#EXTM3U\nhttp://127.0.0.1:" + std::to_string(port) + "/blocker.m3u\n");
                } else if (path.find("/blocker.m3u") != std::string::npos) {
                    write_resp("audio/x-mpegurl", "#EXTM3U\nhttp://127.0.0.1:" + std::to_string(port) + "/blocker.m4a\n");
                } else if (path.find("/blocker.m4a") != std::string::npos) {
                    if (method == "HEAD") {
                        std::string resp =
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: audio/mp4\r\n"
                            "Content-Length: 32000\r\n"
                            "Connection: close\r\n\r\n";
                        asio::error_code wec;
                        asio::write(*sock, asio::buffer(resp), wec);
                    } else {
                        saw_blocker_get.store(true);
                        while (!release_blocker.load() && !stop_srv.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(15));
                        }
                        write_resp("audio/mp4", "");
                    }
                } else if (path.find("/ch1.wav") != std::string::npos || path.find("/queued_next.wav") != std::string::npos) {
                    std::uint32_t total_pcm = 320000u;
                    std::string hdr =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: audio/wav\r\n"
                        "Content-Length: " + std::to_string(44u + total_pcm) + "\r\n"
                        "Connection: close\r\n\r\n";
                    if (method != "HEAD") hdr += make_wav_header(total_pcm);
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(hdr), wec);
                    if (method != "HEAD" && !wec) {
                        std::string chunk(1600, '\0');
                        for (int i = 0; i < 200 && !stop_srv.load(); ++i) {
                            asio::write(*sock, asio::buffer(chunk), wec);
                            if (wec) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        }
                    }
                } else {
                    write_resp("audio/mpeg", "");
                }
            });
        }
    });

    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}, {2, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "Walter"}, {2, "Blocker"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.max_concurrent_streams = 1;
    cfg.max_concurrent_transcoders = 2;
    cfg.pawn_text_encoding = "cp1252";
    cfg.persist_channels = false;

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    ChannelId ch = engine.CreateChannel("SkipLimitTest", 1);
    TEST_CHECK(ch != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.JoinChannel(1, ch));
    TEST_CHECK(engine.PlayChannel(ch, base + "/ch1.wav"));

    const std::string queued_pawn_url = base + "/queued_next.wav?q=cora\xE7\xE3o";
    const std::string queued_utf8_url = ConvertTextFromPawn(queued_pawn_url, "cp1252");
    TEST_CHECK(engine.QueueChannel(ch, queued_utf8_url, queued_pawn_url));

    ChannelInfo cinfo{};
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo) && cinfo.queue_count == 1);
    std::string q_item;
    TEST_CHECK(engine.GetChannelQueueItem(ch, 0, q_item) && q_item == queued_pawn_url);

    // Start an active transcoded stream via .m3u -> .m4a redirect (world_id=0 avoids sync shortcut,
    // pre_passthrough allows initial resolve, then .m4a forces is_passthrough=false)
    StreamOptions bopts;
    bopts.notify_processing = false;
    bopts.volume = 100.0f;
    bopts.world_id = 0;
    bopts.process_direct_audio_files = false;
    StreamId blocker = engine.PlayAudioStreamForPlayer(2, base + "/bypass.m3u", bopts);
    TEST_CHECK(blocker != INVALID_STREAM_ID);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !saw_blocker_get.load()) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_CHECK(saw_blocker_get.load());
    PumpEngineTicks(engine, 3);

    // Concurrency limit is now held by `blocker` -> SkipChannel MUST fail AND preserve queue.front().utf8_url intact!
    TEST_CHECK(!engine.SkipChannel(ch));
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo) && cinfo.queue_count == 1);
    q_item.clear();
    TEST_CHECK(engine.GetChannelQueueItem(ch, 0, q_item) && q_item == queued_pawn_url);

    // Stop and release the blocking stream -> SkipChannel now succeeds and plays the preserved queued track
    TEST_CHECK(engine.StopAudioStreamForPlayer(2));
    release_blocker.store(true);
    PumpEngineTicks(engine, 3);

    TEST_CHECK(engine.SkipChannel(ch));
    TEST_CHECK(engine.GetChannelInfo(ch, cinfo) && cinfo.queue_count == 0);
    TEST_CHECK(!engine.GetChannelQueueItem(ch, 0, q_item));
    TEST_CHECK(engine.GetChannelState(ch) != StreamState::Stopped);

    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> wlk(workers_mtx);
            to_join.swap(workers);
        }
        for (auto& wt : to_join) {
            if (wt.joinable()) wt.join();
        }
    }
    engine.Shutdown();
    std::cout << "[PASS] TestRound31SkipChannelConcurrencyPreservesQueue\n";
}

static std::string BuildTestWavBytes(std::uint32_t sample_rate, std::uint32_t duration_ms) {
    std::uint32_t num_samples = (sample_rate * duration_ms) / 1000u;
    std::uint32_t data_sz = num_samples * 2u;
    std::string wav(44u + data_sz, '\0');
    auto w32 = [&](std::size_t off, std::uint32_t v) {
        std::memcpy(wav.data() + off, &v, 4);
    };
    auto w16 = [&](std::size_t off, std::uint16_t v) {
        std::memcpy(wav.data() + off, &v, 2);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    w32(4, 36u + data_sz);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    w32(16, 16u);
    w16(20, 1u);
    w16(22, 1u);
    w32(24, sample_rate);
    w32(28, sample_rate * 2u);
    w16(32, 2u);
    w16(34, 16u);
    std::memcpy(wav.data() + 36, "data", 4);
    w32(40, data_sz);
    return wav;
}

static void TestRound32CallbacksLimitsAndOptionals() {
    const std::string short_wav = BuildTestWavBytes(16000, 180);
    const std::string long_wav = BuildTestWavBytes(16000, 3500);

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &short_wav, &long_wav]() {
                asio::error_code rec;
                asio::streambuf sbuf;
                asio::read_until(*sock, sbuf, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sbuf);
                std::string method, path;
                is >> method >> path;
                if (path.find("/bad") != std::string::npos) {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                    return;
                }
                const std::string& payload = (path.find("/long") != std::string::npos) ? long_wav : short_wav;
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += payload;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {3, {0.0f, 0.0f, 0.0f}, 0, 0},
        {8, {0.0f, 0.0f, 0.0f}, 0, 0}
    };
    host.player_names = {{1, "Alice"}, {3, "Bob"}, {8, "Carol"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.transcoder_burst_ms = 2000;
    cfg.max_channels = 2;
    cfg.max_queue_per_channel = 2;
    cfg.pawn_text_encoding = "cp1252";

    AudioEngine engine(host, cfg);
    TEST_PHASE("Initialize");
    TEST_CHECK(engine.Initialize());

    // Part 2: max_channels limit (2 allowed, 3rd refused) & max_queue_per_channel limit (idle play doesn't count)
    TEST_PHASE("limits & listeners");
    ChannelId ch1 = engine.CreateChannel("Club One");
    ChannelId ch2 = engine.CreateChannel("Club Two");
    TEST_CHECK(ch1 != INVALID_CHANNEL_ID && ch2 != INVALID_CHANNEL_ID);
    TEST_CHECK(engine.CreateChannel("Club Three") == INVALID_CHANNEL_ID);

    // Part 5: Listeners ascending order (insert out of order: 8, 1, 3 -> expect 1, 3, 8)
    TEST_CHECK(engine.JoinChannel(8, ch1));
    TEST_CHECK(engine.JoinChannel(1, ch1));
    TEST_CHECK(engine.JoinChannel(3, ch1));
    TEST_CHECK(engine.GetChannelListenerAt(ch1, 0) == 1);
    TEST_CHECK(engine.GetChannelListenerAt(ch1, 1) == 3);
    TEST_CHECK(engine.GetChannelListenerAt(ch1, 2) == 8);
    TEST_CHECK(engine.GetChannelListenerAt(ch1, 3) == INVALID_PLAYER_ID);

    // Part 1: Play -> TrackStart fires with original pawn url; Queue limit allows 2 queued items, refuses 3rd
    TEST_PHASE("track transitions & pause");
    TEST_CHECK(engine.GetChannelElapsedSeconds(ch1) == -1);
    const std::string pawn_play_url = base + "/short.wav?t=a\xE9";
    TEST_CHECK(engine.PlayChannel(ch1, ConvertTextFromPawn(pawn_play_url, "cp1252"), pawn_play_url));
    TEST_CHECK(engine.QueueChannel(ch1, base + "/long.wav", base + "/long.wav"));
    TEST_CHECK(engine.QueueChannel(ch1, base + "/short2.wav", base + "/short2.wav"));
    TEST_CHECK(!engine.QueueChannel(ch1, base + "/overflow.wav", base + "/overflow.wav"));
    TEST_CHECK(!engine.InsertChannelQueueItem(ch1, 0, base + "/overflow.wav", base + "/overflow.wav"));

    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (!host.channel_track_starts.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.channel_track_starts.empty());
        TEST_CHECK(host.channel_track_starts[0].channel_id == ch1);
        TEST_CHECK(host.channel_track_starts[0].url == pawn_play_url);
        TEST_CHECK(!host.channel_track_starts[0].title.empty());
        TEST_CHECK(host.error_events.empty());
    }

    // Wait for short.wav -> Queue advances -> long.wav TrackStart -> test Elapsed, Pause freeze, Rename & Stop (no QueueEnd!)
    dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (host.channel_track_starts.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.channel_track_starts.size() >= 2);
        TEST_CHECK(host.channel_track_starts[1].url == base + "/long.wav");
        TEST_CHECK(host.queue_ended_channels.empty());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::int32_t elapsed_run = engine.GetChannelElapsedSeconds(ch1);
    TEST_CHECK(elapsed_run >= 1);
    TEST_CHECK(engine.PauseChannel(ch1, true));
    std::int32_t elapsed_paused_start = engine.GetChannelElapsedSeconds(ch1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::int32_t elapsed_paused_later = engine.GetChannelElapsedSeconds(ch1);
    TEST_CHECK(elapsed_paused_later == elapsed_paused_start);
    TEST_CHECK(engine.PauseChannel(ch1, false));

    // Part 5: SetChannelName validates non-empty & updates GetChannelInfo
    TEST_CHECK(!engine.SetChannelName(ch1, ""));
    TEST_CHECK(engine.SetChannelName(ch1, "Renamed Club"));
    ChannelInfo cinfo{};
    TEST_CHECK(engine.GetChannelInfo(ch1, cinfo) && cinfo.name == "Renamed Club");

    // Stop MUST NOT trigger QueueEnd
    TEST_PHASE("stop without QueueEnd");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.queue_ended_channels.clear();
    }
    TEST_CHECK(engine.StopChannel(ch1));
    TEST_CHECK(engine.GetChannelElapsedSeconds(ch1) == -1);
    PumpEngineTicks(engine, 5);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.queue_ended_channels.empty());
    }

    // Natural finish without queue triggers QueueEnd
    TEST_PHASE("natural QueueEnd");
    TEST_CHECK(engine.PlayChannel(ch1, base + "/short.wav"));
    dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (!host.queue_ended_channels.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.queue_ended_channels.size() == 1 && host.queue_ended_channels[0] == ch1);
    }

    // Bad URL error -> OnAudioStreamChannelError only (zero OnAudioStreamError)
    TEST_PHASE("bad URL error");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.channel_errors.clear();
        host.error_events.clear();
    }
    TEST_CHECK(engine.PlayChannel(ch2, "http://127.0.0.1:" + std::to_string(port) + "/bad"));
    dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (!host.channel_errors.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.error_events.empty());
        TEST_CHECK(host.channel_errors.size() == 1 && host.channel_errors[0].channel_id == ch2);
    }

    TEST_PHASE("teardown");
    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    engine.Shutdown();
    TEST_CHECK(!host.spatial_query_off_main_thread.load());
    std::cout << "[PASS] TestRound32CallbacksLimitsAndOptionals\n";
}

static void TestRound32PositionalChannelsQueueManipulationAndLoop() {
    const std::string track_a = BuildTestWavBytes(16000, 120);
    const std::string track_b = BuildTestWavBytes(16000, 120);
    const std::string track_c = BuildTestWavBytes(16000, 120);

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &track_a, &track_b, &track_c]() {
                asio::error_code rec;
                asio::streambuf sbuf;
                asio::read_until(*sock, sbuf, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sbuf);
                std::string method, path;
                is >> method >> path;
                if (path.find("/bad") != std::string::npos) {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                    return;
                }
                const std::string& payload = (path.find("/b.wav") != std::string::npos) ? track_b :
                                             ((path.find("/c.wav") != std::string::npos) ? track_c : track_a);
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += payload;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},     // Inside initial club pos
        {2, {200.0f, 0.0f, 0.0f}, 0, 0}    // Outside initial club pos
    };
    host.player_names = {{1, "Clubber"}, {2, "DriverFan"}};
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.vehicles[55] = {200.0f, 0.0f, 0.0f};
    }

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.auto_download_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.spatial_update_interval_ms = 20;
    cfg.transcoder_burst_ms = 2000;

    TEST_PHASE("Initialize");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // --- Part 3: Positional channel + auto_listen + Clear + Vehicle attach ---
    TEST_PHASE("wait first play");
    ChannelId ch_pos = engine.CreateChannel("Neon Club");
    TEST_CHECK(ch_pos != INVALID_CHANNEL_ID);
    TEST_CHECK(!engine.SetChannelPos(ch_pos, 0.0f, 0.0f, 0.0f, 0.0f)); // distance <= 0 rejected
    TEST_CHECK(engine.SetChannelPos(ch_pos, 0.0f, 0.0f, 0.0f, 40.0f, -1, -1, true));
    // Hold playback open using Queue loop so tracks stay playing continuously during spatial checks
    TEST_CHECK(engine.SetChannelLoopMode(ch_pos, ChannelLoopMode::Track));
    TEST_CHECK(engine.GetChannelLoopMode(ch_pos) == ChannelLoopMode::Track);
    TEST_CHECK(engine.PlayChannel(ch_pos, base + "/a.wav"));

    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        if (!host.events.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.size() == 1);
        TEST_CHECK(host.events[0].player_id == 1 && host.events[0].play && host.events[0].use_pos);
        host.events.clear();
    }

    // Player 1 leaves range -> receives Stop
    TEST_PHASE("leave radius");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players[0].pos = {150.0f, 0.0f, 0.0f};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && !host.events.back().play && host.events.back().player_id == 1);
        host.events.clear();
    }

    // auto_listen = false -> unjoined player 1 back inside range does NOT hear; joined player 1 hears; GetChannelListenerAt excludes range-only
    TEST_PHASE("auto_listen=false");
    TEST_CHECK(engine.SetChannelPos(ch_pos, 0.0f, 0.0f, 0.0f, 40.0f, -1, -1, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players[0].pos = {5.0f, 0.0f, 0.0f};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.empty());
    }
    TEST_PHASE("JoinChannel");
    TEST_CHECK(engine.JoinChannel(1, ch_pos));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && host.events.back().player_id == 1 && host.events.back().play && host.events.back().use_pos);
        host.events.clear();
    }

    // Attach to vehicle 55 (x=200, near player 2, far from player 1) -> stops 1, enables auto_listen -> player 2 hears 2D attached (use_pos=false)!
    TEST_PHASE("AttachChannelToVehicle");
    TEST_CHECK(!engine.AttachChannelToVehicle(ch_pos, 999));
    TEST_CHECK(engine.SetChannelPos(ch_pos, 0.0f, 0.0f, 0.0f, 40.0f, -1, -1, true));
    TEST_CHECK(engine.AttachChannelToVehicle(ch_pos, 55));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p1_stopped = false, p2_started = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 1 && !ev.play) p1_stopped = true;
            if (ev.player_id == 2 && ev.play && !ev.use_pos) p2_started = true;
        }
        TEST_CHECK(p1_stopped && p2_started);
        host.events.clear();
    }
    // Remove vehicle 55 -> detaches cleanly leaving fixed pos (switches player 2 to use_pos=true); ClearChannelPos -> range-only 2 stops, joined 1 hears use_pos=false!
    TEST_PHASE("remove vehicle");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.vehicles.erase(55);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 3);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p2_fixed = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 2 && ev.play && ev.use_pos && std::abs(ev.pos.x - 200.0f) < 0.01f) p2_fixed = true;
        }
        TEST_CHECK(p2_fixed);
        host.events.clear();
    }
    TEST_PHASE("ClearChannelPos");
    TEST_CHECK(engine.ClearChannelPos(ch_pos));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p2_stopped = false, p1_global = false;
        for (const auto& ev : host.events) {
            if (ev.player_id == 2 && !ev.play) p2_stopped = true;
            if (ev.player_id == 1 && ev.play && !ev.use_pos) p1_global = true;
        }
        TEST_CHECK(p2_stopped && p1_global);
    }
    TEST_PHASE("StopChannel(ch_pos)");
    TEST_CHECK(engine.StopChannel(ch_pos));

    // --- Part 4: Queue Insert/Remove, Loop Track, Loop Queue, Error Skip & 5-Error Auto-Stop ---
    TEST_PHASE("queue operations");
    ChannelId ch_q = engine.CreateChannel("Loop Station");
    TEST_CHECK(engine.PlayChannel(ch_q, base + "/a.wav"));
    TEST_CHECK(engine.QueueChannel(ch_q, base + "/c.wav", base + "/c.wav"));
    TEST_CHECK(engine.InsertChannelQueueItem(ch_q, 0, base + "/b.wav", base + "/b.wav")); // [b, c]
    TEST_CHECK(engine.InsertChannelQueueItem(ch_q, 2, base + "/d.wav", base + "/d.wav")); // [b, c, d]
    TEST_CHECK(!engine.InsertChannelQueueItem(ch_q, 5, base + "/e.wav", base + "/e.wav"));
    TEST_CHECK(engine.RemoveChannelQueueItem(ch_q, 2)); // [b, c]
    TEST_CHECK(!engine.RemoveChannelQueueItem(ch_q, 2));
    std::string qi;
    TEST_CHECK(engine.GetChannelQueueItem(ch_q, 0, qi) && qi == base + "/b.wav");
    TEST_CHECK(engine.GetChannelQueueItem(ch_q, 1, qi) && qi == base + "/c.wav");

    // Track loop repeats A -> A -> A without consuming [b, c]
    TEST_PHASE("track loop");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.channel_track_starts.clear();
    }
    TEST_CHECK(engine.SetChannelLoopMode(ch_q, ChannelLoopMode::Track));
    dl = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        int count_a = 0;
        for (const auto& ts : host.channel_track_starts) {
            if (ts.channel_id == ch_q && ts.url == base + "/a.wav") ++count_a;
        }
        if (count_a >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        int count_a = 0;
        for (const auto& ts : host.channel_track_starts) {
            if (ts.channel_id == ch_q && ts.url == base + "/a.wav") ++count_a;
        }
        TEST_CHECK(count_a >= 2);
        host.channel_track_starts.clear();
    }
    {
        ChannelInfo qci{};
        TEST_CHECK(engine.GetChannelInfo(ch_q, qci) && qci.queue_count == 2); // b and c still intact!
    }

    // Queue loop cycles A -> B -> C -> A (with B, C remaining in queue!)
    TEST_PHASE("queue loop");
    TEST_CHECK(engine.SetChannelLoopMode(ch_q, ChannelLoopMode::Queue));
    dl = std::chrono::steady_clock::now() + std::chrono::seconds(22);
    bool cycled_abc_a = false;
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            std::vector<std::string> seq;
            for (const auto& ts : host.channel_track_starts) {
                if (ts.channel_id == ch_q) seq.push_back(ts.url);
            }
            for (std::size_t i = 0; i + 2 < seq.size(); ++i) {
                if (seq[i] == base + "/b.wav" && seq[i + 1] == base + "/c.wav" &&
                    seq[i + 2] == base + "/a.wav") {
                    cycled_abc_a = true;
                    break;
                }
            }
        }
        if (cycled_abc_a) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_CHECK(cycled_abc_a);
    TEST_CHECK(engine.GetChannelQueueItem(ch_q, 0, qi) && qi == base + "/b.wav");
    TEST_CHECK(engine.GetChannelQueueItem(ch_q, 1, qi) && qi == base + "/c.wav");
    TEST_PHASE("StopChannel(ch_q)");
    TEST_CHECK(engine.StopChannel(ch_q));

    // Error on Track loop does NOT repeat broken link; 5 consecutive errors stop the channel before 6th good track!
    TEST_PHASE("5-error chain");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.channel_errors.clear();
        host.channel_track_starts.clear();
        host.queue_ended_channels.clear();
    }
    TEST_CHECK(engine.SetChannelLoopMode(ch_q, ChannelLoopMode::Track));
    TEST_CHECK(engine.PlayChannel(ch_q, base + "/bad1"));
    for (int i = 2; i <= 5; ++i) {
        TEST_CHECK(engine.QueueChannel(ch_q, base + "/bad" + std::to_string(i), base + "/bad" + std::to_string(i)));
    }
    TEST_CHECK(engine.QueueChannel(ch_q, base + "/a.wav", base + "/a.wav")); // 6th good track must NOT run!

    dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < dl) {
        engine.Tick();
        if (engine.GetChannelState(ch_q) == StreamState::Stopped) {
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_errors.empty() &&
                host.channel_errors.back().error_message == "Too many consecutive track failures; channel stopped") {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    TEST_CHECK(engine.GetChannelState(ch_q) == StreamState::Stopped);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.channel_errors.empty());
        TEST_CHECK(host.channel_errors.back().error_code == static_cast<std::int32_t>(ErrorCode::InternalError));
        TEST_CHECK(host.channel_errors.back().error_message == "Too many consecutive track failures; channel stopped");
        TEST_CHECK(host.channel_track_starts.empty()); // Track /a.wav never started
        TEST_CHECK(host.queue_ended_channels.empty());
    }

    TEST_PHASE("teardown acceptor/server/workers/Shutdown");
    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    engine.Shutdown();
    TEST_CHECK(!host.spatial_query_off_main_thread.load());
    std::cout << "[PASS] TestRound32PositionalChannelsQueueManipulationAndLoop\n";
}

static void TestRound32ChannelPersistenceAndLifecycle() {
    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r32_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);

    // 1. Create full channel -> async debounce -> Shutdown sync flush -> reload into new engine
    ChannelId saved_id = INVALID_CHANNEL_ID;
    const std::string pawn_url_1 = "https://example.com/song1.mp3?q=cora\xE7\xE3o";
    const std::string pawn_url_2 = "https://example.com/song2.mp3?q=f\xE1";
    {
        MockPlatformHost host;
        host.players = {{1, {12.0f, 34.0f, 56.0f}, 2, 1}, {9, {99.0f, 88.0f, 77.0f}, 4, 3}};
        host.player_names = {{1, "Walter\xE9"}, {9, "Attached"}};

        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.base_dir = temp_base.string();
        cfg.pawn_text_encoding = "cp1252";

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());

        saved_id = engine.CreateChannel("Cora\xC3\xA7\xC3\xA3o Club", 1, "secret123", ChannelDestroyMode::Timeout, 4);
        TEST_CHECK(saved_id != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.SetChannelVolume(saved_id, 75.0f));
        TEST_CHECK(engine.SetChannelLoopMode(saved_id, ChannelLoopMode::Queue));
        TEST_CHECK(engine.AttachChannelToPlayer(saved_id, 9)); // Saved as fixed pos (99,88,77), player attach not saved
        TEST_CHECK(engine.PlayChannel(saved_id, ConvertTextFromPawn(pawn_url_1, "cp1252"), pawn_url_1));
        TEST_CHECK(engine.QueueChannel(saved_id, ConvertTextFromPawn(pawn_url_2, "cp1252"), pawn_url_2));

        // Pump until async debounce writes channel_<id>.json
        const std::filesystem::path ch_file = temp_base / "channels" / ("channel_" + std::to_string(saved_id) + ".json");
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < dl && !std::filesystem::exists(ch_file)) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        TEST_CHECK(std::filesystem::exists(ch_file));

        // Password must NEVER appear in plaintext on disk
        {
            std::ifstream ifs(ch_file);
            std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(raw.find("secret123") == std::string::npos);
            TEST_CHECK(raw.find("password_hash") != std::string::npos);
        }

        // Rename inside debounce window (< 1s), then Shutdown synchronously flushes without destroying!
        TEST_CHECK(engine.SetChannelName(saved_id, "After Shutdown Name"));
        engine.Shutdown();
        TEST_CHECK(std::filesystem::exists(ch_file));
    }

    // 2. Second engine loads persisted channel with same ID, hashed password, queue, creator offline + full Timeout countdown
    {
        MockPlatformHost host;
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.base_dir = temp_base.string();
        cfg.pawn_text_encoding = "cp1252";

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        engine.Tick(); // Delivers OnAudioStreamChannelRestored + sees extractor settled unready

        ChannelInfo info{};
        TEST_CHECK(engine.GetChannelInfo(saved_id, info));
        TEST_CHECK(info.name == "After Shutdown Name");
        TEST_CHECK(info.creator_name == "Walter\xE9");
        TEST_CHECK(!info.creator_online);
        TEST_CHECK(info.has_password);
        TEST_CHECK(info.destroy_mode == ChannelDestroyMode::Timeout);
        TEST_CHECK(info.destroy_timeout_seconds == 4);
        TEST_CHECK(info.destroy_remaining_seconds >= 3 && info.destroy_remaining_seconds <= 4);
        TEST_CHECK(engine.GetChannelLoopMode(saved_id) == ChannelLoopMode::Queue);
        TEST_CHECK(engine.VerifyChannelPassword(saved_id, "secret123"));
        TEST_CHECK(!engine.VerifyChannelPassword(saved_id, "wrongpass"));
        std::string q0;
        TEST_CHECK(engine.GetChannelQueueItem(saved_id, 0, q0) && q0 == pawn_url_2);
        // While extractor has not settled/ready yet, channel stays Idle with queue intact; once settled unready,
        // OnAudioStreamChannelError(SA_ERR_EXTRACTOR_NOT_READY) fires once without losing queued items
        TEST_CHECK(engine.GetChannelState(saved_id) == StreamState::Idle);
        auto err_dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < err_dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_errors.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.GetChannelState(saved_id) == StreamState::Idle);
        TEST_CHECK(engine.GetChannelQueueItem(saved_id, 0, q0) && q0 == pawn_url_2);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.restored_channels.size() == 1 && host.restored_channels[0].channel_id == saved_id);
            TEST_CHECK(host.channel_errors.size() == 1 &&
                       host.channel_errors[0].error_code == static_cast<std::int32_t>(ErrorCode::ExtractorNotReady));
        }

        // Creator reconnects matching name -> reclaims ownership & resets countdown
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.players.push_back({5, {0.0f, 0.0f, 0.0f}, 0, 0});
            host.player_names[5] = "walter\xE9";
        }
        engine.OnPlayerConnect(5);
        TEST_CHECK(engine.GetChannelInfo(saved_id, info) && info.creator_online && info.destroy_remaining_seconds == 4);

        // Next channel gets ID > saved_id (never collides)
        ChannelId ch_next = engine.CreateChannel("Extra Channel");
        TEST_CHECK(ch_next > saved_id);

        // DestroyChannel deletes file immediately and pending async flush does not recreate it
        TEST_CHECK(engine.SetChannelName(ch_next, "Dirty Right Before Destroy"));
        TEST_CHECK(engine.DestroyChannel(ch_next));
        TEST_CHECK(!std::filesystem::exists(temp_base / "channels" / ("channel_" + std::to_string(ch_next) + ".json")));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.Tick();
        TEST_CHECK(!std::filesystem::exists(temp_base / "channels" / ("channel_" + std::to_string(ch_next) + ".json")));

        // GMX Reset with persist_channels=true preserves live channels & fires OnAudioStreamChannelRestored
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.restored_channels.clear();
            host.destroyed_channels.clear();
        }
        engine.Reset();
        engine.Tick();
        TEST_CHECK(engine.GetChannelInfo(saved_id, info));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.destroyed_channels.empty());
            TEST_CHECK(host.restored_channels.size() == 1 && host.restored_channels[0].channel_id == saved_id);
        }

        engine.DestroyChannel(saved_id);
        engine.Shutdown();
    }

    // 3. OnDisconnect channel: protected during lifecycle_stopping_, and expires after restore grace on boot
    const std::filesystem::path grace_dir = temp_base / "grace_test";
    std::filesystem::remove_all(grace_dir, ec);
    ChannelId disc_id = INVALID_CHANNEL_ID;
    {
        MockPlatformHost host;
        host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
        host.player_names = {{1, "Owner"}};
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.base_dir = grace_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        disc_id = engine.CreateChannel("Grace Room", 1, "", ChannelDestroyMode::OnDisconnect);
        TEST_CHECK(disc_id != INVALID_CHANNEL_ID);
        engine.NotifyLifecycleStopping();
        engine.OnPlayerDisconnect(1); // Must NOT destroy while lifecycle_stopping_ is true
        ChannelInfo ci{};
        TEST_CHECK(engine.GetChannelInfo(disc_id, ci));
        engine.Shutdown();
        TEST_CHECK(std::filesystem::exists(grace_dir / "channels" / ("channel_" + std::to_string(disc_id) + ".json")));
    }
    {
        MockPlatformHost host;
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.channel_restore_grace_seconds = 1;
        cfg.base_dir = grace_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        ChannelInfo ci{};
        TEST_CHECK(engine.GetChannelInfo(disc_id, ci));
        std::this_thread::sleep_for(std::chrono::milliseconds(1150));
        engine.Tick(); // Grace expired -> DestroyChannelInternal + deletes file
        engine.Tick();
        TEST_CHECK(!engine.GetChannelInfo(disc_id, ci));
        TEST_CHECK(!std::filesystem::exists(grace_dir / "channels" / ("channel_" + std::to_string(disc_id) + ".json")));
        engine.Shutdown();
    }

    // 4. Corrupted JSON renamed to .bad, max_channels cap loads lowest IDs without deleting extras, persist_channels=false skips disk
    const std::filesystem::path bad_dir = temp_base / "bad_and_cap";
    std::filesystem::create_directories(bad_dir / "channels", ec);
    {
        std::ofstream bad_f(bad_dir / "channels" / "channel_99.json");
        bad_f << "{ not valid json";
    }
    for (int id = 1; id <= 3; ++id) {
        std::ofstream ok_f(bad_dir / "channels" / ("channel_" + std::to_string(id) + ".json"));
        ok_f << R"({"format_version":1,"id":)" << id << R"(,"name":"Ch)" << id << R"("})";
    }
    {
        MockPlatformHost host;
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.max_channels = 2;
        cfg.base_dir = bad_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        TEST_CHECK(std::filesystem::exists(bad_dir / "channels" / "channel_99.json.bad"));
        TEST_CHECK(engine.GetChannelCount() == 2);
        ChannelInfo ci{};
        TEST_CHECK(engine.GetChannelInfo(1, ci) && engine.GetChannelInfo(2, ci) && !engine.GetChannelInfo(3, ci));
        TEST_CHECK(std::filesystem::exists(bad_dir / "channels" / "channel_3.json")); // Untouched!
        engine.Shutdown();
    }
    {
        MockPlatformHost host;
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = false;
        cfg.base_dir = bad_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        TEST_CHECK(engine.GetChannelCount() == 0);
        ChannelId np = engine.CreateChannel("No Persist");
        TEST_CHECK(np == 1);
        engine.Shutdown();
        TEST_CHECK(!std::filesystem::exists(bad_dir / "channels" / "channel_10.json"));
    }

    std::filesystem::remove_all(temp_base, ec);
    std::cout << "[PASS] TestRound32ChannelPersistenceAndLifecycle\n";
}

static void TestRound33AuditedFixes() {
    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r33_fixes_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);

    // 1. Problem 4 (HTTP server station_name separation): icy-name uses station_name while StreamTitle stays track title
    {
        HttpStreamingServer srv;
        std::string bind_err;
        TEST_CHECK(srv.Start("127.0.0.1", 0, bind_err, false));

        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::vector<std::uint8_t> payload(24000, 0x55);
        buf->Write(payload.data(), payload.size());
        buf->MarkEof();

        StreamEndpointInfo ep;
        ep.token = "r33_station";
        ep.title = "Song Title";
        ep.artist = "Track Artist";
        ep.station_name = "Old Station";
        ep.bitrate_kbps = 128;
        ep.buffer = buf;
        srv.RegisterEndpoint(ep);

        srv.UpdateEndpointStationName("r33_station", "Renamed Station");

        asio::io_context ioc;
        asio::ip::tcp::socket sock(ioc);
        sock.connect({asio::ip::address_v4::loopback(), srv.BoundPort()});
        std::string req = "GET /stream/r33_station.mp3 HTTP/1.1\r\nHost: 127.0.0.1\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n";
        asio::write(sock, asio::buffer(req));
        std::string raw;
        std::array<char, 4096> tmp{};
        std::error_code rec;
        while (true) {
            std::size_t n = sock.read_some(asio::buffer(tmp), rec);
            if (n > 0) raw.append(tmp.data(), n);
            if (rec) break;
        }
        auto hdr_end = raw.find("\r\n\r\n");
        TEST_CHECK(hdr_end != std::string::npos);
        std::string hdrs = raw.substr(0, hdr_end);
        std::string body = raw.substr(hdr_end + 4);
        TEST_CHECK(hdrs.find("icy-name: Renamed Station") != std::string::npos);
        TEST_CHECK(hdrs.find("Song Title") == std::string::npos);
        TEST_CHECK(body.find("StreamTitle='Track Artist - Song Title';") != std::string::npos);
        TEST_CHECK(body.find("Renamed Station") == std::string::npos);
        srv.Stop();
    }

    // 2. Problems 1, 3, 4 (via AudioEngine) and 5:
    //    - Problem 1: attached sound-car channel -> use_pos == false, stays audible past initial radius; fixed pos -> use_pos == true
    //    - Problem 3: snapshot & disk JSON omit attach_vehicle_id; Reset() with persist_channels=true detaches vehicle & keeps fixed pos
    //    - Problem 4: SetChannelName during playback updates icy-name on endpoint without altering track metadata
    //    - Problem 5: GetChannelListenerAt returns listeners in ascending playerid order (7, 2, 5 -> 2, 5, 7)
    const std::string wav_track = BuildTestWavBytes(16000, 1200);
    asio::io_context wav_io;
    asio::ip::tcp::acceptor wav_acc(wav_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short wav_port = wav_acc.local_endpoint().port();
    const std::string wav_url = "http://127.0.0.1:" + std::to_string(wav_port) + "/road_song.wav";
    std::atomic<bool> stop_wav{false};
    std::mutex wav_workers_mtx;
    std::vector<std::thread> wav_workers;
    std::thread wav_srv([&]() {
        while (!stop_wav.load()) {
            asio::error_code aec;
            wav_acc.non_blocking(true, aec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(wav_io);
            wav_acc.accept(*sock, aec);
            if (aec == asio::error::would_block || aec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (aec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(wav_workers_mtx);
            wav_workers.emplace_back([sock, &wav_track]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sb);
                std::string method, path;
                is >> method >> path;
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(wav_track.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += wav_track;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    const std::filesystem::path p1345_dir = temp_base / "p1345";
    {
        MockPlatformHost host;
        host.players = {
            {7, {0.0f, 0.0f, 0.0f}, 0, 0},
            {2, {300.0f, 0.0f, 0.0f}, 0, 0},
            {5, {0.0f, 0.0f, 0.0f}, 0, 0}
        };
        host.player_names = {{7, "P7"}, {2, "P2"}, {5, "P5"}};
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.vehicles[77] = {0.0f, 0.0f, 0.0f};
        }

        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = true;
        cfg.base_dir = p1345_dir.string();
        cfg.spatial_update_interval_ms = 20;
        cfg.transcoder_burst_ms = 2000;

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());

        // Problem 5: join 7, 2, 5 -> GetChannelListenerAt yields 2, 5, 7
        ChannelId ch_list = engine.CreateChannel("Listener Order");
        TEST_CHECK(engine.JoinChannel(7, ch_list));
        TEST_CHECK(engine.JoinChannel(2, ch_list));
        TEST_CHECK(engine.JoinChannel(5, ch_list));
        TEST_CHECK(engine.GetChannelListenerAt(ch_list, 0) == 2);
        TEST_CHECK(engine.GetChannelListenerAt(ch_list, 1) == 5);
        TEST_CHECK(engine.GetChannelListenerAt(ch_list, 2) == 7);
        TEST_CHECK(engine.GetChannelListenerAt(ch_list, 3) == INVALID_PLAYER_ID);
        TEST_CHECK(engine.DestroyChannel(ch_list));

        // Attached sound car channel + TrackStart + Rename
        ChannelId ch_car = engine.CreateChannel("Old Car FM");
        TEST_CHECK(engine.SetChannelLoopMode(ch_car, ChannelLoopMode::Track));
        TEST_CHECK(engine.SetChannelPos(ch_car, 0.0f, 0.0f, 0.0f, 40.0f, -1, -1, true));
        TEST_CHECK(engine.AttachChannelToVehicle(ch_car, 77));
        TEST_CHECK(engine.PlayChannel(ch_car, wav_url));

        std::string play_url_p7;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_track_starts.empty() && host.events.size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::string initial_track_title;
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(!host.channel_track_starts.empty());
            initial_track_title = host.channel_track_starts.back().title;
            bool p7_2d = false;
            for (const auto& ev : host.events) {
                if (ev.player_id == 7 && ev.play && !ev.use_pos) {
                    p7_2d = true;
                    play_url_p7 = ev.url;
                }
            }
            TEST_CHECK(p7_2d);
            host.events.clear();
        }

        // Problem 4: rename while playing -> track metadata stays intact & HTTP header icy-name changes
        TEST_CHECK(engine.SetChannelName(ch_car, "New Car FM"));
        TrackMetadata unused_meta;
        ChannelInfo cinfo{};
        TEST_CHECK(engine.GetChannelInfo(ch_car, cinfo));
        TEST_CHECK(cinfo.name == "New Car FM");
        TEST_CHECK(cinfo.current_track_title == initial_track_title);

        if (!play_url_p7.empty()) {
            // Query HEAD from channel endpoint on HttpStreamingServer to inspect updated icy-name header
            auto pos = play_url_p7.find("/channel/");
            TEST_CHECK(pos != std::string::npos);
            std::string path_part = play_url_p7.substr(pos);
            asio::io_context hioc;
            asio::ip::tcp::socket hsock(hioc);
            hsock.connect({asio::ip::address_v4::loopback(), engine.GetHttpPort()});
            std::string hreq = "HEAD " + path_part + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
            asio::write(hsock, asio::buffer(hreq));
            std::string hraw;
            std::array<char, 2048> hbuf{};
            std::error_code hec;
            while (true) {
                std::size_t n = hsock.read_some(asio::buffer(hbuf), hec);
                if (n > 0) hraw.append(hbuf.data(), n);
                if (hec) break;
            }
            TEST_CHECK(hraw.find("icy-name: New Car FM") != std::string::npos);
        }

        // Move sound car + passenger 7 to x=300 (near player 2, >200m from initial origin) ->
        // player 7 keeps hearing without re-play/stop, player 5 (still at x=0) stops, player 2 (at x=300) starts with use_pos=false!
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.vehicles[77] = {300.0f, 0.0f, 0.0f};
            host.players[0].pos = {300.0f, 0.0f, 0.0f}; // player 7
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        PumpEngineTicks(engine, 4);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            bool p7_touched = false, p5_stopped = false, p2_started_2d = false;
            for (const auto& ev : host.events) {
                if (ev.player_id == 7) p7_touched = true;
                if (ev.player_id == 5 && !ev.play) p5_stopped = true;
                if (ev.player_id == 2 && ev.play && !ev.use_pos) p2_started_2d = true;
            }
            TEST_CHECK(!p7_touched);
            TEST_CHECK(p5_stopped && p2_started_2d);
            host.events.clear();
        }

        // Problem 3: GMX Reset with persist_channels=true detaches vehicle, bumps spatial_version, keeps fixed pos (use_pos=true)
        engine.Reset();
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            bool p7_fixed = false, p2_fixed = false;
            for (const auto& ev : host.events) {
                if (ev.player_id == 7 && ev.play && ev.use_pos && std::abs(ev.pos.x - 300.0f) < 0.01f) p7_fixed = true;
                if (ev.player_id == 2 && ev.play && ev.use_pos && std::abs(ev.pos.x - 300.0f) < 0.01f) p2_fixed = true;
            }
            TEST_CHECK(p7_fixed && p2_fixed);
            host.events.clear();
        }

        // Re-attach to vehicle before Shutdown and verify saved JSON does NOT contain attach_vehicle_id
        TEST_CHECK(engine.AttachChannelToVehicle(ch_car, 77));
        engine.Shutdown();

        const std::filesystem::path car_file = p1345_dir / "channels" / ("channel_" + std::to_string(ch_car) + ".json");
        TEST_CHECK(std::filesystem::exists(car_file));
        std::ifstream ifs(car_file);
        std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_CHECK(raw.find("attach_vehicle_id") == std::string::npos);
        TEST_CHECK(raw.find("\"use_position\": true") != std::string::npos);
    }

    stop_wav.store(true);
    asio::error_code wec;
    wav_acc.close(wec);
    if (wav_srv.joinable()) wav_srv.join();
    {
        std::lock_guard<std::mutex> wlk(wav_workers_mtx);
        for (auto& w : wav_workers) {
            if (w.joinable()) w.join();
        }
    }

    // 3. Problem 2: Shutdown flush must not lose writes when async FlushDirtyChannels set persist_dirty=false and persist_write_in_flight=true
    const std::filesystem::path p2_dir = temp_base / "p2_shutdown_inflight";
    {
        MockPlatformHost host;
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.base_dir = p2_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        std::vector<ChannelId> cids;
        for (int i = 0; i < 12; ++i) {
            ChannelId id = engine.CreateChannel("Initial_" + std::to_string(i));
            TEST_CHECK(id != INVALID_CHANNEL_ID);
            cids.push_back(id);
        }
        // Flush initial files and let 1s debounce expire
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        // Mark all 12 channels dirty, post async writes in ONE Tick (queuing them in control_pool_), then immediately Shutdown
        for (ChannelId id : cids) {
            TEST_CHECK(engine.SetChannelName(id, "FinalState_" + std::to_string(id)));
        }
        engine.Tick();
        engine.Shutdown();

        for (ChannelId id : cids) {
            const std::filesystem::path f = p2_dir / "channels" / ("channel_" + std::to_string(id) + ".json");
            TEST_CHECK(std::filesystem::exists(f));
            std::ifstream ifs(f);
            std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(raw.find("\"name\": \"FinalState_" + std::to_string(id) + "\"") != std::string::npos);
        }
    }

    // 4. Problem 6: Extractor failure on boot blocks retries for this run ONLY, preserving was_playing = true on disk
    //    AND Problem 3 backwards compatibility: legacy attach_vehicle_id field on disk is ignored cleanly.
    const std::filesystem::path p6_dir = temp_base / "p6_boot_fail";
    std::filesystem::create_directories(p6_dir / "channels", ec);
    {
        std::ofstream ofs(p6_dir / "channels" / "channel_1.json");
        ofs << R"({
  "format_version": 1,
  "id": 1,
  "name": "Boot Play Channel",
  "has_creator": false,
  "creator_name_hex": "536572766572",
  "destroy_mode": 0,
  "destroy_timeout_seconds": 0,
  "volume": 100.0,
  "paused": false,
  "loop_mode": 0,
  "current_input": {
    "utf8_url": "https://example.com/saved.mp3",
    "pawn_url_hex": "68747470733a2f2f6578616d706c652e636f6d2f73617665642e6d7033"
  },
  "was_playing": true,
  "queue": [],
  "spatial": {
    "use_position": true,
    "x": 10.0,
    "y": 20.0,
    "z": 30.0,
    "distance": 40.0,
    "world_id": -1,
    "interior_id": -1,
    "auto_listen": true,
    "attach_vehicle_id": 88
  }
})";
    }
    {
        MockPlatformHost host;
        host.players = {{1, {10.0f, 20.0f, 30.0f}, 0, 0}};
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.auto_download_extractor = false;
        cfg.persist_channels = true;
        cfg.base_dir = p6_dir.string();

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        auto edl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < edl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_errors.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_errors.size() == 1 &&
                       host.channel_errors[0].error_code == static_cast<std::int32_t>(ErrorCode::ExtractorNotReady));
        }
        // Extra ticks must NOT re-emit ExtractorNotReady error
        PumpEngineTicks(engine, 5);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_errors.size() == 1);
        }

        // Modify channel and shut down -> was_playing must STILL be true on disk, and legacy attach_vehicle_id ignored
        TEST_CHECK(engine.SetChannelName(1, "Still Wants Play"));
        engine.Shutdown();

        std::ifstream ifs(p6_dir / "channels" / "channel_1.json");
        std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_CHECK(raw.find("\"was_playing\": true") != std::string::npos);
        TEST_CHECK(raw.find("attach_vehicle_id") == std::string::npos);
    }

    std::filesystem::remove_all(temp_base, ec);
    std::cout << "[PASS] TestRound33AuditedFixes\n";
}

static void TestRound35FeaturesPart1To8() {
    const std::string short_wav = BuildTestWavBytes(16000, 3500);
    const std::string long_wav = BuildTestWavBytes(16000, 6000);

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &short_wav, &long_wav]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sb);
                std::string method, path;
                is >> method >> path;
                if (path.find("/live_radio") != std::string::npos) {
                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-metaint: 0\r\nConnection: close\r\n\r\n";
                    if (method != "HEAD") resp += std::string(1024, '\0');
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                    return;
                }
                const std::string& payload = (path.find("/long.wav") != std::string::npos) ? long_wav : short_wav;
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += payload;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r35_p1_p8_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(temp_base / "bin", ec);
    {
#ifdef _WIN32
        const std::filesystem::path ytdlp_bin = temp_base / "bin" / "yt-dlp.exe";
#else
        const std::filesystem::path ytdlp_bin = temp_base / "bin" / "yt-dlp";
#endif
        std::ofstream bf(ytdlp_bin, std::ios::binary);
        bf << std::string(128 * 1024, 'x');
    }

    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {5.0f, 0.0f, 0.0f}, 0, 0},
        {3, {200.0f, 0.0f, 0.0f}, 0, 0}
    };
    host.player_names = {{1, "Muter"}, {2, "Watcher"}, {3, "FarGuy"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.spatial_update_interval_ms = 20;
    cfg.transcoder_burst_ms = 2000;
    cfg.max_queue_per_channel = 50;

    TEST_PHASE("Initialize & wait extractor");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());

    // Wait for BinaryManager to settle with dummy binary -> IsExtractorReady() == true
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }
    TEST_PHASE("Part 2 & 1: SourceChange, Mute, URL & Pause");

    // Install yt-dlp test hook to validate playlist args and feed controlled JSON for Part 6 & Part 2
    std::atomic<bool> block_destroy_playlist{false};
    std::atomic<bool> destroy_playlist_entered{false};
    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string& out_stderr,
        int,
        const std::atomic<bool>* cancel_flag) -> bool {
        (void)out_stderr;
        bool has_flat = false, has_dump = false, has_yes = false, has_end = false;
        std::string target;
        for (const auto& a : args) {
            if (a == "--flat-playlist") has_flat = true;
            if (a == "--dump-single-json") has_dump = true;
            if (a == "--yes-playlist") has_yes = true;
            if (a == "--playlist-end") has_end = true;
            target = a;
        }
        if (target.find("destroy_mid") != std::string::npos) {
            destroy_playlist_entered.store(true);
            auto wdl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < wdl) {
                if (cancel_flag && cancel_flag->load()) return false;
                if (!block_destroy_playlist.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (cancel_flag && cancel_flag->load()) return false;
        }
        if (target.find("single_vid") != std::string::npos) {
            out_stdout = "{\"webpage_url\":\"" + base + "/track1.wav\"}";
            return true;
        }
        if (target.find("playlist30") != std::string::npos) {
            TEST_CHECK(has_flat && has_dump && has_yes && has_end);
            std::string entries;
            for (int i = 1; i <= 30; ++i) {
                if (i > 1) entries += ",";
                entries += "{\"url\":\"" + base + "/p" + std::to_string(i) + ".wav\"}";
            }
            out_stdout = "{\"entries\":[" + entries + "]}";
            return true;
        }
        out_stdout = "{\"webpage_url\":\"" + base + "/track1.wav\"}";
        return true;
    });

    // --- Part 2: OnPlayerAudioStreamSourceChange & SA_GetPlayerAudioStreamSource ---
    ChannelId ch_range = engine.CreateChannel("Club Range");
    TEST_CHECK(engine.SetChannelPos(ch_range, 0.0f, 0.0f, 0.0f, 30.0f, -1, -1, true));
    TEST_CHECK(engine.PlayChannel(ch_range, base + "/long.wav"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (host.source_changes.size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        bool p1_range = false;
        for (const auto& sc : host.source_changes) {
            if (sc.player_id == 1 &&
                sc.source_type == static_cast<std::int32_t>(AudioStreamSourceType::ChannelRange) &&
                sc.source_id == ch_range) {
                p1_range = true;
            }
        }
        TEST_CHECK(p1_range);
        host.source_changes.clear();
    }
    AudioStreamSourceType src_t = AudioStreamSourceType::None;
    std::int32_t src_id = -1;
    TEST_CHECK(engine.GetPlayerAudioStreamSource(1, src_t, src_id));
    TEST_CHECK(src_t == AudioStreamSourceType::ChannelRange && src_id == ch_range);

    // Move player 1 out of range -> callback NONE (-1), query returns false
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players[0].pos = {150.0f, 0.0f, 0.0f};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.source_changes.empty());
        TEST_CHECK(host.source_changes.back().player_id == 1);
        TEST_CHECK(host.source_changes.back().source_type == static_cast<std::int32_t>(AudioStreamSourceType::None));
        TEST_CHECK(host.source_changes.back().source_id == -1);
        host.source_changes.clear();
    }
    TEST_CHECK(!engine.GetPlayerAudioStreamSource(1, src_t, src_id));
    TEST_CHECK(src_t == AudioStreamSourceType::None && src_id == -1);

    // Move player 1 back inside range AND join another channel, then Skip track inside same channel -> NO source change callback on same channel track switch
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players[0].pos = {0.0f, 0.0f, 0.0f};
    }
    ChannelId ch_sub = engine.CreateChannel("Sub Radio");
    TEST_CHECK(engine.JoinChannel(1, ch_sub));
    TEST_CHECK(engine.PlayChannel(ch_sub, base + "/a.wav"));
    TEST_CHECK(engine.QueueChannel(ch_sub, base + "/b.wav", base + "/b.wav"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            AudioStreamSourceType qt = AudioStreamSourceType::None;
            std::int32_t qid = -1;
            bool got_cb = false;
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& sc : host.source_changes) {
                    if (sc.player_id == 1 &&
                        sc.source_type == static_cast<std::int32_t>(AudioStreamSourceType::Channel) &&
                        sc.source_id == ch_sub) {
                        got_cb = true;
                        break;
                    }
                }
            }
            if (got_cb &&
                engine.GetPlayerAudioStreamSource(1, qt, qid) &&
                qt == AudioStreamSourceType::Channel && qid == ch_sub) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        PumpEngineTicks(engine, 2);
        TEST_CHECK(engine.GetPlayerAudioStreamSource(1, src_t, src_id));
        TEST_CHECK(src_t == AudioStreamSourceType::Channel && src_id == ch_sub);
        std::lock_guard<std::mutex> lk(host.mtx);
        host.source_changes.clear();
    }
    TEST_CHECK(engine.SkipChannel(ch_sub));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            bool started_b = false;
            for (const auto& ts : host.channel_track_starts) {
                if (ts.channel_id == ch_sub && ts.url == base + "/b.wav") started_b = true;
            }
            if (started_b) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        PumpEngineTicks(engine, 2);
        std::lock_guard<std::mutex> lk(host.mtx);
        for (const auto& sc : host.source_changes) {
            TEST_CHECK(sc.player_id != 1); // Same channel track switch must NOT emit source change!
        }
        host.source_changes.clear();
        host.events.clear();
    }

    // --- Part 1: Mute (channel/range + personal false vs true + passthrough non-cancellation + GMX/disconnect persistence) ---
    TEST_CHECK(!engine.SetPlayerAudioStreamMuted(999, true)); // Disconnected -> 0
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, true, false));
    TEST_CHECK(engine.IsPlayerAudioStreamMuted(1));
    TEST_CHECK(engine.IsPlayerInChannel(1, ch_sub)); // Still joined!
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && !host.events.back().play && host.events.back().player_id == 1);
        TEST_CHECK(!host.source_changes.empty() &&
                   host.source_changes.back().source_type == static_cast<std::int32_t>(AudioStreamSourceType::None));
        host.events.clear();
        host.source_changes.clear();
    }
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, false));
    TEST_CHECK(!engine.IsPlayerAudioStreamMuted(1));
    engine.Tick();
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && host.events.back().play && host.events.back().player_id == 1);
        TEST_CHECK(!host.source_changes.empty() &&
                   host.source_changes.back().source_type == static_cast<std::int32_t>(AudioStreamSourceType::Channel) &&
                   host.source_changes.back().source_id == ch_sub);
        host.events.clear();
        host.source_changes.clear();
    }

    // Personal stream overrides Channel, survives mute(include_personal=false), stops/survives/resumes on mute(include_personal=true)
    const std::string pawn_enc_url = base + "/cora\xE7\xE3o.mp3";
    const std::string utf8_url = ConvertTextFromPawn(pawn_enc_url, "cp1252");
    StreamOptions popts;
    popts.notify_processing = false;
    popts.process_direct_audio_files = true; // Transcoded
    StreamId p_sid = engine.PlayAudioStreamForPlayer(1, utf8_url, popts, pawn_enc_url);
    TEST_CHECK(p_sid != INVALID_STREAM_ID);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            AudioStreamSourceType qt = AudioStreamSourceType::None;
            std::int32_t qid = -1;
            if (engine.GetPlayerAudioStreamSource(1, qt, qid) &&
                qt == AudioStreamSourceType::Personal && qid == p_sid) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.GetPlayerAudioStreamSource(1, src_t, src_id));
        TEST_CHECK(src_t == AudioStreamSourceType::Personal && src_id == p_sid);
    }

    // Part 7: GetStreamUrl returns exact raw Pawn bytes ("cora\xE7\xE3o")
    {
        std::string echoed;
        TEST_CHECK(engine.GetStreamUrl(p_sid, echoed));
        TEST_CHECK(echoed == pawn_enc_url);
        TEST_CHECK(!engine.GetStreamUrl(999999, echoed));
    }

    // Mute with include_personal=false -> Personal stream NOT stopped!
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, true, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.events.empty());
    }
    TEST_CHECK(engine.GetPlayerAudioStreamSource(1, src_t, src_id) && src_t == AudioStreamSourceType::Personal);

    // Mute with include_personal=true -> stops personal, does NOT resume yet; unmute -> plays again!
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, true, true));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && !host.events.back().play && host.events.back().player_id == 1);
        host.events.clear();
    }
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && host.events.back().play && host.events.back().player_id == 1);
        host.events.clear();
    }

    // Personal PASSTHROUGH muted with include_personal=true -> session stays alive, NOT cancelled!
    StreamOptions pass_opts;
    pass_opts.notify_processing = false;
    pass_opts.volume = 100.0f;
    StreamId pt_sid = engine.PlayAudioStreamForPlayer(2, base + "/song.mp3", pass_opts);
    TEST_CHECK(pt_sid != INVALID_STREAM_ID);
    PumpEngineTicks(engine, 2);
    TEST_CHECK(engine.GetPlayerStreamId(2) == pt_sid);
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(2, true, true));
    TEST_CHECK(engine.GetPlayerStreamId(2) == pt_sid); // NOT cancelled!
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(2, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.events.empty() && host.events.back().player_id == 2 && host.events.back().play &&
                   host.events.back().url == base + "/song.mp3");
        host.events.clear();
    }

    // Mute stays across GMX Reset, cleared on Disconnect
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, true, false));
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(3, true, true));
    engine.Reset();
    TEST_CHECK(engine.IsPlayerAudioStreamMuted(1));
    TEST_CHECK(engine.IsPlayerAudioStreamMuted(3));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.pop_back(); // Disconnect player 3
    }
    engine.OnPlayerDisconnect(3);
    TEST_CHECK(!engine.IsPlayerAudioStreamMuted(3));
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, false));
    PumpEngineTicks(engine, 2);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
        host.source_changes.clear();
    }

    // --- Parts 3, 4, 5, 8: Global PauseAudioStream (2 listeners, resume=), List/Owner, Elapsed, Stats ---
    StreamOptions gopts;
    gopts.notify_processing = false;
    gopts.use_position = true;
    gopts.distance = 30.0f;
    gopts.position = {0.0f, 0.0f, 0.0f};
    StreamId g_sid = engine.PlayAudioStream(base + "/long.wav", gopts);
    StreamOptions p1o;
    p1o.notify_processing = false;
    StreamId p1_b = engine.PlayAudioStreamForPlayer(1, base + "/long.wav", p1o);
    StreamId p2_b = engine.PlayAudioStreamForPlayer(2, base + "/song.mp3", pass_opts);
    TEST_CHECK(g_sid != INVALID_STREAM_ID && p1_b != INVALID_STREAM_ID && p2_b != INVALID_STREAM_ID);

    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            TrackMetadata mm; StreamState ms = StreamState::Idle;
            AudioEngineStats cur_st = engine.GetStats();
            if (engine.GetStreamInfo(g_sid, mm, ms) && ms == StreamState::Playing &&
                engine.GetStreamInfo(p1_b, mm, ms) && ms == StreamState::Playing &&
                cur_st.active_transcoders >= 1 &&
                cur_st.active_listeners == 2) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // Part 4: Count == 3, sorted IDs ascending, GetStreamOwner
    TEST_CHECK(engine.GetStreamCount() == 3);
    TEST_CHECK(engine.GetStreamIdAt(0) < engine.GetStreamIdAt(1));
    TEST_CHECK(engine.GetStreamIdAt(1) < engine.GetStreamIdAt(2));
    TEST_CHECK(engine.GetStreamIdAt(3) == INVALID_STREAM_ID);
    TEST_CHECK(engine.GetStreamOwner(p1_b) == 1);
    TEST_CHECK(engine.GetStreamOwner(p2_b) == 2);
    TEST_CHECK(engine.GetStreamOwner(g_sid) == INVALID_PLAYER_ID);

    // Part 8: Stats
    {
        // Make player 3 (at x=200) also listen to global? Move player 3 to (10,0,0), stop personal p1/p2 after checking
        AudioEngineStats st = engine.GetStats();
        TEST_CHECK(st.active_streams == 3);
        TEST_CHECK(st.active_channels == 0);
        TEST_CHECK(st.active_transcoders >= 1);
        TEST_CHECK(st.active_listeners == 2);
    }

    // Stop personal streams so players 1 and 2 (both near 0,0,0) listen to global stream g_sid
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    TEST_CHECK(engine.StopAudioStreamForPlayer(2));
    TEST_CHECK(engine.GetStreamCount() == 1 && engine.GetStreamIdAt(0) == g_sid);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PumpEngineTicks(engine, 3);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }

    // Part 5: Elapsed advances while playing, freezes while paused, advances after unpause
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const std::int32_t el_playing = engine.GetStreamElapsedSeconds(g_sid);
    TEST_CHECK(el_playing >= 1);

    // Part 3: Pause global stream -> both listeners receive Stop! Already paused -> returns 1!
    TEST_CHECK(engine.PauseAudioStream(g_sid, true));
    TEST_CHECK(engine.PauseAudioStream(g_sid, true));
    const std::int32_t el_at_pause = engine.GetStreamElapsedSeconds(g_sid);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        bool stop1 = false, stop2 = false;
        for (const auto& ev : host.events) {
            if (!ev.play && ev.player_id == 1) stop1 = true;
            if (!ev.play && ev.player_id == 2) stop2 = true;
        }
        TEST_CHECK(stop1 && stop2);
        host.events.clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TEST_CHECK(engine.GetStreamElapsedSeconds(g_sid) == el_at_pause);

    // Unpause global stream -> both listeners receive Play with identical URL!
    TEST_CHECK(engine.PauseAudioStream(g_sid, false));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        std::string url1, url2;
        for (const auto& ev : host.events) {
            if (ev.play && ev.player_id == 1) url1 = ev.url;
            if (ev.play && ev.player_id == 2) url2 = ev.url;
        }
        TEST_CHECK(!url1.empty() && !url2.empty());
        TEST_CHECK(url1 == url2);
        host.events.clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TEST_CHECK(engine.GetStreamElapsedSeconds(g_sid) > el_at_pause);

    TEST_CHECK(engine.StopAudioStream(g_sid));
    TEST_CHECK(!engine.PauseAudioStream(g_sid, true)); // Stopped stream -> returns 0
    TEST_CHECK(engine.GetStreamElapsedSeconds(g_sid) == -1);
    TEST_CHECK(engine.GetStreamCount() == 0);

    // Part 3 (personal transcoded): PauseAudioStream + ?resume= query offset verification
    {
        StreamOptions pr_opts;
        pr_opts.notify_processing = false;
        pr_opts.process_direct_audio_files = true;
        StreamId pr_sid = engine.PlayAudioStreamForPlayer(1, base + "/long.wav", pr_opts);
        TEST_CHECK(pr_sid != INVALID_STREAM_ID);
        std::string initial_purl;
        {
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < dl) {
                engine.Tick();
                {
                    std::lock_guard<std::mutex> lk(host.mtx);
                    for (const auto& ev : host.events) {
                        if (ev.play && ev.player_id == 1) initial_purl = ev.url;
                    }
                    if (!initial_purl.empty()) {
                        host.events.clear();
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
        auto path_pos = initial_purl.find("/stream/");
        TEST_CHECK(path_pos != std::string::npos);
        {
            asio::io_context gi_io;
            asio::ip::tcp::socket gi_sock(gi_io);
            gi_sock.connect({asio::ip::address_v4::loopback(), engine.GetHttpPort()});
            std::string req = "GET " + initial_purl.substr(path_pos) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
            asio::write(gi_sock, asio::buffer(req));
            std::string hdr_acc;
            std::array<char, 8192> tmp{};
            std::error_code rec;
            auto rdl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < rdl) {
                std::size_t n = gi_sock.read_some(asio::buffer(tmp), rec);
                if (n > 0) hdr_acc.append(tmp.data(), n);
                auto hpos = hdr_acc.find("\r\n\r\n");
                if (hpos != std::string::npos && hdr_acc.size() > hpos + 4 + 256) break;
                if (rec) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            gi_sock.close(rec);
        }
        TEST_CHECK(engine.PauseAudioStream(pr_sid, true));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(!host.events.empty() && !host.events.back().play && host.events.back().player_id == 1);
            host.events.clear();
        }
        TEST_CHECK(engine.PauseAudioStream(pr_sid, false));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(!host.events.empty() && host.events.back().play && host.events.back().player_id == 1);
            TEST_CHECK(host.events.back().url.find("?resume=") != std::string::npos);
            host.events.clear();
        }
        TEST_CHECK(engine.StopAudioStream(pr_sid));
    }

    // Disconnect clears mute
    TEST_CHECK(engine.SetPlayerAudioStreamMuted(1, true, false));
    TEST_CHECK(engine.IsPlayerAudioStreamMuted(1));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.players.erase(host.players.begin());
    }
    engine.OnPlayerDisconnect(1);
    TEST_CHECK(!engine.IsPlayerAudioStreamMuted(1));

    // --- Part 6: Channel playlist import ---
    TEST_PHASE("Part 6: Playlist import");
    ChannelId ch_pl1 = engine.CreateChannel("Playlist Empty");
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.playlist_queued_events.clear();
    }
    TEST_CHECK(!engine.QueueChannelPlaylist(99999, "https://example.com/playlist30"));
    TEST_CHECK(!engine.QueueChannelPlaylist(ch_pl1, "search text only")); // Non-URL rejected
    TEST_CHECK(engine.QueueChannelPlaylist(ch_pl1, "https://www.youtube.com/playlist?list=playlist30", 25));
    // Second concurrent import on same channel rejected!
    TEST_CHECK(!engine.QueueChannelPlaylist(ch_pl1, "https://www.youtube.com/playlist?list=playlist30", 5));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.playlist_queued_events.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.playlist_queued_events.size() == 1);
        TEST_CHECK(host.playlist_queued_events[0].channel_id == ch_pl1);
        TEST_CHECK(host.playlist_queued_events[0].added == 25);
        TEST_CHECK(host.playlist_queued_events[0].error_code == static_cast<std::int32_t>(ErrorCode::None));
        host.playlist_queued_events.clear();
    }
    // Stopped channel -> item 1 started playing, 24 remain queued!
    ChannelInfo pci{};
    TEST_CHECK(engine.GetChannelInfo(ch_pl1, pci));
    TEST_CHECK(pci.queue_count == 24);
    TEST_CHECK(engine.GetChannelState(ch_pl1) != StreamState::Idle &&
               engine.GetChannelState(ch_pl1) != StreamState::Stopped);

    // Fill queue to 45/50 -> next import of 30 items adds exactly 5!
    for (int i = 0; i < 21; ++i) {
        TEST_CHECK(engine.QueueChannel(ch_pl1, base + "/extra" + std::to_string(i) + ".wav"));
    }
    TEST_CHECK(engine.GetChannelInfo(ch_pl1, pci) && pci.queue_count == 45);
    TEST_CHECK(engine.QueueChannelPlaylist(ch_pl1, "https://www.youtube.com/playlist?list=playlist30", 25));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.playlist_queued_events.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.playlist_queued_events.size() == 1);
        TEST_CHECK(host.playlist_queued_events[0].added == 5);
        host.playlist_queued_events.clear();
    }
    TEST_CHECK(engine.GetChannelInfo(ch_pl1, pci) && pci.queue_count == 50);

    // Single video URL -> added = 1
    ChannelId ch_pl_single = engine.CreateChannel("Playlist Single");
    TEST_CHECK(engine.QueueChannelPlaylist(ch_pl_single, "https://www.youtube.com/watch?v=single_vid"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.playlist_queued_events.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.playlist_queued_events.size() == 1 && host.playlist_queued_events[0].added == 1);
        host.playlist_queued_events.clear();
    }

    // Channel destroyed mid-extraction -> cancelled cleanly, NO callback emitted!
    block_destroy_playlist.store(true);
    destroy_playlist_entered.store(false);
    ChannelId ch_pl_destroy = engine.CreateChannel("Playlist Destroyed");
    TEST_CHECK(engine.QueueChannelPlaylist(ch_pl_destroy, "https://www.youtube.com/playlist?list=destroy_mid"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !destroy_playlist_entered.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST_CHECK(destroy_playlist_entered.load());
    }
    TEST_CHECK(engine.DestroyChannel(ch_pl_destroy));
    block_destroy_playlist.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    PumpEngineTicks(engine, 4);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.playlist_queued_events.empty());
    }

    TEST_PHASE("teardown");
    engine.Shutdown();
    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    std::filesystem::remove_all(temp_base, ec);
    TEST_CHECK(!host.spatial_query_off_main_thread.load());
    std::cout << "[PASS] TestRound35FeaturesPart1To8\n";
}

static void TestRound35SeekAndTranscoder() {
    // 1. Direct AudioTranscoder test: 4s WAV -> start_offset_ms = 2000 produces ~half as much MP3 output as start_offset_ms = 0
    const std::string wav_4s = BuildTestWavBytes(16000, 4000);
    const std::string wav_5s = BuildTestWavBytes(16000, 5000);

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &wav_4s, &wav_5s]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sb);
                std::string method, path;
                is >> method >> path;
                if (path.find("/live_radio") != std::string::npos) {
                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\n\r\n";
                    if (method != "HEAD") resp += std::string(1024, '\0');
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                    return;
                }
                const std::string& payload = (path.find("/5s.wav") != std::string::npos) ? wav_5s : wav_4s;
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += payload;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    {
        TEST_PHASE("direct transcoder seek");
        TrackMetadata meta;
        meta.original_url = base + "/4s.wav";
        meta.direct_media_url = base + "/4s.wav";
        meta.is_live = false;

        auto run_tc = [&](std::uint64_t offset_ms) -> std::uint64_t {
            TranscoderConfig tcfg;
            tcfg.target_sample_rate = 48000;
            tcfg.target_bitrate_kbps = 128;
            tcfg.pace_realtime = false;
            tcfg.start_offset_ms = offset_ms;
            auto out_buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
            AudioTranscoder tc(meta, tcfg, out_buf);
            std::atomic<bool> cancel{false}, pause{false};
            std::atomic<float> vol{100.0f};
            std::string err;
            bool ok = tc.Run(cancel, pause, vol, err);
            if (!ok) {
                std::cerr << "[DEBUG] tc.Run(" << offset_ms << ") failed: " << err << "\n";
            }
            TEST_CHECK(ok);
            return out_buf->TotalWritten();
        };

        const std::uint64_t bytes_full = run_tc(0);
        const std::uint64_t bytes_half = run_tc(2000);
        // Full 4s @ 128kbps ~ 64 KB; seeked 2s ~ 32 KB (allow LAME padding variance)
        TEST_CHECK(bytes_full > 48000);
        TEST_CHECK(bytes_half > 20000 && bytes_half < (bytes_full * 2) / 3);
    }

    // 2. Engine Seek tests (with max_concurrent_transcoders = 1 so limit is 100% FULL during Seek!)
    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r35_seek_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(temp_base / "bin", ec);
    {
#ifdef _WIN32
        std::ofstream bf(temp_base / "bin" / "yt-dlp.exe", std::ios::binary);
#else
        std::ofstream bf(temp_base / "bin" / "yt-dlp", std::ios::binary);
#endif
        bf << std::string(128 * 1024, 'x');
    }

    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "Seeker"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.max_concurrent_streams = 1;
    cfg.max_concurrent_transcoders = 1; // Tight 1-slot limit!
    cfg.transcoder_burst_ms = 2000;

    TEST_PHASE("Initialize & wait extractor");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }
    TEST_PHASE("personal & channel seek");

    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string&,
        int,
        const std::atomic<bool>*) -> bool {
        (void)args;
        out_stdout =
            "{\"title\":\"Seekable Song\",\"artist\":\"Tester\",\"duration\":6,\"is_live\":false,"
            "\"ext\":\"wav\",\"acodec\":\"pcm\",\"url\":\"" + base + "/5s.wav\"}";
        return true;
    });

    // Reject seek on passthrough / live streams
    StreamOptions pt_opts;
    pt_opts.notify_processing = false;
    StreamId pt_sid = engine.PlayAudioStreamForPlayer(1, base + "/live_radio", pt_opts);
    PumpEngineTicks(engine, 3);
    TEST_CHECK(!engine.SeekAudioStream(pt_sid, 1));
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));

    // Paused personal stream seek with full concurrency limit (1/1) -> succeeds, stays Paused, elapsed == 4, natural finish fires!
    StreamOptions s_opts;
    s_opts.notify_processing = false;
    StreamId psid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=seek_personal", s_opts);
    TEST_CHECK(psid != INVALID_STREAM_ID);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            TrackMetadata m; StreamState st = StreamState::Idle;
            if (engine.GetStreamInfo(psid, m, st) && st == StreamState::Playing) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    TEST_CHECK(engine.PauseAudioStream(psid, true));
    TEST_CHECK(!engine.SeekAudioStream(psid, -1));
    TEST_CHECK(!engine.SeekAudioStream(psid, 6));
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.source_changes.clear();
        host.callback_names.clear();
    }
    TEST_CHECK(engine.SeekAudioStream(psid, 4));
    TEST_CHECK(engine.GetStreamElapsedSeconds(psid) == 4);
    {
        TrackMetadata m; StreamState st = StreamState::Idle;
        TEST_CHECK(engine.GetStreamInfo(psid, m, st) && st == StreamState::Paused);
    }
    TEST_CHECK(engine.PauseAudioStream(psid, false));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        bool finished = false;
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& cb : host.callback_names) {
                    if (cb == "OnAudioStreamFinished") {
                        finished = true;
                        break;
                    }
                }
            }
            if (finished) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(finished);
    }

    // Channel seek under full 1/1 transcoder limit -> TrackStart fires again, queue intact, NO QueueEnd, elapsed >= 4!
    ChannelId ch_seek = engine.CreateChannel("Seek FM");
    TEST_CHECK(engine.JoinChannel(1, ch_seek));
    TEST_CHECK(engine.PlayChannel(ch_seek, "https://www.youtube.com/watch?v=seek_ch"));
    TEST_CHECK(engine.QueueChannel(ch_seek, "https://www.youtube.com/watch?v=next_queued"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_track_starts.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        PumpEngineTicks(engine, 2);
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.channel_track_starts.size() == 1);
        host.channel_track_starts.clear();
        host.queue_ended_channels.clear();
        host.source_changes.clear();
    }
    TEST_CHECK(!engine.SeekChannelAudioStream(ch_seek, 6)); // >= duration
    TEST_CHECK(engine.SeekChannelAudioStream(ch_seek, 4));
    TEST_CHECK(engine.GetChannelElapsedSeconds(ch_seek) >= 4);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_track_starts.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        PumpEngineTicks(engine, 2);
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(!host.channel_track_starts.empty());
        TEST_CHECK(host.channel_track_starts[0].title == "Seekable Song");
        TEST_CHECK(host.queue_ended_channels.empty());
        TEST_CHECK(host.source_changes.empty()); // Same channel!
    }
    ChannelInfo sci{};
    TEST_CHECK(engine.GetChannelInfo(ch_seek, sci) && sci.queue_count == 1); // Queued track preserved!

    TEST_PHASE("teardown");
    engine.Shutdown();
    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    std::filesystem::remove_all(temp_base, ec);
    std::cout << "[PASS] TestRound35SeekAndTranscoder\n";
}

static void TestRound36AuditedFixes() {
    const std::string wav_8s = BuildTestWavBytes(16000, 8000);

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &wav_8s]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::istream is(&sb);
                std::string method, path;
                is >> method >> path;
                std::string resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(wav_8s.size()) + "\r\nConnection: close\r\n\r\n";
                if (method != "HEAD") resp += wav_8s;
                asio::error_code wec;
                asio::write(*sock, asio::buffer(resp), wec);
            });
        }
    });

    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r36_fixes_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(temp_base / "bin", ec);
    {
#ifdef _WIN32
        std::ofstream bf(temp_base / "bin" / "yt-dlp.exe", std::ios::binary);
#else
        std::ofstream bf(temp_base / "bin" / "yt-dlp", std::ios::binary);
#endif
        bf << std::string(128 * 1024, 'x');
    }

    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "RapidSeeker"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.max_concurrent_streams = 1;
    cfg.max_concurrent_transcoders = 1; // Strictly 1 slot!
    cfg.transcoder_burst_ms = 2000;

    TEST_PHASE("Initialize & wait extractor");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }
    TEST_PHASE("personal & channel seek");

    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string&,
        int,
        const std::atomic<bool>*) -> bool {
        std::string target;
        bool flat = false;
        for (const auto& a : args) {
            if (a == "--flat-playlist") flat = true;
            if (a.rfind("http", 0) == 0) target = a;
        }
        if (flat && target.find("list=pl5") != std::string::npos) {
            out_stdout =
                "{\"_type\":\"playlist\",\"entries\":["
                "{\"id\":\"pl5_0\",\"url\":\"https://www.youtube.com/watch?v=pl5_0\"},"
                "{\"id\":\"pl5_1\",\"url\":\"https://www.youtube.com/watch?v=pl5_1\"},"
                "{\"id\":\"pl5_2\",\"url\":\"https://www.youtube.com/watch?v=pl5_2\"},"
                "{\"id\":\"pl5_3\",\"url\":\"https://www.youtube.com/watch?v=pl5_3\"},"
                "{\"id\":\"pl5_4\",\"url\":\"https://www.youtube.com/watch?v=pl5_4\"}"
                "]}";
            return true;
        }
        out_stdout =
            "{\"title\":\"Rapid Track\",\"artist\":\"Tester\",\"duration\":8,\"is_live\":false,"
            "\"ext\":\"wav\",\"acodec\":\"pcm\",\"url\":\"" + base + "/8s.wav\"}";
        return true;
    });

    // 1. Personal transcoded stream at limit 1: 3 consecutive seeks without waiting -> all 3 return 1,
    //    only 1 transcoder remains active with final offset (4s -> ~4s left on 8s WAV > 2s burst).
    TEST_PHASE("personal rapid seeks");
    StreamOptions opts;
    opts.notify_processing = false;
    StreamId psid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=r36_personal", opts);
    TEST_CHECK(psid != INVALID_STREAM_ID);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            TrackMetadata m; StreamState st = StreamState::Idle;
            if (engine.GetStreamInfo(psid, m, st) && st == StreamState::Playing) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    TEST_CHECK(engine.SeekAudioStream(psid, 1));
    TEST_CHECK(engine.SeekAudioStream(psid, 2));
    TEST_CHECK(engine.SeekAudioStream(psid, 4));
    TEST_CHECK(engine.GetStreamElapsedSeconds(psid) >= 4);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            if (engine.GetStats().active_transcoders == 1) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.Tick();
        TEST_CHECK(engine.GetStats().active_transcoders == 1);
    }
    // Verify ~4s output from offset 4000ms on 8s WAV (~64 KB @ 128kbps, well below >=96 KB for 0/1/2s):
    {
        std::string play_url;
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            for (const auto& ev : host.events) {
                if (ev.play && ev.player_id == 1) play_url = ev.url;
            }
        }
        auto path_pos = play_url.find("/stream/");
        TEST_CHECK(path_pos != std::string::npos);
        asio::io_context c_io;
        asio::ip::tcp::socket c_sock(c_io);
        c_sock.connect({asio::ip::address_v4::loopback(), engine.GetHttpPort()});
        std::string req = "GET " + play_url.substr(path_pos) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        asio::write(c_sock, asio::buffer(req));
        std::string raw;
        std::array<char, 8192> tmp{};
        std::error_code rec;
        while (true) {
            std::size_t n = c_sock.read_some(asio::buffer(tmp), rec);
            if (n > 0) raw.append(tmp.data(), n);
            if (rec) break;
        }
        auto hpos = raw.find("\r\n\r\n");
        TEST_CHECK(hpos != std::string::npos);
        std::size_t mp3_bytes = raw.size() - (hpos + 4);
        // Remaining 4s of 8s @ 128kbps ~= 64 KB (must be clearly < 82 KB so offset 4s ran, not 0/1/2s)
        TEST_CHECK(mp3_bytes > 48000 && mp3_bytes < 82000);
    }
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && engine.GetStats().active_transcoders > 0) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.GetStats().active_transcoders == 0);
    }

    // 2. Channel at limit 1: 3 consecutive seeks without waiting -> all 3 return 1,
    //    exactly 1 active run remains with final offset (4s -> >= 4s elapsed).
    TEST_PHASE("channel rapid seeks");
    ChannelId ch_seek = engine.CreateChannel("Rapid Seek Channel");
    TEST_CHECK(engine.JoinChannel(1, ch_seek));
    TEST_CHECK(engine.PlayChannel(ch_seek, "https://www.youtube.com/watch?v=r36_ch"));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_track_starts.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.channel_track_starts.size() == 1);
        host.channel_track_starts.clear();
    }
    TEST_CHECK(engine.SeekChannelAudioStream(ch_seek, 1));
    TEST_CHECK(engine.SeekChannelAudioStream(ch_seek, 2));
    TEST_CHECK(engine.SeekChannelAudioStream(ch_seek, 4));
    TEST_CHECK(engine.GetChannelElapsedSeconds(ch_seek) >= 4);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.channel_track_starts.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.Tick();
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.channel_track_starts.size() == 1);
    }
    TEST_CHECK(engine.GetStats().active_transcoders == 1);
    TEST_CHECK(engine.GetChannelElapsedSeconds(ch_seek) >= 4);

    // 3. Playlist into stopped channel while limit 1 is full -> added=0, SA_ERR_INTERNAL_ERROR,
    //    "Maximum concurrent transcoders reached", queue stays empty, channel stays stopped.
    TEST_PHASE("stopped playlist at limit");
    ChannelId ch_stopped = engine.CreateChannel("Stopped Full Limit");
    TEST_CHECK(ch_stopped != INVALID_CHANNEL_ID);
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.playlist_queued_events.clear();
    }
    TEST_CHECK(engine.QueueChannelPlaylist(ch_stopped, "https://www.youtube.com/playlist?list=pl5", 5));
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.playlist_queued_events.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.playlist_queued_events.size() == 1);
        TEST_CHECK(host.playlist_queued_events[0].channel_id == ch_stopped);
        TEST_CHECK(host.playlist_queued_events[0].added == 0);
        TEST_CHECK(host.playlist_queued_events[0].error_code == static_cast<std::int32_t>(ErrorCode::InternalError));
        TEST_CHECK(host.playlist_queued_events[0].error_message == "Maximum concurrent transcoders reached");
    }
    ChannelInfo sci_stopped{};
    TEST_CHECK(engine.GetChannelInfo(ch_stopped, sci_stopped));
    TEST_CHECK(sci_stopped.queue_count == 0);
    TEST_CHECK(sci_stopped.state == StreamState::Idle || sci_stopped.state == StreamState::Stopped);

    TEST_PHASE("teardown");
    engine.Shutdown();
    stop_srv.store(true);
    asio::error_code cec;
    acceptor.close(cec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    std::filesystem::remove_all(temp_base, ec);
    TEST_CHECK(!host.spatial_query_off_main_thread.load());
    std::cout << "[PASS] TestRound36AuditedFixes\n";
}

static void TestRound37ExtractorSettledErrorMessage() {
    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r37_extractor_msg_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(temp_base / "bin", ec);

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.persist_channels = false;

    // 1. Before BinaryManager settles -> Resolve returns "Extractor is still initializing"
    {
        BinaryManager bm(cfg);
        MetadataCache mc(16, 60);
        StreamResolver resolver(cfg, bm, mc);
        TEST_CHECK(!bm.IsReady() && !bm.IsSettled() && !bm.HasProvisioningFailed());
        ResolveResult r_pre = resolver.Resolve("https://www.youtube.com/watch?v=r37_pre");
        TEST_CHECK(!r_pre.success && r_pre.error == ErrorCode::ExtractorNotReady);
        TEST_CHECK(r_pre.error_message == "Extractor is still initializing");

        // After settling with auto_download=false and missing binary -> HasProvisioningFailed() == true
        // and all 3 paths return unavailable message
        ThreadPool pool;
        pool.Start(1, 256 * 1024);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(3)));
        pool.Stop();
        TEST_CHECK(!bm.IsReady() && bm.IsSettled() && bm.HasProvisioningFailed());

        ResolveResult r_res = resolver.Resolve("https://www.youtube.com/watch?v=r37_post");
        TEST_CHECK(!r_res.success && r_res.error == ErrorCode::ExtractorNotReady);
        TEST_CHECK(r_res.error_message == "Extractor unavailable: yt-dlp could not be provisioned (see server log)");

        SearchExecutionResult r_srch = resolver.Search("test song", 5);
        TEST_CHECK(!r_srch.success && r_srch.error == ErrorCode::ExtractorNotReady);
        TEST_CHECK(r_srch.error_message == "Extractor unavailable: yt-dlp could not be provisioned (see server log)");

        PlaylistExtractionResult r_pl = resolver.ExtractPlaylist("https://www.youtube.com/playlist?list=r37_pl", 5);
        TEST_CHECK(!r_pl.success && r_pl.error == ErrorCode::ExtractorNotReady);
        TEST_CHECK(r_pl.error_message == "Extractor unavailable: yt-dlp could not be provisioned (see server log)");
    }

    // 1b. After successful provisioning -> HasProvisioningFailed() stays false
    {
        const std::filesystem::path ok_base = temp_base / "ok_case";
        std::filesystem::create_directories(ok_base / "bin", ec);
        {
#ifdef _WIN32
            std::ofstream bf(ok_base / "bin" / "yt-dlp.exe", std::ios::binary);
#else
            std::ofstream bf(ok_base / "bin" / "yt-dlp", std::ios::binary);
#endif
            bf << std::string(128 * 1024, 'x');
        }
        PluginConfig ok_cfg = cfg;
        ok_cfg.base_dir = ok_base.string();
        ok_cfg.bin_dir = (ok_base / "bin").string();
        BinaryManager bm_ok(ok_cfg);
        ThreadPool pool;
        pool.Start(1, 256 * 1024);
        bm_ok.InitializeAsync(pool);
        TEST_CHECK(bm_ok.WaitUntilSettled(std::chrono::seconds(3)));
        pool.Stop();
        TEST_CHECK(bm_ok.IsReady() && bm_ok.IsSettled() && !bm_ok.HasProvisioningFailed());
    }

    // 2. AudioEngine end-to-end with auto_download=false and no binary after settle -> OnAudioStreamError message
    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "NoExtractor"}};

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    TEST_CHECK(engine.BinariesForTesting().WaitUntilSettled(std::chrono::seconds(5)));
    TEST_CHECK(!engine.IsExtractorReady() && engine.BinariesForTesting().HasProvisioningFailed());

    StreamOptions opts;
    opts.notify_processing = false;
    StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=r37_engine", opts);
    TEST_CHECK(sid != INVALID_STREAM_ID);
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            if (!host.error_events.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::lock_guard<std::mutex> lk(host.mtx);
        TEST_CHECK(host.error_events.size() == 1);
        TEST_CHECK(host.error_events[0].stream_id == sid);
        TEST_CHECK(host.error_events[0].error_code == static_cast<std::int32_t>(ErrorCode::ExtractorNotReady));
        TEST_CHECK(host.error_events[0].message == "Extractor unavailable: yt-dlp could not be provisioned (see server log)");
    }

    engine.Shutdown();
    TEST_CHECK(!host.spatial_query_off_main_thread.load());
    std::filesystem::remove_all(temp_base, ec);
    std::cout << "[PASS] TestRound37ExtractorSettledErrorMessage\n";
}

void TestRound39WinHttpCancellationAndTimeouts() {
    std::vector<std::uint8_t> slow_chunk(1024, 0x41);

    asio::io_context srv_io;
    asio::ip::tcp::acceptor acc(srv_io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::uint16_t port = acc.local_endpoint().port();
    std::atomic<bool> stop_srv{false};
    std::mutex sock_mtx;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;

    std::thread srv([&]() {
        std::vector<std::thread> workers;
        while (!stop_srv.load()) {
            auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
            std::error_code ec;
            acc.accept(*sock, ec);
            if (ec || stop_srv.load()) break;
            ConfigureTestSocketTimeouts(*sock);
            {
                std::lock_guard<std::mutex> lk(sock_mtx);
                sockets.push_back(sock);
            }
            workers.emplace_back([sock, &stop_srv, &slow_chunk]() {
                std::error_code io_ec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", io_ec);
                if (io_ec) {
                    sock->close(io_ec);
                    return;
                }
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream iss(raw_req);
                std::string method, path;
                iss >> method >> path;

                if (path == "/hook-test.bin") {
                    std::string body(4096, 'Z');
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    if (!io_ec) asio::write(*sock, asio::buffer(body), io_ec);
                } else if (path == "/hold-body.bin") {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    if (!io_ec) asio::write(*sock, asio::buffer(slow_chunk), io_ec);
                    for (int t = 0; t < 160 && !io_ec && !stop_srv.load(); ++t) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                } else {
                    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), io_ec);
                    for (int c = 0; c < 100 && !io_ec && !stop_srv.load(); ++c) {
                        asio::write(*sock, asio::buffer(slow_chunk), io_ec);
                        if (io_ec) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                sock->close(io_ec);
            });
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    });

    HttpClient::Initialize();
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "", 0});
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // 1. Cancel 50 consecutive in-flight requests during body receive; PollCancellations must stay < 5 ms.
    TEST_PHASE("50x in-flight body cancel");
    auto t_sub1 = std::chrono::steady_clock::now();
    std::int64_t max_poll_us = 0;
    for (int i = 0; i < 50; ++i) {
        auto t_iter = std::chrono::steady_clock::now();
        std::atomic<bool> cancel{false};
        std::atomic<bool> got_chunk{false};
        std::string err;
        std::thread client([&]() {
            HttpClient::StreamGet(
                base + "/slow.bin?i=" + std::to_string(i),
                {},
                [&](const std::uint8_t*, std::size_t sz) {
                    if (sz > 0) got_chunk.store(true);
                    return !cancel.load();
                },
                cancel,
                err,
                nullptr,
                15000);
        });

        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (!got_chunk.load() && std::chrono::steady_clock::now() < dl) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TEST_CHECK(got_chunk.load());

        cancel.store(true);
        auto t0 = std::chrono::steady_clock::now();
        HttpClient::PollCancellations();
        auto poll_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (poll_us > max_poll_us) max_poll_us = poll_us;

        client.join();
        auto iter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_iter).count();
        if (iter_ms > 250) {
            std::cout << "[INFO] Round39 slow iter " << i << ": " << iter_ms << " ms\n";
        }
        TEST_CHECK(max_poll_us < 5000);
    }
    HttpClient::FlushCancellationsForTesting();
    auto sub1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_sub1).count();

    // 2. Cancel between WinHttpReceiveResponse (after headers) and WinHttpReadData via test hook.
    auto t_sub2 = std::chrono::steady_clock::now();
#ifdef _WIN32
    TEST_PHASE("hook between WinHttpReceiveResponse and WinHttpReadData");
    {
        std::uint64_t read_calls_before = HttpClient::GetWinHttpReadDataCallCountForTesting();
        std::atomic<bool> cancel{false};
        std::atomic<bool> hook_fired{false};
        HttpClient::SetWinHttpBetweenCallsHookForTesting([&]() {
            hook_fired.store(true);
            cancel.store(true);
            HttpClient::FlushCancellationsForTesting();
        });
        HttpResponse resp = HttpClient::Get(base + "/hook-test.bin", {}, 5000, &cancel);
        HttpClient::SetWinHttpBetweenCallsHookForTesting({});
        TEST_CHECK(hook_fired.load());
        TEST_CHECK(!resp.Ok());
        TEST_CHECK(HttpClient::GetWinHttpReadDataCallCountForTesting() == read_calls_before);
    }
#endif
    auto sub2_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_sub2).count();

    // 3. PollCancellations after HttpClient::Shutdown must close inline without spawning a new closer thread.
    TEST_PHASE("post-Shutdown PollCancellations creates no closer thread");
    {
        HttpClient::Shutdown();
        const std::uint64_t threads_before = HttpClient::GetWinHttpCloserThreadCountForTesting();
        std::atomic<bool> cancel{false};
        std::atomic<bool> got_chunk{false};
        std::string err;
        std::thread client([&]() {
            HttpClient::StreamGet(
                base + "/hold-body.bin",
                {},
                [&](const std::uint8_t*, std::size_t sz) {
                    if (sz > 0) got_chunk.store(true);
                    return !cancel.load();
                },
                cancel,
                err,
                nullptr,
                15000);
        });

        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (!got_chunk.load() && std::chrono::steady_clock::now() < dl) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TEST_CHECK(got_chunk.load());
        // Let client enter WinHttpQueryDataAvailable waiting for chunk 2 while server holds for 4s.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        auto t_close0 = std::chrono::steady_clock::now();
        cancel.store(true);
        HttpClient::PollCancellations();
        client.join();
        auto close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_close0).count();

        TEST_CHECK(HttpClient::GetWinHttpCloserThreadCountForTesting() == threads_before);
        TEST_CHECK(close_ms < 1500);
    }

    TEST_PHASE("teardown");
    auto t_td = std::chrono::steady_clock::now();
    HttpClient::Shutdown();
    HttpClient::Initialize();
    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    CloseTestSockets(sockets, sock_mtx);
    StopTestAcceptor(acc, port);
    if (srv.joinable()) srv.join();
    auto td_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_td).count();
    std::cout << "[PASS] TestRound39WinHttpCancellationAndTimeouts (max PollCancellations="
              << max_poll_us << " us, sub1=" << sub1_ms << " ms, sub2=" << sub2_ms
              << " ms, td=" << td_ms << " ms)\n";
}

void TestRound41ExtractorReResolveAndPersonalLog() {
    const std::string valid_wav = BuildTestWavBytes(16000, 1500);
    const std::size_t split_offset = valid_wav.size() / 2;

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<bool> stop_srv{false};
    std::atomic<bool> saw_range_resume_on_fresh{false};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &valid_wav, split_offset, &saw_range_resume_on_fresh]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream is(raw_req);
                std::string method, path;
                is >> method >> path;

                std::size_t range_start = 0;
                bool has_range = false;
                {
                    std::string lower_req = raw_req;
                    for (char& ch : lower_req) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    auto rpos = lower_req.find("range: bytes=");
                    if (rpos != std::string::npos) {
                        has_range = true;
                        range_start = static_cast<std::size_t>(std::strtoull(lower_req.c_str() + rpos + 13, nullptr, 10));
                    }
                }

                asio::error_code wec;
                if (path == "/expired" || path == "/fresh_403" || path == "/direct_403.mp3" ||
                    (path == "/drop_partial" && has_range)) {
                    std::string resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(resp), wec);
                } else if (path == "/drop_partial") {
                    std::string hdr =
                        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                        std::to_string(valid_wav.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), wec);
                    if (!wec && method != "HEAD") {
                        asio::write(*sock, asio::buffer(valid_wav.data(), split_offset), wec);
                    }
                } else if (path == "/resume_ok" && has_range && range_start < valid_wav.size()) {
                    saw_range_resume_on_fresh.store(true);
                    const std::size_t rem = valid_wav.size() - range_start;
                    std::string hdr =
                        "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nContent-Length: " +
                        std::to_string(rem) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), wec);
                    if (!wec && method != "HEAD") {
                        asio::write(*sock, asio::buffer(valid_wav.data() + range_start, rem), wec);
                    }
                } else {
                    std::string hdr =
                        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                        std::to_string(valid_wav.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::write(*sock, asio::buffer(hdr), wec);
                    if (!wec && method != "HEAD") {
                        asio::write(*sock, asio::buffer(valid_wav), wec);
                    }
                }
                sock->close(wec);
            });
        }
    });

    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r41_reresolve_test";
    std::error_code fec;
    std::filesystem::remove_all(temp_base, fec);
    std::filesystem::create_directories(temp_base / "bin", fec);
    {
#ifdef _WIN32
        const std::filesystem::path ytdlp_bin = temp_base / "bin" / "yt-dlp.exe";
#else
        const std::filesystem::path ytdlp_bin = temp_base / "bin" / "yt-dlp";
#endif
        std::ofstream bf(ytdlp_bin, std::ios::binary);
        bf << std::string(128 * 1024, 'x');
    }

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> captured_logs;
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        captured_logs.emplace_back(sev, msg);
    });

    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "Tester41"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.transcoder_burst_ms = 2000;

    TEST_PHASE("Initialize & wait extractor");
    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }

    std::atomic<int> resolve_count{0};
    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string&,
        int,
        const std::atomic<bool>*) -> bool {
        const std::string& target = args.empty() ? std::string() : args.back();
        const int hit = resolve_count.fetch_add(1) + 1;
        if (target.find("yt_recover") != std::string::npos) {
            const std::string u = (hit == 1) ? (base + "/expired") : (base + "/fresh");
            out_stdout = "{\"id\":\"EtTbS-KdcrE\",\"format_id\":\"251\",\"available_at\":1,\"url\":\"" + u +
                         "\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_double403") != std::string::npos) {
            const std::string u = (hit == 1) ? (base + "/expired") : (base + "/fresh_403");
            out_stdout = "{\"id\":\"DoubleFail1\",\"format_id\":\"251\",\"available_at\":1,\"url\":\"" + u +
                         "\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_range403") != std::string::npos) {
            const std::string u = (hit == 1) ? (base + "/drop_partial") : (base + "/resume_ok");
            out_stdout = "{\"id\":\"RangeFail1\",\"format_id\":\"251\",\"url\":\"" + u +
                         "\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        return false;
    });

    auto has_log = [&](LogSeverity sev, const std::string& substr) {
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& [s, m] : captured_logs) {
            if (s == sev && m.find(substr) != std::string::npos) return true;
        }
        return false;
    };

    // 1. /expired (403) -> re-resolve once -> /fresh (200): plays & finishes with exactly 2 resolves.
    TEST_PHASE("403 recovers after single re-resolve");
    {
        resolve_count.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        {
            std::lock_guard<std::mutex> lk(log_mtx);
            captured_logs.clear();
        }
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_recover", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool finished = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamFinished") !=
                    host.callback_names.end()) {
                    finished = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(finished);
        TEST_CHECK(resolve_count.load() == 2);
        TEST_CHECK(has_log(LogSeverity::Warning, "Media URL rejected with HTTP 403; re-resolving EtTbS-KdcrE once"));
    }

    // 2. /expired (403) -> re-resolve -> /fresh_403 (403): fails with "after re-resolve" & only 2 resolves.
    TEST_PHASE("403 twice fails after re-resolve without looping");
    {
        resolve_count.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        {
            std::lock_guard<std::mutex> lk(log_mtx);
            captured_logs.clear();
        }
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_double403", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        std::string err_msg;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (!host.error_events.empty()) {
                    err_msg = host.error_events.back().message;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(resolve_count.load() == 2);
        TEST_CHECK(err_msg.find("HTTP status 403 after re-resolve") != std::string::npos);
        TEST_CHECK(has_log(LogSeverity::Error, "transcode failed: HTTP status 403 after re-resolve"));
        TEST_CHECK(has_log(LogSeverity::Error, "(player 1) transcode failed:"));
    }

    // 3. Direct URL with 403: immediate error, zero re-resolutions, logged to server log.
    TEST_PHASE("direct URL 403 fails immediately with no re-resolve");
    {
        resolve_count.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        {
            std::lock_guard<std::mutex> lk(log_mtx);
            captured_logs.clear();
        }
        StreamOptions opts;
        opts.process_direct_audio_files = true;
        StreamId sid = engine.PlayAudioStreamForPlayer(1, base + "/direct_403.mp3", opts);
        TEST_CHECK(sid != INVALID_STREAM_ID);

        std::string err_msg;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (!host.error_events.empty()) {
                    err_msg = host.error_events.back().message;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(resolve_count.load() == 0);
        TEST_CHECK(err_msg.find("HTTP status 403 (attempt 1/3, byte 0)") != std::string::npos);
        TEST_CHECK(err_msg.find("after re-resolve") == std::string::npos);
        TEST_CHECK(has_log(LogSeverity::Error, "(player 1) transcode failed: HTTP status 403 (attempt 1/3, byte 0)"));
    }

    // 4. Range resume rejected with 403 -> re-resolve once -> /resume_ok (206) completes cleanly.
    TEST_PHASE("Range resume 403 recovers via re-resolve and 206");
    {
        resolve_count.store(0);
        saw_range_resume_on_fresh.store(false);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        {
            std::lock_guard<std::mutex> lk(log_mtx);
            captured_logs.clear();
        }
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_range403", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool finished = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamFinished") !=
                    host.callback_names.end()) {
                    finished = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(finished);
        TEST_CHECK(resolve_count.load() == 2);
        TEST_CHECK(saw_range_resume_on_fresh.load());
        TEST_CHECK(has_log(LogSeverity::Warning, "Media URL rejected with HTTP 403; re-resolving RangeFail1 once"));
    }

    TEST_PHASE("teardown");
    engine.Shutdown();
    Logger::Instance().SetSink(nullptr);
    stop_srv.store(true);
    acceptor.close(fec);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
    std::filesystem::remove_all(temp_base, fec);
    std::cout << "[PASS] TestRound41ExtractorReResolveAndPersonalLog\n";
}

void TestRound42HttpDiagnosticsAndDenoRuntime() {
    Logger::Instance().Flush();

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetDebugEnabled(true);

    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& [s, m] : logs) {
            if (s == sev && m.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    auto any_log_contains = [&](const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& [s, m] : logs) {
            (void)s;
            if (m.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    // Part A: Redirected 403 error body preview + First HTTP response + format_id/c= logging without leaking secrets.
    TEST_PHASE("Part A: HTTP 403 debug details & First HTTP response");
    {
        asio::io_context io;
        asio::ip::tcp::acceptor acc(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const unsigned short port = acc.local_endpoint().port();
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        std::atomic<bool> stop_srv{false};

        std::thread srv([&]() {
            while (!stop_srv.load()) {
                asio::error_code ec;
                asio::ip::tcp::socket sock(io);
                acc.accept(sock, ec);
                if (ec || stop_srv.load()) break;
                ConfigureTestSocketTimeouts(sock);
                asio::streambuf sb;
                asio::read_until(sock, sb, "\r\n\r\n", ec);
                if (!ec) {
                    std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                    std::istringstream iss(raw_req);
                    std::string method, path;
                    iss >> method >> path;
                    if (path.rfind("/redir_to_403", 0) == 0) {
                        std::string resp =
                            "HTTP/1.1 302 Found\r\nLocation: " + base +
                            "/err403?token=SECRET_COOKIE_VAL\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        asio::write(sock, asio::buffer(resp), ec);
                    } else if (path.rfind("/err403", 0) == 0) {
                        std::string body = std::string("Forbidden\x01\x02" "DetailBody_") + std::string(240, 'X');
                        std::string resp =
                            "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        asio::write(sock, asio::buffer(resp), ec);
                    } else {
                        std::string body = BuildTestWavBytes(16000, 300);
                        std::string resp =
                            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        asio::write(sock, asio::buffer(resp), ec);
                    }
                }
                sock.close(ec);
            }
        });

        HttpClient::SetSecurityPolicy({true, "127.0.0.1", "", 0});

        const std::filesystem::path temp_a =
            std::filesystem::temp_directory_path() / "sampaudiostream_r42_parta";
        std::error_code tec;
        std::filesystem::remove_all(temp_a, tec);
        std::filesystem::create_directories(temp_a, tec);
#ifdef _WIN32
        const std::filesystem::path fake_yt = temp_a / "yt-dlp.exe";
#else
        const std::filesystem::path fake_yt = temp_a / "yt-dlp";
#endif
        {
            std::ofstream ofs(fake_yt, std::ios::binary);
            ofs << std::string(128 * 1024, 'y');
        }

        PluginConfig pcfg;
        pcfg.bin_dir = temp_a.string();
        pcfg.auto_download_extractor = false;
        pcfg.auto_update_extractor = false;
        pcfg.enable_js_runtime = false;
        pcfg.debug_logging = true;

        BinaryManager bm(pcfg);
        MetadataCache mc(16, 60);
        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsReady());

        StreamResolver resolver(pcfg, bm, mc);
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) {
            out_stdout =
                "{\"id\":\"EtTbS-KdcrE\",\"format_id\":\"251\",\"url\":\"" + base +
                "/redir_to_403?c=TVHTML5&sig=TOP_SECRET_SIG\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        });

        clear_logs();
        ResolveResult rr = resolver.Resolve("https://www.youtube.com/watch?v=EtTbS-KdcrE");
        TEST_CHECK(rr.success);
        TEST_CHECK(find_log(LogSeverity::Debug, "format_id=251"));
        TEST_CHECK(find_log(LogSeverity::Debug, "c=TVHTML5"));
        TEST_CHECK(!any_log_contains("TOP_SECRET_SIG"));

        clear_logs();
        rr.metadata.resolved_at = std::chrono::steady_clock::now() - std::chrono::seconds(15);
        TranscoderConfig tcfg;
        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string trans_err;
        AudioTranscoder tr(rr.metadata, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, trans_err));
        TEST_CHECK(find_log(LogSeverity::Debug, "First HTTP response HTTP 403"));
        TEST_CHECK(find_log(LogSeverity::Debug, "HTTP error details: status=403, attempt=1/3, host=127.0.0.1, since_resolve_ms="));
        TEST_CHECK(find_log(LogSeverity::Debug, "redirected=true (redirect_host=127.0.0.1)"));
        TEST_CHECK(find_log(LogSeverity::Debug, "body_preview=\"Forbidden..DetailBody_"));
        TEST_CHECK(!any_log_contains("SECRET_COOKIE_VAL"));
        TEST_CHECK(!any_log_contains("TOP_SECRET_SIG"));

        pool.Stop();
        HttpClient::Shutdown();
        HttpClient::SetSecurityPolicy({});
        stop_srv.store(true);
        StopTestAcceptor(acc, port);
        if (srv.joinable()) srv.join();
        std::filesystem::remove_all(temp_a, tec);
    }

    // Part B.1: extractor.js_runtime config loading, saving & sanitization.
    TEST_PHASE("Part B.1: extractor.js_runtime config validation");
    {
        const std::filesystem::path cfg_path =
            std::filesystem::temp_directory_path() / "sampaudiostream_r42_config.json";
        std::error_code ec;
        std::filesystem::remove(cfg_path, ec);

        PluginConfig created = ConfigManager::LoadOrCreate(cfg_path.string());
        TEST_CHECK(created.js_runtime == "auto");
        {
            std::ifstream ifs(cfg_path);
            std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(txt.find("\"js_runtime\": \"auto\"") != std::string::npos);
        }

        PluginConfig dirty;
        dirty.js_runtime = "DeNo";
        ConfigManager::Sanitize(dirty);
        TEST_CHECK(dirty.js_runtime == "deno");

        dirty.js_runtime = "invalid_engine";
        clear_logs();
        ConfigManager::Sanitize(dirty);
        TEST_CHECK(dirty.js_runtime == "auto");
        TEST_CHECK(find_log(LogSeverity::Warning, "extractor.js_runtime"));

        std::filesystem::remove(cfg_path, ec);
    }

    // Part B.5 (1-5): BinaryManager + StreamResolver Deno/QuickJS selection, env vars, readiness, health failure & fallback.
    const std::filesystem::path temp_b =
        std::filesystem::temp_directory_path() / "sampaudiostream_r42_partb";
    std::error_code bec;
    std::filesystem::remove_all(temp_b, bec);
    std::filesystem::create_directories(temp_b, bec);

    auto make_fake_yt_and_qjs = [&](const std::filesystem::path& dir, bool with_qjs = true) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
        const std::filesystem::path yt = dir / "yt-dlp.exe";
        const std::filesystem::path qjs = dir / "qjs.exe";
#else
        const std::filesystem::path yt = dir / "yt-dlp";
        const std::filesystem::path qjs = dir / "qjs";
#endif
        {
            std::ofstream ofs(yt, std::ios::binary);
            ofs << std::string(128 * 1024, 'y');
        }
        if (with_qjs) {
            std::ofstream ofs(qjs, std::ios::binary);
            ofs << std::string(64 * 1024, 'q');
        }
    };
    auto write_fake_deno = [&](const std::string& exe_path) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(exe_path).parent_path(), ec);
        std::ofstream ofs(exe_path, std::ios::binary);
        ofs << std::string(64 * 1024, 'd');
    };
    auto args_contain = [](const std::vector<std::string>& args, const std::string& needle) {
        for (const auto& a : args) {
            if (a.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    // 1 & 2. Selection matrix + DENO_DIR / DENO_NO_UPDATE_CHECK env + startup log.
    TEST_PHASE("Part B.5: selection matrix, env vars, and readiness");
    {
        const std::filesystem::path d1 = temp_b / "case1";
        make_fake_yt_and_qjs(d1, true);

        PluginConfig c;
        c.bin_dir = d1.string();
        c.auto_download_extractor = true;
        c.auto_update_extractor = false;
        c.enable_js_runtime = true;
        c.js_runtime = "auto";
        c.log_extractor_status = true;

        std::atomic<bool> allow_deno{false};
        std::atomic<int> dl_calls{0};
        BinaryManager bm(c);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoDownloadHookForTesting([&](const std::string& exe_p, const std::string&, const std::atomic<bool>*, std::string&) {
            dl_calls.fetch_add(1);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!allow_deno.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            write_fake_deno(exe_p);
            return true;
        });
        bm.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& out_ver, std::string&) {
            out_ver = "2.9.8";
            return true;
        });

        MetadataCache mc(16, 60);
        StreamResolver resolver(c, bm, mc);
        std::vector<std::string> last_args;
        std::vector<std::pair<std::string, std::string>> last_env;
        resolver.SetYtDlpEnvObserverForTesting([&](const std::vector<std::pair<std::string, std::string>>& env) {
            last_env = env;
        });
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) {
            last_args = args;
            out_stdout = "{\"id\":\"v1\",\"format_id\":\"251\",\"url\":\"https://example.com/a.webm\"}";
            return true;
        });

        ThreadPool pool(1);
        clear_logs();
        bm.InitializeAsync(pool);
        // Readiness settles BEFORE slow Deno download completes!
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsReady());
        TEST_CHECK(!bm.IsDenoReady());
        TEST_CHECK(find_log(LogSeverity::Info, "Audio extractor subsystem ready (JS runtime: quickjs)."));

        // First resolution during background Deno download uses QuickJS with no DENO env vars.
        ResolveResult r_qjs = resolver.Resolve("https://www.youtube.com/watch?v=r1");
        TEST_CHECK(r_qjs.success);
        TEST_CHECK(args_contain(last_args, "quickjs:"));
        TEST_CHECK(!args_contain(last_args, "deno:"));
        TEST_CHECK(last_env.empty());

        // Let slow Deno finish -> next resolution switches to deno + injects DENO_DIR / DENO_NO_UPDATE_CHECK.
        allow_deno.store(true);
        TEST_CHECK(bm.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(3)));
        TEST_CHECK(dl_calls.load() == 1);
        TEST_CHECK(bm.IsDenoReady());

        ResolveResult r_deno = resolver.Resolve("https://www.youtube.com/watch?v=r2");
        TEST_CHECK(r_deno.success);
        TEST_CHECK(args_contain(last_args, "deno:"));
        TEST_CHECK(!args_contain(last_args, "quickjs:"));
        TEST_CHECK(last_env.size() == 2);
        TEST_CHECK(last_env[0].first == "DENO_DIR" && last_env[0].second.find("deno-cache") != std::string::npos);
        TEST_CHECK(last_env[1].first == "DENO_NO_UPDATE_CHECK" && last_env[1].second == "1");
        pool.Stop();
    }

    // Unsupported arch + forced "quickjs" + forced "deno" missing + enable_js_runtime=false
    TEST_PHASE("Part B.5: arch unsupported, quickjs force, deno missing warning, disabled");
    {
        const std::filesystem::path d2 = temp_b / "case2";
        make_fake_yt_and_qjs(d2, true);
        PluginConfig c;
        c.bin_dir = d2.string();
        c.auto_download_extractor = true;
        c.auto_update_extractor = false;
        c.enable_js_runtime = true;
        c.js_runtime = "auto";

        std::atomic<int> dl_calls{0};
        BinaryManager bm_unsup(c);
        bm_unsup.SetDenoSupportedOverrideForTesting(0);
        bm_unsup.SetDenoDownloadHookForTesting([&](const std::string&, const std::string&, const std::atomic<bool>*, std::string&) {
            dl_calls.fetch_add(1);
            return false;
        });
        ThreadPool pool(1);
        bm_unsup.InitializeAsync(pool);
        TEST_CHECK(bm_unsup.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm_unsup.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
        TEST_CHECK(dl_calls.load() == 0);
        TEST_CHECK(bm_unsup.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
        pool.Stop();

        // Forced "quickjs" even with Deno binary present: does NOT probe/select Deno.
        write_fake_deno(bm_unsup.GetDenoPath());
        c.js_runtime = "quickjs";
        BinaryManager bm_fqjs(c);
        bm_fqjs.SetDenoSupportedOverrideForTesting(1);
        bm_fqjs.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& v, std::string&) { v = "2.9.8"; return true; });
        ThreadPool pool2(1);
        bm_fqjs.InitializeAsync(pool2);
        TEST_CHECK(bm_fqjs.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm_fqjs.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
        TEST_CHECK(bm_fqjs.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
        pool2.Stop();

        // Forced "deno" without Deno installed: warns & falls back to QuickJS.
        std::filesystem::remove_all(d2 / "deno", bec);
        c.js_runtime = "deno";
        c.auto_download_extractor = false;
        BinaryManager bm_nodeno(c);
        bm_nodeno.SetDenoSupportedOverrideForTesting(1);
        clear_logs();
        ThreadPool pool3(1);
        bm_nodeno.InitializeAsync(pool3);
        TEST_CHECK(bm_nodeno.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm_nodeno.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
        TEST_CHECK(find_log(LogSeverity::Warning, "js_runtime='deno'"));
        pool3.Stop();

        // enable_js_runtime=false -> no --js-runtimes passed.
        c.enable_js_runtime = false;
        BinaryManager bm_off(c);
        ThreadPool pool4(1);
        bm_off.InitializeAsync(pool4);
        TEST_CHECK(bm_off.WaitUntilSettled(std::chrono::seconds(2)));
        MetadataCache mc(8, 60);
        StreamResolver r_off(c, bm_off, mc);
        std::vector<std::string> off_args;
        r_off.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) {
            off_args = args;
            out_stdout = "{\"id\":\"voff\",\"url\":\"https://example.com/off.webm\"}";
            return true;
        });
        TEST_CHECK(r_off.Resolve("https://www.youtube.com/watch?v=off").success);
        TEST_CHECK(!args_contain(off_args, "--js-runtimes"));
        pool4.Stop();
    }

    // 4. Health check failure marks Deno unavailable in extractor_state.json and skips re-download on next boot.
    TEST_PHASE("Part B.5: Deno health check failure persists across boots");
    {
        const std::filesystem::path d3 = temp_b / "case3_health";
        make_fake_yt_and_qjs(d3, true);
        PluginConfig c;
        c.bin_dir = d3.string();
        c.auto_download_extractor = true;
        c.auto_update_extractor = false;
        c.enable_js_runtime = true;
        c.js_runtime = "auto";

        std::atomic<int> dl_calls{0}, health_calls{0};
        {
            BinaryManager bm1(c);
            bm1.SetDenoSupportedOverrideForTesting(1);
            bm1.SetDenoDownloadHookForTesting([&](const std::string& ep, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                write_fake_deno(ep);
                return true;
            });
            bm1.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string&, std::string& err) {
                health_calls.fetch_add(1);
                err = "GLIBC_2.34 not found";
                return false;
            });
            clear_logs();
            ThreadPool p1(1);
            bm1.InitializeAsync(p1);
            TEST_CHECK(bm1.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm1.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
            TEST_CHECK(!bm1.IsDenoReady());
            TEST_CHECK(bm1.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 1);
            TEST_CHECK(find_log(LogSeverity::Warning, "Deno health check failed"));
            p1.Stop();
        }
        // Boot 2 with same pinned version must NOT re-download or re-run health check.
        {
            std::filesystem::remove_all(d3 / "deno", bec);
            BinaryManager bm2(c);
            bm2.SetDenoSupportedOverrideForTesting(1);
            bm2.SetDenoDownloadHookForTesting([&](const std::string&, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                return false;
            });
            bm2.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string&, std::string&) {
                health_calls.fetch_add(1);
                return false;
            });
            ThreadPool p2(1);
            bm2.InitializeAsync(p2);
            TEST_CHECK(bm2.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm2.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 1);
            TEST_CHECK(!bm2.IsDenoReady());
            TEST_CHECK(bm2.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
            p2.Stop();
        }
    }

    // 5. Deno runtime failure fallback + 3 consecutive failures disable Deno for session.
    TEST_PHASE("Part B.5: Deno fallback & 3-strike disable");
    {
        const std::filesystem::path d4 = temp_b / "case4_fallback";
        make_fake_yt_and_qjs(d4, true);
        PluginConfig c;
        c.bin_dir = d4.string();
        c.auto_download_extractor = false;
        c.auto_update_extractor = false;
        c.enable_js_runtime = true;
        c.js_runtime = "auto";

        BinaryManager bm(c);
        bm.SetDenoSupportedOverrideForTesting(1);
        write_fake_deno(bm.GetDenoPath());
        bm.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& ver, std::string&) {
            ver = "2.9.8";
            return true;
        });

        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsDenoReady());

        MetadataCache mc(16, 60);
        StreamResolver resolver(c, bm, mc);
        int deno_attempts = 0;
        int qjs_attempts = 0;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            if (args_contain(args, "deno:")) {
                ++deno_attempts;
                out_stderr = "ERROR: [youtube] deno: challenge solver execution failed";
                return false;
            }
            if (args_contain(args, "quickjs:")) {
                ++qjs_attempts;
                out_stdout = "{\"id\":\"fb\",\"format_id\":\"251\",\"url\":\"https://example.com/ok.webm\"}";
                return true;
            }
            return false;
        });

        clear_logs();
        for (int i = 0; i < 3; ++i) {
            ResolveResult r = resolver.Resolve("https://www.youtube.com/watch?v=fb" + std::to_string(i));
            TEST_CHECK(r.success);
        }
        TEST_CHECK(deno_attempts == 3);
        TEST_CHECK(qjs_attempts == 3);
        TEST_CHECK(!bm.IsDenoReady());
        TEST_CHECK(find_log(LogSeverity::Warning, "3 consecutive runtime failures"));

        // 4th resolve skips Deno completely and uses QuickJS directly.
        ResolveResult r4 = resolver.Resolve("https://www.youtube.com/watch?v=fb4");
        TEST_CHECK(r4.success);
        TEST_CHECK(deno_attempts == 3);
        TEST_CHECK(qjs_attempts == 4);
        pool.Stop();
    }

    // Real subprocess env override check + cache dir preservation for deno/ & deno-cache/
    TEST_PHASE("Part B.2/5: real subprocess env override & cache preservation");
    {
        std::string sub_out, sub_err;
        std::vector<std::pair<std::string, std::string>> envs = {
            {"DENO_NO_UPDATE_CHECK", "1"},
            {"DENO_DIR", (temp_b / "custom-deno-cache").string()}
        };
#ifdef _WIN32
        wchar_t sys_dir[MAX_PATH]{};
        UINT slen = GetSystemDirectoryW(sys_dir, MAX_PATH);
        std::filesystem::path cmd_path = (slen > 0 && slen < MAX_PATH)
                                             ? (std::filesystem::path(sys_dir) / L"cmd.exe")
                                             : std::filesystem::path(L"C:\\Windows\\System32\\cmd.exe");
        const bool env_ok = RunSubprocessWithDiag(
            "Round42 Part B.2/5 env override",
            cmd_path.string(),
            {"/c", "echo %DENO_NO_UPDATE_CHECK%/%DENO_DIR%"},
            sub_out,
            sub_err,
            5000,
            &envs);
        TEST_CHECK(env_ok);
#else
        const bool env_ok = RunSubprocessWithDiag(
            "Round42 Part B.2/5 env override",
            "/bin/sh",
            {"-c", "printf '%s/%s' \"$DENO_NO_UPDATE_CHECK\" \"$DENO_DIR\""},
            sub_out,
            sub_err,
            5000,
            &envs);
        TEST_CHECK(env_ok);
#endif
        TEST_CHECK(sub_out.find("1/") != std::string::npos);
        TEST_CHECK(sub_out.find("custom-deno-cache") != std::string::npos);

        const std::filesystem::path prune_root = temp_b / "prune_check";
        const std::filesystem::path deno_dir = prune_root / "deno";
        const std::filesystem::path deno_cache = prune_root / "deno-cache";
        std::filesystem::create_directories(deno_dir, bec);
        std::filesystem::create_directories(deno_cache, bec);
        { std::ofstream f1(deno_dir / "keep.txt"); f1 << "ok"; }
        { std::ofstream f2(deno_cache / "cache.db"); f2 << "ok"; }
        std::unordered_set<std::string> unknown_dirs;
        TEST_CHECK(StreamResolver::PruneYtDlpCacheDir(prune_root.string(), std::chrono::hours(1), 1024, std::chrono::seconds(0), &unknown_dirs) == 0);
        TEST_CHECK(std::filesystem::exists(deno_dir / "keep.txt"));
        TEST_CHECK(std::filesystem::exists(deno_cache / "cache.db"));
        TEST_CHECK(unknown_dirs.empty());
    }

    std::filesystem::remove_all(temp_b, bec);
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound42HttpDiagnosticsAndDenoRuntime\n";
}

void TestRound43WarningsUpgradeAndHealthTimeout() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto count_logs = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        int count = 0;
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                ++count;
            }
        }
        return count;
    };
    auto any_log_count = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        return logs.size();
    };
    auto any_log_contains = [&](const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r43_tests";
    std::error_code tec;
    std::filesystem::remove_all(temp_root, tec);
    std::filesystem::create_directories(temp_root, tec);

    auto make_fake_bins = [&](const std::filesystem::path& dir, bool with_deno) {
        std::filesystem::create_directories(dir, tec);
#ifdef _WIN32
        const std::filesystem::path yt = dir / "yt-dlp.exe";
        const std::filesystem::path qjs = dir / "qjs.exe";
        const std::filesystem::path deno = dir / "deno" / "deno.exe";
#else
        const std::filesystem::path yt = dir / "yt-dlp";
        const std::filesystem::path qjs = dir / "qjs";
        const std::filesystem::path deno = dir / "deno" / "deno";
#endif
        { std::ofstream f(yt, std::ios::binary); f << std::string(128 * 1024, 'y'); }
        { std::ofstream f(qjs, std::ios::binary); f << std::string(64 * 1024, 'q'); }
        if (with_deno) {
            std::filesystem::create_directories(deno.parent_path(), tec);
            std::ofstream f(deno, std::ios::binary);
            f << std::string(64 * 1024, 'd');
        }
    };
    auto args_contain = [](const std::vector<std::string>& args, const std::string& needle) {
        for (const auto& a : args) {
            if (a.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    // 1. Ajuste 1: Resolve/ExtractPlaylist drop --no-warnings; Search keeps --no-warnings.
    TEST_PHASE("Ajuste 1: warnings enabled on Resolve/ExtractPlaylist and Deno warning retry");
    {
        const std::filesystem::path d1 = temp_root / "a1_deno";
        make_fake_bins(d1, true);
        PluginConfig cfg;
        cfg.bin_dir = d1.string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";
        cfg.debug_logging = false;
        Logger::Instance().SetDebugEnabled(false);

        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& ver, std::string&) {
            ver = "2.9.8";
            return true;
        });
        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsDenoReady());

        MetadataCache mc(16, 60);
        StreamResolver resolver(cfg, bm, mc);

        std::vector<std::string> last_resolve_args, last_search_args, last_pl_args;
        int deno_calls = 0, qjs_calls = 0;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            if (args_contain(args, "--flat-playlist")) {
                if (args_contain(args, "--playlist-end")) {
                    last_pl_args = args;
                    out_stdout = "{\"entries\":[{\"url\":\"https://www.youtube.com/watch?v=pl1\"}]}";
                } else {
                    last_search_args = args;
                    out_stdout = "{\"entries\":[{\"id\":\"s1\",\"title\":\"Song\",\"url\":\"https://www.youtube.com/watch?v=s1\"}]}";
                }
                return true;
            }
            last_resolve_args = args;
            if (args_contain(args, "deno:")) {
                ++deno_calls;
                out_stderr = "WARNING: [youtube] Vid123: n challenge solving failed: Some formats may be missing\n";
                out_stdout = "{\"id\":\"Vid123\",\"format_id\":\"18\",\"url\":\"https://example.com/bad_deno.mp4\"}";
                return true;
            }
            if (args_contain(args, "quickjs:")) {
                ++qjs_calls;
                out_stdout = "{\"id\":\"Vid123\",\"format_id\":\"251\",\"url\":\"https://example.com/good_qjs.webm\"}";
                return true;
            }
            return false;
        });

        TEST_CHECK(resolver.Search("test query", 5).success);
        TEST_CHECK(args_contain(last_search_args, "--no-warnings"));
        TEST_CHECK(resolver.ExtractPlaylist("https://www.youtube.com/playlist?list=PL1", 5).success);
        TEST_CHECK(!args_contain(last_pl_args, "--no-warnings"));

        // Deno returns ok=true with JS challenge warning -> retries once with QuickJS and takes QuickJS result.
        // Repeating 3 times triggers RecordDenoRuntimeFailure() x3 -> 4th goes straight to QuickJS.
        clear_logs();
        for (int i = 0; i < 3; ++i) {
            ResolveResult rr = resolver.Resolve("https://www.youtube.com/watch?v=WarnRetry" + std::to_string(i));
            TEST_CHECK(rr.success);
            TEST_CHECK(!args_contain(last_resolve_args, "--no-warnings"));
            TEST_CHECK(rr.metadata.direct_media_url == "https://example.com/good_qjs.webm");
            TEST_CHECK(rr.metadata.format_id == "251");
        }
        TEST_CHECK(deno_calls == 3);
        TEST_CHECK(qjs_calls == 3);
        TEST_CHECK(!bm.IsDenoReady());
        TEST_CHECK(count_logs(LogSeverity::Warning, "3 consecutive runtime failures") == 1);

        ResolveResult r4 = resolver.Resolve("https://www.youtube.com/watch?v=WarnRetryAfterDisable");
        TEST_CHECK(r4.success);
        TEST_CHECK(deno_calls == 3);
        TEST_CHECK(qjs_calls == 4);
        pool.Stop();
    }

    // 2. Ajuste 1: QuickJS + JS challenge warning -> max 1 Warn in 10 min; unrelated warning + debug off -> 0 logs, 0 retry.
    TEST_PHASE("Ajuste 1: QuickJS throttled JS warning, unrelated warning silent, debug query strip");
    {
        const std::filesystem::path d2 = temp_root / "a1_qjs";
        make_fake_bins(d2, true);
        PluginConfig cfg;
        cfg.bin_dir = d2.string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";
        cfg.debug_logging = false;
        Logger::Instance().SetDebugEnabled(false);

        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& ver, std::string&) {
            ver = "2.9.8";
            return true;
        });
        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsDenoReady());

        MetadataCache mc(16, 60);
        StreamResolver resolver(cfg, bm, mc);
        int call_count = 0;
        std::string fake_stderr = "WARNING: [youtube] Falling back to generic n function search\n";
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            ++call_count;
            out_stderr = fake_stderr;
            out_stdout = "{\"id\":\"ok\",\"format_id\":\"251\",\"url\":\"https://example.com/ok.webm\"}";
            return true;
        });

        // Unrelated warning with Deno active + debug_logging=false -> 0 logs, 0 retries, Deno remains ready.
        clear_logs();
        TEST_CHECK(resolver.Resolve("https://www.youtube.com/watch?v=unrelated1").success);
        TEST_CHECK(call_count == 1);
        TEST_CHECK(bm.IsDenoReady());
        TEST_CHECK(any_log_count() == 0);
        pool.Stop();

        // Force QuickJS & test JS challenge warning emitted across 3 resolutions -> single Warn.
        PluginConfig cfg_q = cfg;
        cfg_q.js_runtime = "quickjs";
        BinaryManager bm_q(cfg_q);
        ThreadPool pool_q(1);
        bm_q.InitializeAsync(pool_q);
        TEST_CHECK(bm_q.WaitUntilSettled(std::chrono::seconds(2)));

        MetadataCache mc_q(16, 60);
        StreamResolver resolver_q(cfg_q, bm_q, mc_q);
        fake_stderr = "WARNING: [youtube] abc: n challenge solving failed: Some formats may be missing\n";
        int q_calls = 0;
        resolver_q.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            ++q_calls;
            out_stderr = fake_stderr;
            out_stdout = "{\"id\":\"q\",\"format_id\":\"251\",\"url\":\"https://example.com/q.webm\"}";
            return true;
        });

        clear_logs();
        for (int i = 0; i < 3; ++i) {
            TEST_CHECK(resolver_q.Resolve("https://www.youtube.com/watch?v=qjs_warn_" + std::to_string(i)).success);
        }
        TEST_CHECK(q_calls == 3);
        TEST_CHECK(count_logs(LogSeverity::Warning, "yt-dlp could not solve the YouTube JS challenge") == 1);

        // Verify debug_logging strips query strings and caps line to <= 200 chars.
        PluginConfig cfg_dbg = cfg_q;
        cfg_dbg.debug_logging = true;
        Logger::Instance().SetDebugEnabled(true);
        StreamResolver resolver_dbg(cfg_dbg, bm_q, mc_q);
        resolver_dbg.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            out_stderr = "WARNING: Failed script https://www.youtube.com/s/player/123456/base.js?secret=XYZ&token=ABC " +
                         std::string(240, 'x') + "\n";
            out_stdout = "{\"id\":\"dbg\",\"format_id\":\"251\",\"url\":\"https://example.com/dbg.webm\"}";
            return true;
        });
        clear_logs();
        TEST_CHECK(resolver_dbg.Resolve("https://www.youtube.com/watch?v=dbg_strip_url").success);
        TEST_CHECK(any_log_contains("https://www.youtube.com/s/player/123456/base.js"));
        TEST_CHECK(!any_log_contains("secret=XYZ"));
        {
            Logger::Instance().Flush();
            std::lock_guard<std::mutex> lk(log_mtx);
            bool found_capped_warning = false;
            for (const auto& kv : logs) {
                if (kv.first == LogSeverity::Debug && kv.second.rfind("WARNING: ", 0) == 0) {
                    TEST_CHECK(kv.second.size() == 200);
                    found_capped_warning = true;
                }
            }
            TEST_CHECK(found_capped_warning);
        }
        Logger::Instance().SetDebugEnabled(false);
        pool_q.Stop();
    }

    // 3. Ajuste 1: ExecuteSubprocess 64 KB stderr cap without blocking.
    TEST_PHASE("Ajuste 1: ExecuteSubprocess stderr 64 KB cap");
    {
        const std::string self_exe = GetSelfExecutablePath();
        TEST_CHECK(!self_exe.empty());
        std::string sub_out, sub_err;
        const bool cap_ok = RunSubprocessWithDiag(
            "Round43 stderr 64 KB cap",
            self_exe,
            {"--child-stderr", "80000"},
            sub_out,
            sub_err,
            10000);
        TEST_CHECK(cap_ok);
        TEST_CHECK(sub_err.size() == 64 * 1024);
    }

    // 4. Ajuste 2: Outdated Deno binary triggers one background download while old Deno stays active.
    TEST_PHASE("Ajuste 2: pinned Deno version upgrade keeps old Deno active during download");
    {
        const std::filesystem::path d3 = temp_root / "a2_upgrade";
        make_fake_bins(d3, true);
        PluginConfig cfg;
        cfg.bin_dir = d3.string();
        cfg.auto_download_extractor = true;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";
        cfg.log_extractor_status = true;

        std::atomic<int> dl_calls{0};
        std::atomic<bool> allow_upgrade{false};
        std::atomic<bool> upgraded{false};

        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string& ver, std::string&) {
            ver = upgraded.load() ? "2.9.8" : "2.5.0";
            return true;
        });
        bm.SetDenoDownloadHookForTesting([&](const std::string& exe_p, const std::string&, const std::atomic<bool>*, std::string&) {
            dl_calls.fetch_add(1);
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!allow_upgrade.load() && std::chrono::steady_clock::now() < dl) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::ofstream f(exe_p, std::ios::binary);
            f << std::string(64 * 1024, 'n');
            upgraded.store(true);
            return true;
        });

        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        auto dl_start_wait = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (dl_calls.load() == 0 && std::chrono::steady_clock::now() < dl_start_wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Old Deno must remain active BEFORE and DURING the background upgrade download!
        TEST_CHECK(dl_calls.load() == 1);
        TEST_CHECK(bm.IsReady());
        TEST_CHECK(bm.IsDenoReady());
        TEST_CHECK(bm.SelectJsRuntime().kind == JsRuntimeKind::Deno);
        TEST_CHECK(bm.GetDenoVersion() == "2.5.0");

        allow_upgrade.store(true);
        TEST_CHECK(bm.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(3)));
        TEST_CHECK(dl_calls.load() == 1);
        TEST_CHECK(bm.IsDenoReady());
        TEST_CHECK(bm.GetDenoVersion() == "2.9.8");
        pool.Stop();
    }

    // 5. Ajuste 3: Pre-settle health timeout does not persist deno_failed_version; retried in EnsureDenoBackground.
    TEST_PHASE("Ajuste 3: pre-settle Deno timeout falls back without state failure and retries in background");
    {
        const std::filesystem::path d4 = temp_root / "a3_timeout";
        make_fake_bins(d4, true);
        PluginConfig cfg;
        cfg.bin_dir = d4.string();
        cfg.auto_download_extractor = true;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";

        std::atomic<int> health_calls{0};
        std::atomic<int> dl_calls{0};
        std::atomic<bool> allow_bg_probe{false};

        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string& ver, std::string& err) {
            int n = health_calls.fetch_add(1) + 1;
            if (n == 1) {
                err = "Extractor timed out";
                return false;
            }
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!allow_bg_probe.load() && std::chrono::steady_clock::now() < dl) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ver = "2.9.8";
            return true;
        });
        bm.SetDenoDownloadHookForTesting([&](const std::string&, const std::string&, const std::atomic<bool>*, std::string&) {
            dl_calls.fetch_add(1);
            return false;
        });

        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        // Settled cleanly on QuickJS; no deno_failed_version persisted.
        TEST_CHECK(bm.IsReady());
        TEST_CHECK(!bm.IsDenoReady());
        TEST_CHECK(bm.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
        const std::filesystem::path state_path = d4 / "extractor_state.json";
        if (std::filesystem::exists(state_path)) {
            std::ifstream ifs(state_path);
            std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(txt.find("deno_failed_version") == std::string::npos);
        }

        // Background verification completes cleanly with 10s retry and activates Deno without re-download.
        allow_bg_probe.store(true);
        TEST_CHECK(bm.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(3)));
        TEST_CHECK(health_calls.load() == 2);
        TEST_CHECK(dl_calls.load() == 0);
        TEST_CHECK(bm.IsDenoReady());
        TEST_CHECK(bm.SelectJsRuntime().kind == JsRuntimeKind::Deno);
        pool.Stop();
    }

    std::filesystem::remove_all(temp_root, tec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound43WarningsUpgradeAndHealthTimeout\n";
}

void TestRound44DenoTimeoutNotPermanent() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r44_tests";
    std::error_code tec;
    std::filesystem::remove_all(temp_root, tec);
    std::filesystem::create_directories(temp_root, tec);

    auto make_base_bins = [&](const std::filesystem::path& dir) {
        std::filesystem::create_directories(dir, tec);
#ifdef _WIN32
        const std::filesystem::path yt = dir / "yt-dlp.exe";
        const std::filesystem::path qjs = dir / "qjs.exe";
#else
        const std::filesystem::path yt = dir / "yt-dlp";
        const std::filesystem::path qjs = dir / "qjs";
#endif
        { std::ofstream f(yt, std::ios::binary); f << std::string(128 * 1024, 'y'); }
        { std::ofstream f(qjs, std::ios::binary); f << std::string(64 * 1024, 'q'); }
    };
    auto write_fake_deno = [&](const std::string& exe_path) {
        std::filesystem::create_directories(std::filesystem::path(exe_path).parent_path(), tec);
        std::ofstream f(exe_path, std::ios::binary);
        f << std::string(64 * 1024, 'd');
    };

    // 1. Timeout after download retries once and NEVER writes deno_failed_version; next boot re-tests & succeeds.
    TEST_PHASE("post-download timeout retries once without permanent failure and re-tests on next boot");
    {
        const std::filesystem::path d1 = temp_root / "case_timeout";
        make_base_bins(d1);
        PluginConfig cfg;
        cfg.bin_dir = d1.string();
        cfg.auto_download_extractor = true;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";
        cfg.log_extractor_status = true;

        std::atomic<int> dl_calls{0}, health_calls{0};
        {
            BinaryManager bm1(cfg);
            bm1.SetDenoSupportedOverrideForTesting(1);
            bm1.SetPostDownloadRetryDelayMsForTesting(15);
            bm1.SetDenoDownloadHookForTesting([&](const std::string& ep, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                write_fake_deno(ep);
                return true;
            });
            bm1.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string&, std::string& err) {
                health_calls.fetch_add(1);
                err = "Extractor timed out";
                return false;
            });
            clear_logs();
            ThreadPool p1(1);
            bm1.InitializeAsync(p1);
            TEST_CHECK(bm1.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm1.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(3)));
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 2); // 1st 20s probe + 1 post-download retry
            TEST_CHECK(!bm1.IsDenoReady());
            TEST_CHECK(bm1.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
            TEST_CHECK(find_log(LogSeverity::Warning, "Deno health check timed out"));

            const std::filesystem::path state_path = d1 / "extractor_state.json";
            if (std::filesystem::exists(state_path)) {
                std::ifstream ifs(state_path);
                std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                TEST_CHECK(txt.find("deno_failed_version") == std::string::npos);
            }
            p1.Stop();
        }
        // Boot 2: existing Deno is tested again without re-download and becomes ready.
        {
            BinaryManager bm2(cfg);
            bm2.SetDenoSupportedOverrideForTesting(1);
            bm2.SetDenoDownloadHookForTesting([&](const std::string&, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                return false;
            });
            bm2.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string& ver, std::string&) {
                health_calls.fetch_add(1);
                ver = "2.9.8";
                return true;
            });
            ThreadPool p2(1);
            bm2.InitializeAsync(p2);
            TEST_CHECK(bm2.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm2.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 3);
            TEST_CHECK(bm2.IsDenoReady());
            TEST_CHECK(bm2.SelectJsRuntime().kind == JsRuntimeKind::Deno);
            p2.Stop();
        }
    }

    // 2. Post-download timeout recovers immediately on single background retry.
    TEST_PHASE("post-download timeout recovers on second probe");
    {
        const std::filesystem::path d2 = temp_root / "case_recover";
        make_base_bins(d2);
        PluginConfig cfg;
        cfg.bin_dir = d2.string();
        cfg.auto_download_extractor = true;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";

        std::atomic<int> health_calls{0};
        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetPostDownloadRetryDelayMsForTesting(15);
        bm.SetDenoDownloadHookForTesting([&](const std::string& ep, const std::string&, const std::atomic<bool>*, std::string&) {
            write_fake_deno(ep);
            return true;
        });
        bm.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string& ver, std::string& err) {
            if (health_calls.fetch_add(1) == 0) {
                err = "Extractor timed out";
                return false;
            }
            ver = "2.9.8";
            return true;
        });
        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(3)));
        TEST_CHECK(health_calls.load() == 2);
        TEST_CHECK(bm.IsDenoReady());
        TEST_CHECK(bm.GetDenoVersion() == "2.9.8");
        pool.Stop();
    }

    // 3. Real failure (exit code 1) records deno_failed_version immediately and skips download on next boot.
    TEST_PHASE("real exit-code failure records deno_failed_version and skips next boot");
    {
        const std::filesystem::path d3 = temp_root / "case_real_fail";
        make_base_bins(d3);
        PluginConfig cfg;
        cfg.bin_dir = d3.string();
        cfg.auto_download_extractor = true;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";

        std::atomic<int> dl_calls{0}, health_calls{0};
        {
            BinaryManager bm1(cfg);
            bm1.SetDenoSupportedOverrideForTesting(1);
            bm1.SetPostDownloadRetryDelayMsForTesting(15);
            bm1.SetDenoDownloadHookForTesting([&](const std::string& ep, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                write_fake_deno(ep);
                return true;
            });
            bm1.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string&, std::string& err) {
                health_calls.fetch_add(1);
                err = "deno --version failed with exit code 1";
                return false;
            });
            clear_logs();
            ThreadPool p1(1);
            bm1.InitializeAsync(p1);
            TEST_CHECK(bm1.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm1.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 1); // No 30s retry on real execution error
            TEST_CHECK(!bm1.IsDenoReady());
            TEST_CHECK(find_log(LogSeverity::Warning, "Deno health check failed"));

            const std::filesystem::path state_path = d3 / "extractor_state.json";
            TEST_CHECK(std::filesystem::exists(state_path));
            std::ifstream ifs(state_path);
            std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(txt.find("\"deno_failed_version\"") != std::string::npos);
            p1.Stop();
        }
        {
            std::filesystem::remove_all(d3 / "deno", tec);
            BinaryManager bm2(cfg);
            bm2.SetDenoSupportedOverrideForTesting(1);
            bm2.SetDenoDownloadHookForTesting([&](const std::string&, const std::string&, const std::atomic<bool>*, std::string&) {
                dl_calls.fetch_add(1);
                return false;
            });
            bm2.SetDenoHealthCheckHookForTesting([&](const std::string&, std::string&, std::string&) {
                health_calls.fetch_add(1);
                return false;
            });
            ThreadPool p2(1);
            bm2.InitializeAsync(p2);
            TEST_CHECK(bm2.WaitUntilSettled(std::chrono::seconds(2)));
            TEST_CHECK(bm2.WaitForBackgroundProvisioningForTesting(std::chrono::seconds(2)));
            TEST_CHECK(dl_calls.load() == 1);
            TEST_CHECK(health_calls.load() == 1);
            TEST_CHECK(!bm2.IsDenoReady());
            TEST_CHECK(bm2.SelectJsRuntime().kind == JsRuntimeKind::QuickJs);
            p2.Stop();
        }
    }

    std::filesystem::remove_all(temp_root, tec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound44DenoTimeoutNotPermanent\n";
}

void TestRound45WinHttpCookiesFreshSessionAndAb() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    auto any_log_contains = [&](const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    HttpSecurityPolicy sec;
    sec.allow_private_network_urls = true;
    HttpClient::SetSecurityPolicy(sec);
    HttpClient::ResetWinHttpSessionCountersForTesting();

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const std::string valid_wav = BuildTestWavBytes(16000, 1200);

    std::atomic<bool> stop_srv{false};
    std::atomic<bool> saw_cookie_on_second_req{false};
    std::atomic<int> ab403_hits{0};
    std::atomic<int> fresh_hits{0};
    std::mutex workers_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(workers_mtx);
            workers.emplace_back([sock, &valid_wav, &saw_cookie_on_second_req, &ab403_hits, &fresh_hits]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream is(raw_req);
                std::string method, path;
                is >> method >> path;

                std::string lower_req = raw_req;
                for (char& ch : lower_req) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const bool has_cookie_hdr = (lower_req.find("\ncookie:") != std::string::npos);

                std::string hdr, body;
                if (path == "/cookie_set") {
                    body = "cookie1";
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nSet-Cookie: x=1; Path=/\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/cookie_check") {
                    if (has_cookie_hdr) {
                        saw_cookie_on_second_req.store(true);
                    }
                    body = "ok";
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/ab_403") {
                    ab403_hits.fetch_add(1);
                    body = "Forbidden_403";
                    hdr = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/fresh_wav") {
                    fresh_hits.fetch_add(1);
                    body = valid_wav;
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else {
                    hdr = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                }
                asio::error_code wec;
                asio::write(*sock, asio::buffer(hdr), wec);
                if (!body.empty() && !wec) {
                    asio::write(*sock, asio::buffer(body), wec);
                }
            });
        }
    });

    // 1. Shared session must NOT store or replay Set-Cookie across requests.
    TEST_PHASE("WINHTTP_DISABLE_COOKIES prevents automatic cookie replay on shared session");
    {
        HttpResponse r1 = HttpClient::Get(base + "/cookie_set", {}, 5000);
        TEST_CHECK(r1.Ok());
        HttpResponse r2 = HttpClient::Get(base + "/cookie_check", {}, 5000);
        TEST_CHECK(r2.Ok());
        TEST_CHECK(!saw_cookie_on_second_req.load());
    }

    // 2. debug_logging ON: A/B probes fire once, sent headers logged safely (no cookie secret value), re-resolve uses fresh session.
    TEST_PHASE("debug_logging ON logs A/B diagnostics and sent headers, re-resolve uses fresh session");
    {
        ab403_hits.store(0);
        fresh_hits.store(0);
        HttpClient::ResetWinHttpSessionCountersForTesting();
        Logger::Instance().SetDebugEnabled(true);
        clear_logs();

        TrackMetadata meta;
        meta.id = "EtTbS-KdcrE";
        meta.original_url = "https://www.youtube.com/watch?v=EtTbS-KdcrE";
        meta.direct_media_url = base + "/ab_403";
        meta.source_type = SourceType::ExtractorStream;
        meta.container_ext = "wav";
        meta.codec = "pcm_s16le";
        meta.format_id = "251";
        meta.resolved_at = std::chrono::steady_clock::now() - std::chrono::seconds(15);
        meta.http_headers["User-Agent"] = "TestAgent/45.0";
        meta.http_headers["Cookie"] = "SUPER_SECRET_COOKIE=VAL45";
        meta.http_headers["X-Custom-Header"] = "SecretHeaderVal45";

        std::atomic<int> rr_calls{0};
        TranscoderConfig tcfg;
        tcfg.re_resolve_callback = [&](const TrackMetadata&, const std::atomic<bool>&, TrackMetadata& out_new, std::string&) {
            rr_calls.fetch_add(1);
            out_new = meta;
            out_new.direct_media_url = base + "/fresh_wav";
            out_new.resolved_at = std::chrono::steady_clock::now();
            return true;
        };

        auto out_buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, out_buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(rr_calls.load() == 1);
        TEST_CHECK(fresh_hits.load() == 1);
        // 1 initial 403 + 2 A/B diagnostic probes = 3 hits to /ab_403
        TEST_CHECK(ab403_hits.load() == 3);
        TEST_CHECK(find_log(LogSeverity::Debug, "HTTP 403 A/B: shared_session_no_headers=403, fresh_session_same_headers=403"));
#ifdef _WIN32
        TEST_CHECK(find_log(LogSeverity::Debug, "reused_conn="));
        TEST_CHECK(find_log(LogSeverity::Debug, "HTTP request headers sent:"));
        TEST_CHECK(find_log(LogSeverity::Debug, "User-Agent=\"TestAgent/45.0\""));
        TEST_CHECK(find_log(LogSeverity::Debug, "Cookie=<present>"));
        TEST_CHECK(find_log(LogSeverity::Debug, "X-Custom-Header"));
        TEST_CHECK(HttpClient::GetWinHttpFreshSessionCountForTesting() == 2); // 1 for A/B fresh probe + 1 for re-resolve
#endif
        TEST_CHECK(!any_log_contains("SUPER_SECRET_COOKIE"));
        TEST_CHECK(!any_log_contains("SecretHeaderVal45"));
    }

    // 3. debug_logging OFF: zero extra A/B requests, re-resolve still runs via fresh session.
    TEST_PHASE("debug_logging OFF skips A/B probes and re-resolve uses fresh session");
    {
        ab403_hits.store(0);
        fresh_hits.store(0);
        HttpClient::ResetWinHttpSessionCountersForTesting();
        Logger::Instance().SetDebugEnabled(false);
        clear_logs();

        TrackMetadata meta;
        meta.id = "EtTbS-KdcrE";
        meta.original_url = "https://www.youtube.com/watch?v=EtTbS-KdcrE";
        meta.direct_media_url = base + "/ab_403";
        meta.source_type = SourceType::ExtractorStream;
        meta.container_ext = "wav";
        meta.codec = "pcm_s16le";
        meta.format_id = "251";
        meta.resolved_at = std::chrono::steady_clock::now() - std::chrono::seconds(15);

        std::atomic<int> rr_calls{0};
        TranscoderConfig tcfg;
        tcfg.re_resolve_callback = [&](const TrackMetadata&, const std::atomic<bool>&, TrackMetadata& out_new, std::string&) {
            rr_calls.fetch_add(1);
            out_new = meta;
            out_new.direct_media_url = base + "/fresh_wav";
            out_new.resolved_at = std::chrono::steady_clock::now();
            return true;
        };

        auto out_buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        AudioTranscoder tr(meta, tcfg, out_buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err));
        TEST_CHECK(rr_calls.load() == 1);
        TEST_CHECK(fresh_hits.load() == 1);
        // Zero extra A/B requests when debug_logging is off!
        TEST_CHECK(ab403_hits.load() == 1);
        TEST_CHECK(!any_log_contains("A/B:"));
#ifdef _WIN32
        TEST_CHECK(HttpClient::GetWinHttpFreshSessionCountForTesting() == 1); // re-resolve session only
#endif
    }

    stop_srv.store(true);
    StopTestAcceptor(acceptor, port);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> wlk(workers_mtx);
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }
    HttpClient::SetSecurityPolicy({});
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound45WinHttpCookiesFreshSessionAndAb\n";
}

void TestRound46VerboseYtDlpPortraitDumpAndEnvDiag() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    auto any_log_contains = [&](const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r46_tests";
    std::error_code tec;
    std::filesystem::remove_all(temp_root, tec);
    std::filesystem::create_directories(temp_root / "bin", tec);

#ifdef _WIN32
    const std::filesystem::path fake_yt = temp_root / "bin" / "yt-dlp.exe";
    const std::filesystem::path fake_qjs = temp_root / "bin" / "qjs.exe";
    const std::filesystem::path fake_deno = temp_root / "bin" / "deno" / "deno.exe";
#else
    const std::filesystem::path fake_yt = temp_root / "bin" / "yt-dlp";
    const std::filesystem::path fake_qjs = temp_root / "bin" / "qjs";
    const std::filesystem::path fake_deno = temp_root / "bin" / "deno" / "deno";
#endif
    { std::ofstream f(fake_yt, std::ios::binary); f << std::string(128 * 1024, 'y'); }
    { std::ofstream f(fake_qjs, std::ios::binary); f << std::string(64 * 1024, 'q'); }
    std::filesystem::create_directories(fake_deno.parent_path(), tec);
    { std::ofstream f(fake_deno, std::ios::binary); f << std::string(64 * 1024, 'd'); }

    auto args_contain = [](const std::vector<std::string>& args, const std::string& needle) {
        for (const auto& a : args) {
            if (a == needle) return true;
        }
        return false;
    };

    // Parts 1, 2, 4a: -v on Resolve (not Search), filtered verbose lines without query strings, URL portrait, masked target command.
    TEST_PHASE("Part 1, 2 & 4a: verbose -v filtering, URL portrait & command line logging");
    {
        HttpSecurityPolicy sec;
        sec.allow_private_network_urls = true;
        sec.public_ip = "203.0.113.7";
        HttpClient::SetSecurityPolicy(sec);

        PluginConfig cfg;
        cfg.base_dir = temp_root.string();
        cfg.bin_dir = (temp_root / "bin").string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = true;
        cfg.js_runtime = "auto";
        cfg.debug_logging = true;
        Logger::Instance().SetDebugEnabled(true);

        BinaryManager bm(cfg);
        bm.SetDenoSupportedOverrideForTesting(1);
        bm.SetDenoHealthCheckHookForTesting([](const std::string&, std::string& ver, std::string&) {
            ver = "2.9.8";
            return true;
        });
        ThreadPool pool(1);
        bm.InitializeAsync(pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsDenoReady());

        MetadataCache mc(16, 60);
        StreamResolver resolver(cfg, bm, mc);

        std::vector<std::string> last_resolve_args, last_search_args;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) {
            for (const auto& a : args) {
                if (a == "--flat-playlist") {
                    last_search_args = args;
                    out_stdout = "{\"entries\":[{\"id\":\"s1\",\"title\":\"S\",\"url\":\"https://example.com/s1\"}]}";
                    return true;
                }
            }
            last_resolve_args = args;
            out_stderr =
                "[debug] Command-line config: ['--ignore-config']\n"
                "[debug] JS runtimes: deno-2.9.8\n"
                "[debug] [youtube] Extracting URL: https://www.youtube.com/watch?v=EtTbS-KdcrE&secret_q=1\n"
                "[debug] [jsc] Solving JS challenges using deno with https://www.youtube.com/s/player/1/base.js?secret_p=XYZ\n"
                "[debug] Running deno\n"
                "[debug] Unrelated line should stay hidden\n";
            out_stdout =
                "{\"id\":\"EtTbS-KdcrE\",\"format_id\":\"251\",\"acodec\":\"opus\",\"ext\":\"webm\","
                "\"url\":\"http://127.0.0.1:8080/videoplayback?expire=999&ei=abc&ip=203.0.113.7&n=NNNNNN&lsig=L123&c=TVHTML5\"}";
            return true;
        });

        clear_logs();
        TEST_CHECK(resolver.Search("query46", 5).success);
        TEST_CHECK(!args_contain(last_search_args, "-v"));

        ResolveResult rr = resolver.Resolve("https://www.youtube.com/watch?v=EtTbS-KdcrE");
        TEST_CHECK(rr.success);
        TEST_CHECK(args_contain(last_resolve_args, "-v"));
        TEST_CHECK(rr.metadata.js_runtime_used == "deno");
        // [debug] JS/deno lines must NOT be treated as failure -> Deno remains ready!
        TEST_CHECK(bm.IsDenoReady());

        // Verbose filtered lines + URL query stripping + unrelated hidden
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp -v: [debug] JS runtimes: deno-2.9.8"));
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp -v: [debug] [jsc] Solving JS challenges using deno with https://www.youtube.com/s/player/1/base.js"));
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp -v: [debug] Running deno"));
        TEST_CHECK(!any_log_contains("secret_p=XYZ"));
        TEST_CHECK(!any_log_contains("Unrelated line should stay hidden"));

        // Media URL portrait + NO sensitive IP or parameter values
        TEST_CHECK(find_log(LogSeverity::Debug, "url_len="));
        TEST_CHECK(find_log(LogSeverity::Debug, "n_len=6"));
        TEST_CHECK(find_log(LogSeverity::Debug, "has_sig=false"));
        TEST_CHECK(find_log(LogSeverity::Debug, "has_lsig=true"));
        TEST_CHECK(find_log(LogSeverity::Debug, "ip_matches_public=true"));
        TEST_CHECK(find_log(LogSeverity::Debug, "params=expire,ei,ip,n,lsig,c"));
        TEST_CHECK(!any_log_contains("203.0.113.7"));
        TEST_CHECK(!any_log_contains("NNNNNN"));
        TEST_CHECK(!any_log_contains("L123"));

        // First-resolve command line logged with <target>
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp command line (first resolve):"));
        TEST_CHECK(find_log(LogSeverity::Debug, "-- <target>"));
        TEST_CHECK(!any_log_contains("-- https://www.youtube.com/watch?v=EtTbS-KdcrE"));

        // Second resolution must NOT repeat first-resolve command log and tests ip_matches_public=false.
        clear_logs();
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) {
            out_stdout =
                "{\"id\":\"Mismatch1\",\"format_id\":\"251\",\"acodec\":\"opus\",\"ext\":\"webm\","
                "\"url\":\"http://127.0.0.1:8080/videoplayback?ip=198.51.100.2&sig=SSSS&c=WEB\"}";
            return true;
        });
        TEST_CHECK(resolver.Resolve("https://www.youtube.com/watch?v=Mismatch1").success);
        TEST_CHECK(!find_log(LogSeverity::Debug, "yt-dlp command line (first resolve):"));
        TEST_CHECK(find_log(LogSeverity::Debug, "ip_matches_public=false"));
        TEST_CHECK(find_log(LogSeverity::Debug, "has_sig=true"));
        TEST_CHECK(find_log(LogSeverity::Debug, "n_len=0"));
        TEST_CHECK(!any_log_contains("198.51.100.2"));
        TEST_CHECK(!any_log_contains("SSSS"));

        // debug_logging OFF: -v must NOT be passed.
        PluginConfig cfg_off = cfg;
        cfg_off.debug_logging = false;
        Logger::Instance().SetDebugEnabled(false);
        StreamResolver resolver_off(cfg_off, bm, mc);
        std::vector<std::string> off_args;
        resolver_off.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) {
            off_args = args;
            out_stdout = "{\"id\":\"Off1\",\"url\":\"http://127.0.0.1:8080/off\"}";
            return true;
        });
        TEST_CHECK(resolver_off.Resolve("https://www.youtube.com/watch?v=Off1").success);
        TEST_CHECK(!args_contain(off_args, "-v"));
        pool.Stop();
    }

    // Part 1 & Part 4b/c: real ExecuteSubprocess >64 KB stderr streaming line filter + child env & process diag.
    TEST_PHASE("Part 1 & 4b/c: ExecuteSubprocess >64KB line filter + child env/process diag");
    {
        Logger::Instance().SetDebugEnabled(true);
        clear_logs();
        const std::string self_exe = GetSelfExecutablePath();
        TEST_CHECK(!self_exe.empty());
        std::vector<std::string> late_lines;
        std::string sub_out, sub_err;
        std::vector<std::pair<std::string, std::string>> envs = {
            {"DENO_DIR", "SUPER_SECRET_DENO_PATH_VAL"},
            {"YTDLP_TEST_VAR", "1"}
        };
        auto cb = [&](const std::string& ln) {
            if (ln.find("[debug] JS runtimes:") != std::string::npos) late_lines.push_back(ln);
        };
        const bool filter_ok = RunSubprocessWithDiag(
            "Round46 >64KB stderr line filter",
            self_exe,
            {"--child-stderr-lines", "360", "200", "[debug] JS runtimes: deno-late-after-72kb"},
            sub_out,
            sub_err,
            10000,
            &envs,
            cb,
            true);
        TEST_CHECK(filter_ok);
        TEST_CHECK(sub_err.size() == 64 * 1024);
        TEST_CHECK(late_lines.size() == 1);
        TEST_CHECK(late_lines[0].find("deno-late-after-72kb") != std::string::npos);
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp child env: cwd="));
        TEST_CHECK(find_log(LogSeverity::Debug, "DENO_DIR"));
        TEST_CHECK(find_log(LogSeverity::Debug, "YTDLP_TEST_VAR"));
        TEST_CHECK(!any_log_contains("SUPER_SECRET_DENO_PATH_VAL"));
#ifdef _WIN32
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp child process: job_object=true, inherited_handles=restricted_list(3:stdout_pipe,stderr_pipe,nul_stdin), stdin_redirected=true, stdin_target=NUL"));
#else
        TEST_CHECK(find_log(LogSeverity::Debug, "yt-dlp child process: job_object=false(pgid=setpgid)"));
#endif
    }

    // Part 3: Local rejected request file created ONLY when debug_logging is enabled, excluding Cookie & Authorization.
    TEST_PHASE("Part 3: last_rejected_media_request.txt gated on debug_logging and scrubbed");
    {
        asio::io_context io;
        asio::ip::tcp::acceptor acc(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const unsigned short port = acc.local_endpoint().port();
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        std::atomic<bool> stop_srv{false};
        std::vector<std::thread> workers;
        std::mutex wm;
        std::thread srv([&]() {
            while (!stop_srv.load()) {
                asio::error_code ec;
                acc.non_blocking(true, ec);
                auto sock = std::make_shared<asio::ip::tcp::socket>(io);
                acc.accept(*sock, ec);
                if (ec == asio::error::would_block || ec == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (ec) break;
                ConfigureTestSocketTimeouts(*sock);
                std::lock_guard<std::mutex> lk(wm);
                workers.emplace_back([sock]() {
                    asio::error_code rec;
                    asio::streambuf sb;
                    asio::read_until(*sock, sb, "\r\n\r\n", rec);
                    if (rec) return;
                    const std::string resp =
                        "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 3\r\nConnection: close\r\n\r\n403";
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(resp), wec);
                });
            }
        });

        const std::filesystem::path dump_dir = temp_root / "dump_case";
        const std::filesystem::path dump_file = dump_dir / "debug" / "last_rejected_media_request.txt";

        TrackMetadata meta;
        meta.id = "EtTbS-KdcrE";
        meta.direct_media_url = base + "/media?n=ABC&sig=SIG";
        meta.source_type = SourceType::ExtractorStream;
        meta.format_id = "251";
        meta.js_runtime_used = "deno";
        meta.resolved_at = std::chrono::steady_clock::now() - std::chrono::seconds(15);
        meta.http_headers["User-Agent"] = "UA46/1.0";
        meta.http_headers["Cookie"] = "DONT_DUMP_COOKIE=1";
        meta.http_headers["Authorization"] = "Bearer DONT_DUMP_AUTH";
        meta.http_headers["Accept-Language"] = "en-US";

        TranscoderConfig tcfg;
        tcfg.base_dir = dump_dir.string();

        // Off first: must NOT create file.
        Logger::Instance().SetDebugEnabled(false);
        {
            auto buf = std::make_shared<AudioStreamBuffer>(32 * 1024);
            std::atomic<bool> cancel{false}, pause{false};
            std::atomic<float> vol{100.0f};
            std::string err;
            AudioTranscoder tr(meta, tcfg, buf);
            TEST_CHECK(!tr.Run(cancel, pause, vol, err));
            TEST_CHECK(!std::filesystem::exists(dump_file));
        }

        // On second: MUST create/overwrite file with full details minus Cookie/Authorization, and log file path once.
        Logger::Instance().SetDebugEnabled(true);
        clear_logs();
        {
            auto buf = std::make_shared<AudioStreamBuffer>(32 * 1024);
            std::atomic<bool> cancel{false}, pause{false};
            std::atomic<float> vol{100.0f};
            std::string err;
            AudioTranscoder tr(meta, tcfg, buf);
            TEST_CHECK(!tr.Run(cancel, pause, vol, err));
            TEST_CHECK(std::filesystem::exists(dump_file));
            std::ifstream ifs(dump_file);
            std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(txt.find("timestamp: ") != std::string::npos);
            TEST_CHECK(txt.find("status: 403") != std::string::npos);
            TEST_CHECK(txt.find("format_id: 251") != std::string::npos);
            TEST_CHECK(txt.find("js_runtime: deno") != std::string::npos);
            TEST_CHECK(txt.find(meta.direct_media_url) != std::string::npos);
            TEST_CHECK(txt.find("User-Agent: UA46/1.0") != std::string::npos);
            TEST_CHECK(txt.find("Accept-Language: en-US") != std::string::npos);
            TEST_CHECK(txt.find("DONT_DUMP_COOKIE") == std::string::npos);
            TEST_CHECK(txt.find("DONT_DUMP_AUTH") == std::string::npos);
            TEST_CHECK(find_log(LogSeverity::Debug, "Saved rejected media request details to"));
        }

        stop_srv.store(true);
        StopTestAcceptor(acc, port);
        if (srv.joinable()) srv.join();
        std::lock_guard<std::mutex> lk(wm);
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    HttpClient::SetSecurityPolicy({});
    std::filesystem::remove_all(temp_root, tec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound46VerboseYtDlpPortraitDumpAndEnvDiag\n";
}

void TestRound47AvailableAtAndEarly403Retry() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r47_tests";
    std::error_code fec;
    std::filesystem::remove_all(temp_base, fec);
    std::filesystem::create_directories(temp_base / "bin", fec);
#ifdef _WIN32
    { std::ofstream f(temp_base / "bin" / "yt-dlp.exe", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#else
    { std::ofstream f(temp_base / "bin" / "yt-dlp", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#endif

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const std::string valid_wav = BuildTestWavBytes(16000, 1200);

    std::atomic<bool> stop_srv{false};
    std::atomic<int> avail_media_hits{0};
    std::atomic<int> early_no_avail_hits{0};
    std::atomic<int> implausible_hits{0};
    std::atomic<int> cancel_media_hits{0};
    std::atomic<int> preresolve_hits{0};
    std::atomic<bool> saw_pragma_no_cache{false};
    std::chrono::steady_clock::time_point gate_tp{};
    std::chrono::steady_clock::time_point early_gate_tp{};
    std::mutex srv_mtx;
    std::vector<std::chrono::steady_clock::time_point> avail_hit_times;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(srv_mtx);
            workers.emplace_back([&, sock]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream is(raw_req);
                std::string method, path;
                is >> method >> path;

                std::string lower_req = raw_req;
                for (char& ch : lower_req) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower_req.find("pragma: no-cache") != std::string::npos) {
                    saw_pragma_no_cache.store(true);
                }

                const auto now_tp = std::chrono::steady_clock::now();
                std::string hdr, body;
                if (path == "/avail_media") {
                    avail_media_hits.fetch_add(1);
                    {
                        std::lock_guard<std::mutex> lk(srv_mtx);
                        avail_hit_times.push_back(now_tp);
                    }
                    if (now_tp < gate_tp) {
                        body = "TooEarly403";
                        hdr = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    } else {
                        body = valid_wav;
                        hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    }
                } else if (path == "/early_no_avail") {
                    int n = early_no_avail_hits.fetch_add(1) + 1;
                    if (n == 1 && now_tp < early_gate_tp) {
                        body = "Early403";
                        hdr = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    } else {
                        body = valid_wav;
                        hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    }
                } else if (path == "/implausible") {
                    implausible_hits.fetch_add(1);
                    body = valid_wav;
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/cancel_media") {
                    cancel_media_hits.fetch_add(1);
                    hdr = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else if (path == "/preresolved_ok") {
                    preresolve_hits.fetch_add(1);
                    body = valid_wav;
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else {
                    hdr = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                }
                asio::error_code wec;
                asio::write(*sock, asio::buffer(hdr), wec);
                if (!body.empty() && !wec) {
                    asio::write(*sock, asio::buffer(body), wec);
                }
            });
        }
    });

    MockPlatformHost host;
    host.players = {
        {1, {0.0f, 0.0f, 0.0f}, 0, 0},
        {2, {0.0f, 0.0f, 0.0f}, 0, 0},
        {3, {0.0f, 0.0f, 0.0f}, 0, 0},
        {4, {0.0f, 0.0f, 0.0f}, 0, 0}
    };
    host.player_names = {
        {1, "Tester47A"},
        {2, "Tester47B"},
        {3, "Tester47C"},
        {4, "Tester47D"}
    };

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.worker_threads = 2;
    cfg.transcoder_burst_ms = 2000;
    cfg.debug_logging = true;
    Logger::Instance().SetDebugEnabled(true);

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }

    auto epoch_sec_offset = [](double sec) -> std::string {
        const double cur = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / 1000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << (cur + sec);
        return oss.str();
    };

    std::atomic<int> resolve_count{0};
    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string&,
        int,
        const std::atomic<bool>*) -> bool {
        const std::string& target = args.empty() ? std::string() : args.back();
        resolve_count.fetch_add(1);
        if (target.find("yt_avail_personal") != std::string::npos ||
            target.find("yt_avail_channel") != std::string::npos) {
            out_stdout =
                "{\"id\":\"AvailVid\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(1.2) +
                ",\"url\":\"" + base + "/avail_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_avail_cancel") != std::string::npos) {
            out_stdout =
                "{\"id\":\"CancelVid\",\"requested_formats\":[{\"format_id\":\"251\",\"acodec\":\"opus\",\"ext\":\"webm\",\"available_at\":" +
                epoch_sec_offset(1.4) + ",\"url\":\"" + base + "/cancel_media\"}]}";
            return true;
        }
        if (target.find("yt_avail_implausible") != std::string::npos) {
            out_stdout =
                "{\"id\":\"FarFuture\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(120.0) +
                ",\"url\":\"" + base + "/implausible\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_early_no_avail") != std::string::npos) {
            out_stdout =
                "{\"id\":\"NoAvail\",\"format_id\":\"251\",\"url\":\"" + base +
                "/early_no_avail\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_preresolve_past") != std::string::npos) {
            out_stdout =
                "{\"id\":\"PastAvail\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(-5.0) +
                ",\"url\":\"" + base + "/preresolved_ok\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_fast_other") != std::string::npos) {
            out_stdout =
                "{\"id\":\"FastOther\",\"format_id\":\"251\",\"url\":\"" + base +
                "/implausible\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        return false;
    });

    // Test 1: Personal stream respects available_at = now + 2s without blocking worker_threads=2.
    TEST_PHASE("Test 1: Personal stream waits until available_at + 300ms without blocking pool workers");
    {
        avail_media_hits.store(0);
        saw_pragma_no_cache.store(false);
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            avail_hit_times.clear();
        }
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.ready_stream_ids.clear();
            host.error_events.clear();
        }
        clear_logs();

        gate_tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        StreamId sid1 = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_avail_personal", StreamOptions{});
        TEST_CHECK(sid1 != INVALID_STREAM_ID);

        // Let extraction finish (~150ms), confirm NO OnAudioStreamReady yet and NO media hit,
        // and confirm 2 real PlayAudioStreamForPlayer streams on players 3 and 4 resolve via extraction_pool_
        // and become ready before player 1's gate_tp (worker_threads=2 is not blocked).
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            bool has_ready = std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamReady") !=
                             host.callback_names.end();
            TEST_CHECK(!has_ready);
        }
        TEST_CHECK(avail_media_hits.load() == 0);
        TEST_CHECK(find_log(LogSeverity::Debug, "available_in_ms="));

        StreamId sid3 = engine.PlayAudioStreamForPlayer(3, "https://www.youtube.com/watch?v=yt_fast_other_1", StreamOptions{});
        StreamId sid4 = engine.PlayAudioStreamForPlayer(4, "https://www.youtube.com/watch?v=yt_fast_other_2", StreamOptions{});
        TEST_CHECK(sid3 != INVALID_STREAM_ID);
        TEST_CHECK(sid4 != INVALID_STREAM_ID);

        std::chrono::steady_clock::time_point ready3_tp{}, ready4_tp{};
        auto pool_dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < pool_dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (ready3_tp == std::chrono::steady_clock::time_point{} &&
                    std::find(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid3) !=
                        host.ready_stream_ids.end()) {
                    ready3_tp = std::chrono::steady_clock::now();
                }
                if (ready4_tp == std::chrono::steady_clock::time_point{} &&
                    std::find(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid4) !=
                        host.ready_stream_ids.end()) {
                    ready4_tp = std::chrono::steady_clock::now();
                }
                TEST_CHECK(std::find(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid1) ==
                           host.ready_stream_ids.end());
            }
            if (ready3_tp != std::chrono::steady_clock::time_point{} &&
                ready4_tp != std::chrono::steady_clock::time_point{}) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(ready3_tp != std::chrono::steady_clock::time_point{} && ready3_tp < gate_tp);
        TEST_CHECK(ready4_tp != std::chrono::steady_clock::time_point{} && ready4_tp < gate_tp);
        TEST_CHECK(avail_media_hits.load() == 0);

        TEST_CHECK(engine.StopAudioStreamForPlayer(3));
        TEST_CHECK(engine.StopAudioStreamForPlayer(4));
        engine.Tick();
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.ready_stream_ids.clear();
        }

        bool finished = false;
        std::chrono::steady_clock::time_point ready_tp{};
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (ready_tp == std::chrono::steady_clock::time_point{} &&
                    std::find(host.ready_stream_ids.begin(), host.ready_stream_ids.end(), sid1) !=
                        host.ready_stream_ids.end()) {
                    ready_tp = std::chrono::steady_clock::now();
                }
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamFinished") !=
                    host.callback_names.end()) {
                    finished = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(finished);
        TEST_CHECK(ready_tp >= gate_tp);
        TEST_CHECK(avail_media_hits.load() == 1);
        TEST_CHECK(!saw_pragma_no_cache.load()); // Ajuste 2: WINHTTP_FLAG_REFRESH removed
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            TEST_CHECK(avail_hit_times.size() == 1 && avail_hit_times[0] >= gate_tp);
        }
    }

    // Test 2: Channel respects available_at and elapsed excludes wait.
    TEST_PHASE("Test 2: Channel waits for available_at; TrackStart & elapsed exclude wait");
    {
        avail_media_hits.store(0);
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            avail_hit_times.clear();
        }
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }

        ChannelId cid = engine.CreateChannel("AvailChan", 1);
        TEST_CHECK(cid != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.JoinChannel(1, cid));

        gate_tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        TEST_CHECK(engine.PlayChannel(cid, "https://www.youtube.com/watch?v=yt_avail_channel"));

        bool track_started = false;
        std::chrono::steady_clock::time_point track_start_tp{};
        std::int32_t elapsed_at_start = -1;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamChannelTrackStart") !=
                    host.callback_names.end()) {
                    track_started = true;
                    track_start_tp = std::chrono::steady_clock::now();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(track_started);
        TEST_CHECK(track_start_tp >= gate_tp);
        elapsed_at_start = engine.GetChannelElapsedSeconds(cid);
        TEST_CHECK(elapsed_at_start <= 1);
        TEST_CHECK(engine.DestroyChannel(cid));
    }

    // Test 3: Cancellation during available_at wait produces zero callbacks, zero media requests, zero transcoders.
    TEST_PHASE("Test 3: Cancel during available_at wait aborts cleanly with zero callbacks/requests");
    {
        cancel_media_hits.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        StreamId sid_stop = engine.PlayAudioStreamForPlayer(2, "https://www.youtube.com/watch?v=yt_avail_cancel_1", StreamOptions{});
        TEST_CHECK(sid_stop != INVALID_STREAM_ID);
        ChannelId cid_dest = engine.CreateChannel("CancelChan", 1);
        TEST_CHECK(cid_dest != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.PlayChannel(cid_dest, "https://www.youtube.com/watch?v=yt_avail_cancel_2"));

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.Tick();
        TEST_CHECK(engine.StopAudioStreamForPlayer(2));
        TEST_CHECK(engine.DestroyChannel(cid_dest));

        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1900);
        while (std::chrono::steady_clock::now() < until) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(cancel_media_hits.load() == 0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            for (const auto& cb : host.callback_names) {
                TEST_CHECK(cb != "OnAudioStreamReady");
                TEST_CHECK(cb != "OnAudioStreamStart");
                TEST_CHECK(cb != "OnAudioStreamChannelTrackStart");
            }
        }
    }

    // Test 4: Implausible available_at (120s in future) warns and proceeds without waiting.
    TEST_PHASE("Test 4: available_at 120s in future warns and plays immediately");
    {
        implausible_hits.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        clear_logs();

        auto t0 = std::chrono::steady_clock::now();
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_avail_implausible", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);
        bool ready = false;
        auto dl = t0 + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamReady") !=
                    host.callback_names.end()) {
                    ready = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(ready);
        TEST_CHECK(find_log(LogSeverity::Warning, "implausible available_at"));
        TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    }

    // Test 5: Early 403 without available_at waits 5s and retries the SAME URL once with zero re-resolve.
    TEST_PHASE("Test 5: Early 403 without available_at waits 5s and retries same URL once");
    {
        early_no_avail_hits.store(0);
        resolve_count.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        early_gate_tp = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_early_no_avail", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool finished = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(13);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamFinished") !=
                    host.callback_names.end()) {
                    finished = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(finished);
        TEST_CHECK(early_no_avail_hits.load() == 2);
        TEST_CHECK(resolve_count.load() == 1);
    }

    // Test 6: Pre-resolved channel track with already-past available_at starts immediately (no wait).
    TEST_PHASE("Test 6: Pre-resolved track with past available_at has zero wait");
    {
        preresolve_hits.store(0);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            host.callback_names.clear();
            host.error_events.clear();
        }
        ChannelId cid = engine.CreateChannel("PreResChan", 1);
        TEST_CHECK(cid != INVALID_CHANNEL_ID);
        // Resolve into cache first via Resolve (simulating pre-resolution before track play).
        ResolveResult pre = engine.ResolverForTesting().Resolve("https://www.youtube.com/watch?v=yt_preresolve_past");
        TEST_CHECK(pre.success && pre.metadata.available_at_epoch_ms > 0);

        auto t0 = std::chrono::steady_clock::now();
        TEST_CHECK(engine.PlayChannel(cid, "https://www.youtube.com/watch?v=yt_preresolve_past"));
        bool track_started = false;
        auto dl = t0 + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamChannelTrackStart") !=
                    host.callback_names.end()) {
                    track_started = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(track_started);
        auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        TEST_CHECK(start_ms < 800);
        TEST_CHECK(engine.DestroyChannel(cid));
    }

    engine.Shutdown();
    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acceptor, port);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> lk(srv_mtx);
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }
    std::filesystem::remove_all(temp_base, fec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound47AvailableAtAndEarly403Retry\n";
}

void TestRound48NoAudioWatchdog() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto has_log_substr = [&](const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const std::string valid_wav = BuildTestWavBytes(16000, 1200);

    std::atomic<bool> stop_srv{false};
    std::atomic<int> rr_403_hits{0};
    std::atomic<int> avail_rr_hits{0};
    std::atomic<int> silent_hits{0};
    std::chrono::steady_clock::time_point gate_tp{};
    std::chrono::steady_clock::time_point avail_rr_hit_tp{};
    std::mutex srv_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(srv_mtx);
            workers.emplace_back([&, sock]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream is(raw_req);
                std::string method, path;
                is >> method >> path;

                const auto now_tp = std::chrono::steady_clock::now();
                if (path == "/rr_403") {
                    rr_403_hits.fetch_add(1);
                    const std::string body = "Rejected403";
                    const std::string hdr =
                        "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(hdr), wec);
                    if (!wec) asio::write(*sock, asio::buffer(body), wec);
                    return;
                }
                if (path == "/avail_after_rr") {
                    avail_rr_hits.fetch_add(1);
                    {
                        std::lock_guard<std::mutex> lk(srv_mtx);
                        avail_rr_hit_tp = now_tp;
                    }
                    if (now_tp < gate_tp) {
                        const std::string body = "TooEarly403";
                        const std::string hdr =
                            "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                        asio::error_code wec;
                        asio::write(*sock, asio::buffer(hdr), wec);
                        if (!wec) asio::write(*sock, asio::buffer(body), wec);
                    } else {
                        const std::string hdr =
                            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                            std::to_string(valid_wav.size()) + "\r\nConnection: close\r\n\r\n";
                        asio::error_code wec;
                        asio::write(*sock, asio::buffer(hdr), wec);
                        if (!wec) asio::write(*sock, asio::buffer(valid_wav), wec);
                    }
                    return;
                }
                if (path == "/silent_stall") {
                    silent_hits.fetch_add(1);
                    const std::string hdr =
                        "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\n\r\n";
                    asio::error_code wec;
                    asio::write(*sock, asio::buffer(hdr), wec);
                    sock->non_blocking(true, wec);
                    const auto hold_dl = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                    char tmp[16];
                    while (!stop_srv.load() && std::chrono::steady_clock::now() < hold_dl) {
                        asio::error_code rdc;
                        std::size_t n = sock->read_some(asio::buffer(tmp), rdc);
                        if (n == 0 && rdc && rdc != asio::error::would_block && rdc != asio::error::try_again) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    return;
                }
                const std::string hdr = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                asio::error_code wec;
                asio::write(*sock, asio::buffer(hdr), wec);
            });
        }
    });

    // Part 1: Deliberate waits (re_resolve_callback + available_at) must not trigger no_audio_timeout_ms=1500.
    TEST_PHASE("Part 1: Intentional waits excluded from zero-audio watchdog");
    {
        clear_logs();
        TrackMetadata meta;
        meta.id = "r48_wait";
        meta.source_type = SourceType::ExtractorStream;
        meta.format_id = "251";
        meta.direct_media_url = base + "/rr_403";
        meta.available_at_epoch_ms = 1000; // Past timestamp so initial 403 triggers re_resolve immediately.

        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.no_audio_timeout_ms = 1500;
        tcfg.re_resolve_callback = [&](const TrackMetadata&, const std::atomic<bool>& cancel,
                                       TrackMetadata& out_meta, std::string&) -> bool {
            for (int i = 0; i < 30 && !cancel.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (cancel.load()) return false;
            gate_tp = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            const auto now_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            out_meta = meta;
            out_meta.direct_media_url = base + "/avail_after_rr";
            out_meta.available_at_epoch_ms = now_epoch_ms + 2000;
            out_meta.resolved_at = std::chrono::steady_clock::now();
            return true;
        };

        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(tr.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(buf->TotalWritten() > 0);
        TEST_CHECK(rr_403_hits.load() == 1);
        TEST_CHECK(avail_rr_hits.load() == 1);
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            TEST_CHECK(avail_rr_hit_tp >= gate_tp);
        }
        TEST_CHECK(!has_log_substr("produced 0 decoded PCM frames"));
    }

    // Part 2 (Control): Silent server without deliberate waits still trips no_audio_timeout_ms=1500.
    TEST_PHASE("Part 2: Silent connection without deliberate wait aborts at no_audio_timeout_ms");
    {
        clear_logs();
        TrackMetadata meta;
        meta.source_type = SourceType::DirectAudio;
        meta.direct_media_url = base + "/silent_stall";

        TranscoderConfig tcfg;
        tcfg.pace_realtime = false;
        tcfg.no_audio_timeout_ms = 1500;

        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        const auto t0 = std::chrono::steady_clock::now();
        AudioTranscoder tr(meta, tcfg, buf);
        TEST_CHECK(!tr.Run(cancel, pause, vol, err, &ec));
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        TEST_CHECK(ec != ErrorCode::None);
        TEST_CHECK(silent_hits.load() == 1);
        TEST_CHECK(elapsed_ms >= 1300 && elapsed_ms < 5000);
        TEST_CHECK(has_log_substr("produced 0 decoded PCM frames"));
    }

    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acceptor, port);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> lk(srv_mtx);
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound48NoAudioWatchdog\n";
}

void TestRound49WaitingCallbacks() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto find_log = [&](LogSeverity sev, const std::string& needle) {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::filesystem::path temp_base =
        std::filesystem::temp_directory_path() / "sampaudiostream_r49_tests";
    std::error_code fec;
    std::filesystem::remove_all(temp_base, fec);
    std::filesystem::create_directories(temp_base / "bin", fec);
#ifdef _WIN32
    { std::ofstream f(temp_base / "bin" / "yt-dlp.exe", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#else
    { std::ofstream f(temp_base / "bin" / "yt-dlp", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#endif

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const std::string valid_wav = BuildTestWavBytes(16000, 1200);

    std::atomic<bool> stop_srv{false};
    std::atomic<int> rr_403_hits{0};
    std::atomic<int> rr_avail_hits{0};
    std::mutex srv_mtx;
    std::vector<std::thread> workers;

    std::thread srv_thread([&]() {
        while (!stop_srv.load()) {
            asio::error_code ec;
            acceptor.non_blocking(true, ec);
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (ec) break;
            ConfigureTestSocketTimeouts(*sock);
            std::lock_guard<std::mutex> wlk(srv_mtx);
            workers.emplace_back([&, sock]() {
                asio::error_code rec;
                asio::streambuf sb;
                asio::read_until(*sock, sb, "\r\n\r\n", rec);
                if (rec) return;
                std::string raw_req(asio::buffers_begin(sb.data()), asio::buffers_end(sb.data()));
                std::istringstream is(raw_req);
                std::string method, path;
                is >> method >> path;

                std::string hdr, body;
                if (path == "/rr_403") {
                    rr_403_hits.fetch_add(1);
                    body = "Rejected403";
                    hdr = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/rr_avail_ok") {
                    rr_avail_hits.fetch_add(1);
                    body = valid_wav;
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else if (path == "/ok_media") {
                    body = valid_wav;
                    hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                } else {
                    hdr = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                }
                asio::error_code wec;
                asio::write(*sock, asio::buffer(hdr), wec);
                if (!body.empty() && !wec) {
                    asio::write(*sock, asio::buffer(body), wec);
                }
            });
        }
    });

    MockPlatformHost host;
    host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}, {2, {0.0f, 0.0f, 0.0f}, 0, 0}};
    host.player_names = {{1, "Tester49A"}, {2, "Tester49B"}};

    PluginConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.http_port = 0;
    cfg.base_dir = temp_base.string();
    cfg.bin_dir = (temp_base / "bin").string();
    cfg.auto_download_extractor = false;
    cfg.enable_js_runtime = false;
    cfg.auto_update_extractor = false;
    cfg.allow_private_network_urls = true;
    cfg.persist_channels = false;
    cfg.worker_threads = 2;
    cfg.transcoder_burst_ms = 2000;
    cfg.debug_logging = true;
    Logger::Instance().SetDebugEnabled(true);

    AudioEngine engine(host, cfg);
    TEST_CHECK(engine.Initialize());
    {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(engine.IsExtractorReady());
    }

    auto epoch_sec_offset = [](double sec) -> std::string {
        const double cur = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / 1000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << (cur + sec);
        return oss.str();
    };

    std::atomic<int> resolve_count{0};
    std::atomic<bool> stop_case_hook_done{false};
    engine.ResolverForTesting().SetYtDlpTestHook([&](
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string&,
        int,
        const std::atomic<bool>*) -> bool {
        const std::string& target = args.empty() ? std::string() : args.back();
        const int n = resolve_count.fetch_add(1) + 1;
        if (target.find("yt_r49_personal") != std::string::npos ||
            target.find("yt_r49_global") != std::string::npos ||
            target.find("yt_r49_channel") != std::string::npos) {
            out_stdout =
                "{\"id\":\"R49Wait\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(2.0) +
                ",\"url\":\"" + base + "/ok_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_r49_past") != std::string::npos) {
            out_stdout =
                "{\"id\":\"R49Past\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(-5.0) +
                ",\"url\":\"" + base + "/ok_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_r49_absent") != std::string::npos) {
            out_stdout =
                "{\"id\":\"R49Absent\",\"format_id\":\"251\",\"url\":\"" +
                base + "/ok_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_r49_far") != std::string::npos) {
            out_stdout =
                "{\"id\":\"R49Far\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(120.0) +
                ",\"url\":\"" + base + "/ok_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            return true;
        }
        if (target.find("yt_r49_stop_queued") != std::string::npos) {
            out_stdout =
                "{\"id\":\"R49StopQ\",\"format_id\":\"251\",\"available_at\":" +
                epoch_sec_offset(2.5) +
                ",\"url\":\"" + base + "/ok_media\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            stop_case_hook_done.store(true);
            return true;
        }
        if (target.find("yt_r49_reresolve") != std::string::npos) {
            if (n == 1) {
                out_stdout =
                    "{\"id\":\"R49RR\",\"format_id\":\"251\",\"available_at\":1,\"url\":\"" +
                    base + "/rr_403\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            } else {
                out_stdout =
                    "{\"id\":\"R49RR\",\"format_id\":\"251\",\"available_at\":" +
                    epoch_sec_offset(2.0) +
                    ",\"url\":\"" + base + "/rr_avail_ok\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
            }
            return true;
        }
        return false;
    });

    auto clear_host_events = [&]() {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.callback_names.clear();
        host.ready_stream_ids.clear();
        host.error_events.clear();
        host.processing_events.clear();
        host.waiting_events.clear();
        host.channel_waiting_events.clear();
        host.channel_track_starts.clear();
    };

    auto index_of_cb = [](const std::vector<std::string>& names, const std::string& target) -> std::ptrdiff_t {
        auto it = std::find(names.begin(), names.end(), target);
        return (it == names.end()) ? -1 : std::distance(names.begin(), it);
    };

    // 1. Personal stream with available_at = now + 2 s
    TEST_PHASE("1. Personal stream waiting callback, range, and guaranteed order");
    {
        clear_host_events();
        clear_logs();
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_r49_personal", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool started = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamStart") !=
                    host.callback_names.end()) {
                    started = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(started);
        TEST_CHECK(find_log(LogSeverity::Debug, "waiting "));
        TEST_CHECK(find_log(LogSeverity::Debug, "ms for media availability"));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.waiting_events.size() == 1);
            TEST_CHECK(host.waiting_events[0].player_id == 1);
            TEST_CHECK(host.waiting_events[0].stream_id == sid);
            TEST_CHECK(host.waiting_events[0].wait_ms >= 1500 && host.waiting_events[0].wait_ms <= 2400);
            const auto i_proc = index_of_cb(host.callback_names, "OnAudioStreamProcessing");
            const auto i_wait = index_of_cb(host.callback_names, "OnAudioStreamWaiting");
            const auto i_ready = index_of_cb(host.callback_names, "OnAudioStreamReady");
            const auto i_start = index_of_cb(host.callback_names, "OnAudioStreamStart");
            TEST_CHECK(i_proc >= 0 && i_wait > i_proc && i_ready > i_wait && i_start > i_ready);
        }
        TEST_CHECK(engine.StopAudioStreamForPlayer(1));
        PumpEngineTicks(engine, 4);
    }

    // 2. Global stream with available_at = now + 2 s
    TEST_PHASE("2. Global stream waiting callback with PAWN INVALID_PLAYER_ID");
    {
        clear_host_events();
        StreamId sid = engine.PlayAudioStream("https://www.youtube.com/watch?v=yt_r49_global", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool started = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamStart") !=
                    host.callback_names.end()) {
                    started = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(started);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.waiting_events.size() == 1);
            TEST_CHECK(host.waiting_events[0].player_id == PAWN_INVALID_PLAYER_ID);
            TEST_CHECK(host.waiting_events[0].stream_id == sid);
            TEST_CHECK(host.waiting_events[0].wait_ms >= 1500 && host.waiting_events[0].wait_ms <= 2400);
            const auto i_proc = index_of_cb(host.callback_names, "OnAudioStreamProcessing");
            const auto i_wait = index_of_cb(host.callback_names, "OnAudioStreamWaiting");
            const auto i_ready = index_of_cb(host.callback_names, "OnAudioStreamReady");
            const auto i_start = index_of_cb(host.callback_names, "OnAudioStreamStart");
            TEST_CHECK(i_proc >= 0 && i_wait > i_proc && i_ready > i_wait && i_start > i_ready);
        }
        TEST_CHECK(engine.StopAudioStream(sid));
        PumpEngineTicks(engine, 4);
    }

    // 3. Channel with available_at = now + 2 s
    TEST_PHASE("3. Channel track waiting callback once before TrackStart");
    {
        clear_host_events();
        clear_logs();
        ChannelId cid = engine.CreateChannel("R49Chan", 1);
        TEST_CHECK(cid != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.JoinChannel(1, cid));
        TEST_CHECK(engine.PlayChannel(cid, "https://www.youtube.com/watch?v=yt_r49_channel"));

        bool track_started = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamChannelTrackStart") !=
                    host.callback_names.end()) {
                    track_started = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        TEST_CHECK(track_started);
        TEST_CHECK(find_log(LogSeverity::Debug, "ms for track availability"));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_waiting_events.size() == 1);
            TEST_CHECK(host.channel_waiting_events[0].channel_id == cid);
            TEST_CHECK(host.channel_waiting_events[0].wait_ms >= 1500 && host.channel_waiting_events[0].wait_ms <= 2400);
            const auto i_cwait = index_of_cb(host.callback_names, "OnAudioStreamChannelTrackWaiting");
            const auto i_cstart = index_of_cb(host.callback_names, "OnAudioStreamChannelTrackStart");
            TEST_CHECK(i_cwait >= 0 && i_cstart > i_cwait);
        }
        TEST_CHECK(engine.DestroyChannel(cid));
        PumpEngineTicks(engine, 4);
    }

    // 4. No wait (available_at in past, absent, or 120s in future): neither callback fires
    TEST_PHASE("4. No waiting callbacks when available_at is past, absent, or >60s");
    {
        const std::vector<std::string> no_wait_urls = {
            "https://www.youtube.com/watch?v=yt_r49_past",
            "https://www.youtube.com/watch?v=yt_r49_absent",
            "https://www.youtube.com/watch?v=yt_r49_far"
        };
        for (const std::string& url : no_wait_urls) {
            clear_host_events();
            StreamId sid = engine.PlayAudioStreamForPlayer(1, url, StreamOptions{});
            TEST_CHECK(sid != INVALID_STREAM_ID);
            bool started = false;
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < dl) {
                engine.Tick();
                {
                    std::lock_guard<std::mutex> lk(host.mtx);
                    if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamStart") !=
                        host.callback_names.end()) {
                        started = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            TEST_CHECK(started);
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.waiting_events.empty());
                TEST_CHECK(host.channel_waiting_events.empty());
            }
            TEST_CHECK(engine.StopAudioStreamForPlayer(1));
            PumpEngineTicks(engine, 3);
        }
        ChannelId cid = engine.CreateChannel("R49NoWaitChan", 1);
        TEST_CHECK(cid != INVALID_CHANNEL_ID);
        for (const std::string& url : no_wait_urls) {
            clear_host_events();
            TEST_CHECK(engine.PlayChannel(cid, url));
            bool track_started = false;
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < dl) {
                engine.Tick();
                {
                    std::lock_guard<std::mutex> lk(host.mtx);
                    if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamChannelTrackStart") !=
                        host.callback_names.end()) {
                        track_started = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            TEST_CHECK(track_started);
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.waiting_events.empty());
                TEST_CHECK(host.channel_waiting_events.empty());
            }
        }
        TEST_CHECK(engine.DestroyChannel(cid));
        PumpEngineTicks(engine, 4);
    }

    // 5. Stop right after resolution (while Waiting post is still queued in MainThreadDispatcher)
    TEST_PHASE("5. Stop before queued Waiting post executes suppresses OnAudioStreamWaiting");
    {
        clear_host_events();
        stop_case_hook_done.store(false);
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_r49_stop_queued", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        auto hook_dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < hook_dl && !stop_case_hook_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        TEST_CHECK(stop_case_hook_done.load());
        // Let the extraction worker enqueue pending_delayed_stream_starts_ and post Waiting onto MainThreadDispatcher
        // before Tick() drains the dispatcher.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        TEST_CHECK(engine.StopAudioStreamForPlayer(1));
        PumpEngineTicks(engine, 10);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.waiting_events.empty());
            TEST_CHECK(std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamWaiting") ==
                       host.callback_names.end());
            TEST_CHECK(std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamReady") ==
                       host.callback_names.end());
        }
    }

    // 6. Re-resolution after 403 returning future available_at emits zero extra Waiting callbacks
    TEST_PHASE("6. Downloader 403 re-resolve with future available_at fires zero extra Waiting callbacks");
    {
        clear_host_events();
        resolve_count.store(0);
        rr_403_hits.store(0);
        rr_avail_hits.store(0);
        StreamId sid = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=yt_r49_reresolve", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);

        bool finished = false;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.error_events.empty());
                if (std::find(host.callback_names.begin(), host.callback_names.end(), "OnAudioStreamFinished") !=
                    host.callback_names.end()) {
                    finished = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        TEST_CHECK(finished);
        TEST_CHECK(resolve_count.load() == 2);
        TEST_CHECK(rr_403_hits.load() >= 1);
        TEST_CHECK(rr_avail_hits.load() == 1);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.waiting_events.empty());
            TEST_CHECK(host.channel_waiting_events.empty());
        }
    }

    engine.Shutdown();
    HttpClient::SetSecurityPolicy({});
    stop_srv.store(true);
    StopTestAcceptor(acceptor, port);
    if (srv_thread.joinable()) srv_thread.join();
    {
        std::lock_guard<std::mutex> lk(srv_mtx);
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }
    std::filesystem::remove_all(temp_base, fec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound49WaitingCallbacks\n";
}

void TestRound51YouTubeAutoCookies() {
    struct BrowserOverrideGuard {
        ~BrowserOverrideGuard() { SetBrowserCookieSearchBaseDirsForTesting(false); }
    } browser_guard;

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetDebugEnabled(false);
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto count_logs_matching = [&](LogSeverity sev, const std::string& needle) -> int {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        int c = 0;
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                ++c;
            }
        }
        return c;
    };
    auto any_log_contains = [&](const std::string& needle) -> bool {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r51_tests";
    std::error_code fec;
    std::filesystem::remove_all(temp_root, fec);
    const std::filesystem::path bin_dir = temp_root / "bin";
    const std::filesystem::path appdata_dir = temp_root / "appdata";
    const std::filesystem::path localappdata_dir = temp_root / "localappdata";
    const std::filesystem::path home_dir = temp_root / "home";
    std::filesystem::create_directories(bin_dir, fec);
    std::filesystem::create_directories(appdata_dir, fec);
    std::filesystem::create_directories(localappdata_dir, fec);
    std::filesystem::create_directories(home_dir, fec);
#ifdef _WIN32
    { std::ofstream f(bin_dir / "yt-dlp.exe", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#else
    { std::ofstream f(bin_dir / "yt-dlp", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#endif

    // Helper: create fake browser cookie DB with explicit relative mtime offset (seconds from now).
    auto create_cookie_file = [&](const std::filesystem::path& file_path, int mtime_offset_sec) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
        {
            std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
            ofs << "FAKE_COOKIE_DB";
        }
        const auto mtime = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(mtime_offset_sec);
        std::filesystem::last_write_time(file_path, mtime, ec);
    };

    auto make_cfg = [&](const std::filesystem::path& base_dir) {
        PluginConfig cfg;
        cfg.base_dir = base_dir.string();
        cfg.bin_dir = bin_dir.string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = false;
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = false;
        cfg.debug_logging = false;
        return cfg;
    };

    auto find_arg_val = [](const std::vector<std::string>& args, const std::string& flag) -> std::string {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag) return args[i + 1];
        }
        return {};
    };
    auto has_arg = [](const std::vector<std::string>& args, const std::string& flag) -> bool {
        return std::find(args.begin(), args.end(), flag) != args.end();
    };

    const std::string valid_media_json =
        "{\"id\":\"r51vid\",\"title\":\"Track 51\",\"format_id\":\"251\",\"available_at\":1,"
        "\"url\":\"http://127.0.0.1:18080/media.webm\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
    const std::string valid_search_json =
        "{\"entries\":[{\"id\":\"r51s1\",\"title\":\"Search Hit\",\"uploader\":\"Chan\",\"duration\":120}]}";
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});

    // Config key tests: cookies_from_browser validation & persistence
    TEST_PHASE("Config: extractor.cookies_from_browser parsing and sanitization");
    {
        clear_logs();
        const std::filesystem::path cfg_path = temp_root / "cfg_r51.json";
        {
            std::ofstream ofs(cfg_path, std::ios::trunc);
            ofs << "{\"extractor\":{\"cookies_from_browser\":\"OFF\"}}";
        }
        PluginConfig c1 = ConfigManager::LoadOrCreate(cfg_path.string());
        TEST_CHECK(c1.cookies_from_browser == "off");

        PluginConfig c2;
        c2.cookies_from_browser = "invalid_val";
        ConfigManager::Sanitize(c2);
        TEST_CHECK(c2.cookies_from_browser == "auto");
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "extractor.cookies_from_browser") == 1);
    }

    // 10. Detection unit checks + Search test
    TEST_PHASE("10. Auth challenge detection variants and Search integration");
    {
        bool age = false;
        TEST_CHECK(StreamResolver::IsYouTubeAuthChallenge(
            "ERROR: [youtube] xyz: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies for authentication.",
            &age));
        TEST_CHECK(!age);

        age = false;
        TEST_CHECK(StreamResolver::IsYouTubeAuthChallenge(
            "ERROR: Please sign in to confirm you\xE2\x80\x99re not a bot.",
            &age));
        TEST_CHECK(!age);

        age = false;
        TEST_CHECK(StreamResolver::IsYouTubeAuthChallenge(
            "Sign in to confirm you are not a bot",
            &age));
        TEST_CHECK(!age);

        age = false;
        TEST_CHECK(!StreamResolver::IsYouTubeAuthChallenge(
            "ERROR: ... --cookies-from-browser or --cookies ...",
            &age));
        TEST_CHECK(!age);

        age = false;
        TEST_CHECK(StreamResolver::IsYouTubeAuthChallenge(
            "ERROR: [youtube] xyz: Sign in to confirm your age. This video may be inappropriate for some users.",
            &age));
        TEST_CHECK(age);

        TEST_CHECK(!StreamResolver::IsYouTubeAuthChallenge("ERROR: Video unavailable", &age));
        TEST_CHECK(!age);
    }

    // Setup fake browser tree (both Firefox older/newer and Chrome even newer)
    const std::filesystem::path ff_old = appdata_dir / "Mozilla" / "Firefox" / "Profiles" / "old.default";
    const std::filesystem::path ff_new = appdata_dir / "Mozilla" / "Firefox" / "Profiles" / "newest.default-release";
    const std::filesystem::path chrome_def = localappdata_dir / "Google" / "Chrome" / "User Data" / "Default";
    const std::filesystem::path edge_def = localappdata_dir / "Microsoft" / "Edge" / "User Data" / "Default";
    const std::filesystem::path linux_ff = home_dir / "snap" / "firefox" / "common" / ".mozilla" / "firefox" / "snap.default";
    create_cookie_file(ff_old / "cookies.sqlite", -300);
    create_cookie_file(ff_new / "cookies.sqlite", -100);
    create_cookie_file(chrome_def / "Network" / "Cookies", -10); // Newest mtime overall, but Chrome comes AFTER Firefox!
    create_cookie_file(edge_def / "Cookies", -5);                 // Legacy Chrome/Edge path without Network\

    // Linux path test via home_dir alongside Windows path test
    SetBrowserCookieSearchBaseDirsForTesting(true, {appdata_dir.string(), localappdata_dir.string(), home_dir.string()});
    create_cookie_file(linux_ff / "cookies.sqlite", -200);
    {
        auto cands = DiscoverBrowserCookieCandidates();
        TEST_CHECK(cands.size() == 5);
        // Priority 1: Firefox (newest.default-release -> snap.default -> old.default)
        TEST_CHECK(cands[0].browser_name == "firefox" && cands[0].profile_name == "newest.default-release");
        TEST_CHECK(cands[1].browser_name == "firefox" && cands[1].profile_name == "snap.default");
        TEST_CHECK(cands[2].browser_name == "firefox" && cands[2].profile_name == "old.default");
        // Priority 2: Chrome before Edge
        TEST_CHECK(cands[3].browser_name == "chrome" && cands[3].profile_name == "Default");
        TEST_CHECK(cands[4].browser_name == "edge" && cands[4].profile_name == "Default");
    }
    // Keep only Windows-style ff_old, ff_new, chrome_def for resolver scenarios below
    std::filesystem::remove_all(home_dir, fec);
    std::filesystem::remove_all(edge_def.parent_path().parent_path(), fec);

    ThreadPool bm_pool(1);
    auto init_bm = [&](BinaryManager& bm) {
        bm.InitializeAsync(bm_pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsReady());
    };

    // 1. Without challenge: zero calls with --cookies-from-browser
    TEST_PHASE("1. No challenge never touches browsers or passes --cookies-from-browser");
    {
        clear_logs();
        const std::filesystem::path base1 = temp_root / "case1";
        std::filesystem::create_directories(base1, fec);
        PluginConfig cfg = make_cfg(base1);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        bool saw_browser_flag = false;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) -> bool {
            if (has_arg(args, "--cookies-from-browser")) saw_browser_flag = true;
            out_stdout = valid_media_json;
            return true;
        });
        ResolveResult r = resolver.Resolve("https://www.youtube.com/watch?v=r51_ok");
        TEST_CHECK(r.success);
        TEST_CHECK(!saw_browser_flag);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 0);
    }

    // 2. Challenge + Firefox & Chrome profiles: Firefox newest runs first, pinned for next calls, Info logged
    TEST_PHASE("2. Challenge discovers Firefox newest profile first and pins it");
    {
        clear_logs();
        const std::filesystem::path base2 = temp_root / "case2";
        std::filesystem::create_directories(base2, fec);
        PluginConfig cfg = make_cfg(base2);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        std::vector<std::string> seen_browser_specs;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            TEST_CHECK(!(has_arg(args, "--cookies") && has_arg(args, "--cookies-from-browser")));
            const std::string cfb = find_arg_val(args, "--cookies-from-browser");
            seen_browser_specs.push_back(cfb);
            if (cfb == "firefox:" + ff_new.string()) {
                out_stdout = valid_media_json;
                return true;
            }
            out_stderr = "ERROR: Sign in to confirm you're not a bot";
            return false;
        });

        ResolveResult r1 = resolver.Resolve("https://www.youtube.com/watch?v=r51_ff1");
        TEST_CHECK(r1.success);
        TEST_CHECK(seen_browser_specs.size() == 2);
        TEST_CHECK(seen_browser_specs[0].empty());
        TEST_CHECK(seen_browser_specs[1] == "firefox:" + ff_new.string());
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
        TEST_CHECK(count_logs_matching(
            LogSeverity::Info,
            "YouTube challenged this server's IP; using cookies from Firefox (profile: newest.default-release)") == 1);

        // Next call uses Firefox immediately on first attempt without re-discovery
        ResolveResult r2 = resolver.Resolve("https://www.youtube.com/watch?v=r51_ff2");
        TEST_CHECK(r2.success);
        TEST_CHECK(seen_browser_specs.size() == 3);
        TEST_CHECK(seen_browser_specs[2] == "firefox:" + ff_new.string());
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
    }

    // 3. Firefox fails, Chrome succeeds -> Chrome pinned for resolve & search
    TEST_PHASE("3. Firefox profiles fail, Chrome succeeds and is pinned");
    {
        clear_logs();
        const std::filesystem::path base3 = temp_root / "case3";
        std::filesystem::create_directories(base3, fec);
        PluginConfig cfg = make_cfg(base3);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        std::vector<std::string> seen_browser_specs;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            const std::string cfb = find_arg_val(args, "--cookies-from-browser");
            seen_browser_specs.push_back(cfb);
            if (cfb == "chrome:" + chrome_def.string()) {
                const std::string target = args.empty() ? std::string() : args.back();
                out_stdout = (target.rfind("ytsearch", 0) == 0) ? valid_search_json : valid_media_json;
                return true;
            }
            out_stderr = "ERROR: Sign in to confirm you\xE2\x80\x99re not a bot";
            return false;
        });

        ResolveResult r1 = resolver.Resolve("https://www.youtube.com/watch?v=r51_ch1");
        TEST_CHECK(r1.success);
        TEST_CHECK(seen_browser_specs.size() == 4); // none -> ff_new -> ff_old -> chrome_def
        TEST_CHECK(seen_browser_specs[1] == "firefox:" + ff_new.string());
        TEST_CHECK(seen_browser_specs[2] == "firefox:" + ff_old.string());
        TEST_CHECK(seen_browser_specs[3] == "chrome:" + chrome_def.string());
        TEST_CHECK(count_logs_matching(LogSeverity::Info, "using cookies from Chrome (profile: Default)") == 1);

        // Search uses pinned Chrome directly
        SearchExecutionResult sr = resolver.Search("test query", 5);
        TEST_CHECK(sr.success && sr.items.size() == 1);
        TEST_CHECK(seen_browser_specs.size() == 5);
        TEST_CHECK(seen_browser_specs[4] == "chrome:" + chrome_def.string());
    }

    // 4 & 10(search fail). None succeed -> SA_ERR_AUTH_REQUIRED (callback + resolver), 1 Warn, 30m/10m suppression
    TEST_PHASE("4. All browsers fail -> SA_ERR_AUTH_REQUIRED + single Warn + 30m cooldown");
    {
        clear_logs();
        const std::filesystem::path base4 = temp_root / "case4";
        std::filesystem::create_directories(base4, fec);
        MockPlatformHost host;
        host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
        PluginConfig cfg = make_cfg(base4);
        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        {
            auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < dl && !engine.IsExtractorReady()) {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            TEST_CHECK(engine.IsExtractorReady());
        }

        std::atomic<int> hook_calls{0};
        engine.ResolverForTesting().SetYtDlpTestHook([&](const std::vector<std::string>&, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            hook_calls.fetch_add(1);
            out_stderr = "ERROR: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies";
            return false;
        });

        StreamId sid1 = engine.PlayAudioStreamForPlayer(1, "https://www.youtube.com/watch?v=r51_fail1", StreamOptions{});
        TEST_CHECK(sid1 != INVALID_STREAM_ID);
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < dl) {
            engine.Tick();
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                if (!host.error_events.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.error_events.size() == 1);
            TEST_CHECK(host.error_events[0].error_code == static_cast<std::int32_t>(ErrorCode::AuthRequired));
            TEST_CHECK(static_cast<std::int32_t>(ErrorCode::AuthRequired) == 10);
            host.error_events.clear();
        }
        TEST_CHECK(engine.ResolverForTesting().GetCookieDiscoveryCountForTesting() == 1);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "docs/youtube-cookies.md") == 1);

        // Second blocked search within cooldown -> immediate SA_ERR_AUTH_REQUIRED, zero new discovery, zero new Warn
        const int calls_before_second = hook_calls.load();
        SearchExecutionResult sr = engine.ResolverForTesting().Search("blocked query", 3);
        TEST_CHECK(!sr.success && sr.error == ErrorCode::AuthRequired);
        TEST_CHECK(hook_calls.load() == calls_before_second + 1);
        TEST_CHECK(engine.ResolverForTesting().GetCookieDiscoveryCountForTesting() == 1);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "docs/youtube-cookies.md") == 1);

        engine.Shutdown();
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});
    }

    // 5. Default cookies.txt present at startup -> used on every call, zero browser discovery
    TEST_PHASE("5. Default scriptfiles/SampAudioStream/cookies.txt used from startup");
    {
        clear_logs();
        const std::filesystem::path base5 = temp_root / "case5";
        std::filesystem::create_directories(base5, fec);
        const std::filesystem::path def_cookies = base5 / "cookies.txt";
        { std::ofstream ofs(def_cookies, std::ios::binary); ofs << "# Netscape HTTP Cookie File\n"; }

        PluginConfig cfg = make_cfg(base5);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        TEST_CHECK(count_logs_matching(LogSeverity::Info, "Using YouTube cookies file:") == 1);

        std::string seen_cookies_arg;
        std::string seen_cookies_contents;
        bool saw_browser_arg = false;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string&, int, const std::atomic<bool>*) -> bool {
            seen_cookies_arg = find_arg_val(args, "--cookies");
            if (!seen_cookies_arg.empty()) {
                std::ifstream cifs(seen_cookies_arg, std::ios::binary);
                seen_cookies_contents.assign((std::istreambuf_iterator<char>(cifs)), std::istreambuf_iterator<char>());
            }
            if (has_arg(args, "--cookies-from-browser")) saw_browser_arg = true;
            out_stdout = valid_media_json;
            return true;
        });

        ResolveResult r = resolver.Resolve("https://www.youtube.com/watch?v=r51_defck");
        TEST_CHECK(r.success);
        TEST_CHECK(!seen_cookies_arg.empty());
        TEST_CHECK(seen_cookies_arg != def_cookies.string());
        TEST_CHECK(std::filesystem::path(seen_cookies_arg).parent_path() == (base5 / "tmp"));
        TEST_CHECK(seen_cookies_contents == "# Netscape HTTP Cookie File\n");
        TEST_CHECK(!saw_browser_arg);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 0);
    }

    // 6. Hot-dropping cookies.txt DURING 30-min cooldown makes next blocked resolve use it
    TEST_PHASE("6. Dropping cookies.txt after failed discovery activates on next blocked resolve");
    {
        clear_logs();
        const std::filesystem::path base6 = temp_root / "case6";
        std::filesystem::create_directories(base6, fec);
        // Temporarily hide all browsers so discovery fails
        const std::filesystem::path empty_home = temp_root / "empty_browsers";
        std::filesystem::create_directories(empty_home, fec);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty_home.string(), empty_home.string(), empty_home.string()});

        PluginConfig cfg = make_cfg(base6);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        const std::filesystem::path def_cookies = base6 / "cookies.txt";

        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            const std::string ck = find_arg_val(args, "--cookies");
            if (!ck.empty()) {
                out_stdout = valid_media_json;
                return true;
            }
            out_stderr = "ERROR: Sign in to confirm you're not a bot";
            return false;
        });

        ResolveResult r_fail = resolver.Resolve("https://www.youtube.com/watch?v=r51_hd1");
        TEST_CHECK(!r_fail.success && r_fail.error == ErrorCode::AuthRequired);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);

        // Now drop cookies.txt while still inside 30-min cooldown
        { std::ofstream ofs(def_cookies); ofs << "# Netscape HTTP Cookie File\n"; }
        ResolveResult r_ok = resolver.Resolve("https://www.youtube.com/watch?v=r51_hd2");
        TEST_CHECK(r_ok.success);
        TEST_CHECK(count_logs_matching(LogSeverity::Info, "Detected YouTube cookies file") == 1);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);

        SetBrowserCookieSearchBaseDirsForTesting(true, {appdata_dir.string(), localappdata_dir.string(), home_dir.string()});
    }

    // 7. cookies_from_browser = "off": no browser consulted even when Firefox exists
    TEST_PHASE("7. cookies_from_browser=off skips browser discovery and fails with AuthRequired");
    {
        clear_logs();
        const std::filesystem::path base7 = temp_root / "case7";
        std::filesystem::create_directories(base7, fec);
        PluginConfig cfg = make_cfg(base7);
        cfg.cookies_from_browser = "off";
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        bool saw_browser_arg = false;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            if (has_arg(args, "--cookies-from-browser")) saw_browser_arg = true;
            out_stderr = "ERROR: Sign in to confirm you're not a bot";
            return false;
        });
        ResolveResult r = resolver.Resolve("https://www.youtube.com/watch?v=r51_off");
        TEST_CHECK(!r.success && r.error == ErrorCode::AuthRequired);
        TEST_CHECK(!saw_browser_arg);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 0);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "cookies_from_browser=off") == 1);
    }

    // 8. Three concurrent blocked resolves run ONLY ONE discovery
    TEST_PHASE("8. Three concurrent blocked resolves execute only one browser discovery");
    {
        clear_logs();
        const std::filesystem::path base8 = temp_root / "case8";
        std::filesystem::create_directories(base8, fec);
        PluginConfig cfg = make_cfg(base8);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            const std::string cfb = find_arg_val(args, "--cookies-from-browser");
            if (cfb.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                out_stderr = "ERROR: Sign in to confirm you're not a bot";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (cfb == "firefox:" + ff_new.string()) {
                out_stdout = valid_media_json;
                return true;
            }
            out_stderr = "ERROR: Sign in to confirm you're not a bot";
            return false;
        });

        ResolveResult r1, r2, r3;
        std::thread t1([&]() { r1 = resolver.Resolve("https://www.youtube.com/watch?v=r51_conc1"); });
        std::thread t2([&]() { r2 = resolver.Resolve("https://www.youtube.com/watch?v=r51_conc2"); });
        std::thread t3([&]() { r3 = resolver.Resolve("https://www.youtube.com/watch?v=r51_conc3"); });
        t1.join();
        t2.join();
        t3.join();

        TEST_CHECK(r1.success && r2.success && r3.success);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
    }

    // 9. Configured cookies_file pointing to missing file -> single Warn at startup
    TEST_PHASE("9. Missing configured cookies_file emits single Warn on initialization");
    {
        clear_logs();
        const std::filesystem::path base9 = temp_root / "case9";
        std::filesystem::create_directories(base9, fec);
        PluginConfig cfg = make_cfg(base9);
        cfg.cookies_file = (base9 / "nonexistent_cookies.txt").string();
        BinaryManager bm(cfg);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "does not exist") == 1);
    }

    // 11. Never log cookie values at any log level (even with debug_logging=true)
    TEST_PHASE("11. Cookie secrets in yt-dlp stderr never appear in any log or error message");
    {
        clear_logs();
        const std::filesystem::path empty_home = temp_root / "empty_sec";
        std::filesystem::create_directories(empty_home, fec);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty_home.string(), empty_home.string(), empty_home.string()});

        const std::filesystem::path base11 = temp_root / "case11";
        std::filesystem::create_directories(base11, fec);
        PluginConfig cfg = make_cfg(base11);
        cfg.debug_logging = true;
        Logger::Instance().SetDebugEnabled(true);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        resolver.SetYtDlpTestHook([&](const std::vector<std::string>&, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            out_stderr =
                "WARNING: Cookie: VISITOR_INFO1_LIVE=SECRET_COOKIE_HDR_111\n"
                "ERROR: SAPISID=FAKE_SECRET_SAPISID_999; __Secure-3PSID=TOP_SECRET_3PSID_777; LOGIN_INFO=SECRET_LOGIN_INFO_555\n"
                "Set-Cookie: SID=RAW_SID_SECRET_333";
            return false;
        });

        ResolveResult r = resolver.Resolve("https://www.youtube.com/watch?v=r51_sec");
        TEST_CHECK(!r.success);
        for (const char* secret : {"SECRET_COOKIE_HDR_111", "FAKE_SECRET_SAPISID_999", "TOP_SECRET_3PSID_777",
                                   "SECRET_LOGIN_INFO_555", "RAW_SID_SECRET_333"}) {
            TEST_CHECK(r.error_message.find(secret) == std::string::npos);
            TEST_CHECK(!any_log_contains(secret));
        }
    }

    HttpClient::SetSecurityPolicy({});
    std::filesystem::remove_all(temp_root, fec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound51YouTubeAutoCookies\n";
}

void TestRound52PartAFixes() {
    struct TestEnvGuards {
        bool prev_debug{false};
        TestEnvGuards() : prev_debug(Logger::Instance().IsDebugEnabled()) {}
        ~TestEnvGuards() {
            StreamResolver::SetSimulateRootUidForTesting(false);
            SetBrowserCookieSearchBaseDirsForTesting(false);
            HttpClient::SetSecurityPolicy({});
            Logger::Instance().Flush();
            Logger::Instance().SetDebugEnabled(prev_debug);
            Logger::Instance().SetSink(nullptr);
        }
    } env_guard;

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    Logger::Instance().SetDebugEnabled(false);
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto count_logs_matching = [&](LogSeverity sev, const std::string& needle) -> int {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        int c = 0;
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                ++c;
            }
        }
        return c;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r52_parta_tests";
    std::error_code fec;
    std::filesystem::remove_all(temp_root, fec);
    const std::filesystem::path bin_dir = temp_root / "bin";
    const std::filesystem::path appdata_dir = temp_root / "appdata";
    const std::filesystem::path localappdata_dir = temp_root / "localappdata";
    const std::filesystem::path home_dir = temp_root / "home";
    std::filesystem::create_directories(bin_dir, fec);
    std::filesystem::create_directories(appdata_dir, fec);
    std::filesystem::create_directories(localappdata_dir, fec);
    std::filesystem::create_directories(home_dir, fec);
#ifdef _WIN32
    { std::ofstream f(bin_dir / "yt-dlp.exe", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#else
    { std::ofstream f(bin_dir / "yt-dlp", std::ios::binary); f << std::string(128 * 1024, 'y'); }
#endif

    auto create_cookie_db = [&](const std::filesystem::path& file_path, int mtime_offset_sec) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
        {
            std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
            ofs << "FAKE_BROWSER_COOKIE_DB";
        }
        const auto mtime = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(mtime_offset_sec);
        std::filesystem::last_write_time(file_path, mtime, ec);
    };

    auto make_cfg = [&](const std::filesystem::path& base_dir) {
        PluginConfig cfg;
        cfg.base_dir = base_dir.string();
        cfg.bin_dir = bin_dir.string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = false;
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = false;
        cfg.debug_logging = false;
        return cfg;
    };

    auto find_arg_val = [](const std::vector<std::string>& args, const std::string& flag) -> std::string {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag) return args[i + 1];
        }
        return {};
    };

    const std::string valid_media_json =
        "{\"id\":\"r52vid\",\"title\":\"Track 52\",\"format_id\":\"251\",\"available_at\":1,"
        "\"url\":\"http://127.0.0.1:18080/media.webm\",\"acodec\":\"opus\",\"ext\":\"webm\"}";
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});

    ThreadPool bm_pool(1);
    auto init_bm = [&](BinaryManager& bm) {
        bm.InitializeAsync(bm_pool);
        TEST_CHECK(bm.WaitUntilSettled(std::chrono::seconds(2)));
        TEST_CHECK(bm.IsReady());
    };

    // A1: Temp copy of cookies.txt preserves original & cleans stale files; expired cookies.txt stops retrying
    TEST_PHASE("A1: cookies.txt isolated temp snapshot, stale cleanup, original integrity");
    {
        clear_logs();
        const std::filesystem::path empty_dir = temp_root / "empty_a1";
        std::filesystem::create_directories(empty_dir, fec);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty_dir.string(), empty_dir.string(), empty_dir.string()});

        const std::filesystem::path base1 = temp_root / "case_a1";
        const std::filesystem::path tmp_dir = base1 / "tmp";
        std::filesystem::create_directories(tmp_dir, fec);
        const std::filesystem::path stale_file = tmp_dir / "cookies-999-1.txt";
        {
            std::ofstream ofs(stale_file, std::ios::binary | std::ios::trunc);
            ofs << "STALE";
        }
        TEST_CHECK(std::filesystem::exists(stale_file));

        const std::filesystem::path orig_cookies = base1 / "cookies.txt";
        const std::string orig_contents = "# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t0\tSID\tKEEP_ORIGINAL\n";
        {
            std::ofstream ofs(orig_cookies, std::ios::binary | std::ios::trunc);
            ofs << orig_contents;
        }
        const auto orig_mtime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
        std::filesystem::last_write_time(orig_cookies, orig_mtime, fec);
        const auto orig_mtime_recorded = std::filesystem::last_write_time(orig_cookies, fec);

        PluginConfig cfg = make_cfg(base1);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        // Stale temp file must be deleted on startup.
        TEST_CHECK(!std::filesystem::exists(stale_file));

        int total_calls = 0;
        int calls_with_cookies_flag = 0;
        std::string observed_cookie_arg;
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            ++total_calls;
            const std::string ck = find_arg_val(args, "--cookies");
            if (!ck.empty()) {
                ++calls_with_cookies_flag;
                observed_cookie_arg = ck;
                // Simulate yt-dlp overwriting the cookies file it received on exit.
                std::ofstream mut(ck, std::ios::binary | std::ios::trunc);
                mut << "MUTATED_BY_YTDLP";
            }
            out_stderr = "ERROR: [youtube] a1vid: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies for the authentication of your session.";
            return false;
        });

        ResolveResult r1 = resolver.Resolve("https://www.youtube.com/watch?v=r52_a1_1");
        TEST_CHECK(!r1.success);
        TEST_CHECK(r1.error == ErrorCode::AuthRequired);
        TEST_CHECK(calls_with_cookies_flag == 1);
        TEST_CHECK(!observed_cookie_arg.empty());
        TEST_CHECK(std::filesystem::weakly_canonical(std::filesystem::path(observed_cookie_arg), fec) !=
                   std::filesystem::weakly_canonical(orig_cookies, fec));

        // Original cookies.txt must remain bit-for-bit untouched with unchanged mtime.
        {
            std::ifstream ifs(orig_cookies, std::ios::binary);
            std::string actual((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            TEST_CHECK(actual == orig_contents);
        }
        TEST_CHECK(std::filesystem::last_write_time(orig_cookies, fec) == orig_mtime_recorded);

        // Zero cookies-*.txt temp files left inside <base_dir>/tmp.
        int remaining_tmp = 0;
        if (std::filesystem::exists(tmp_dir, fec)) {
            for (const auto& e : std::filesystem::directory_iterator(tmp_dir, fec)) {
                const std::string fname = e.path().filename().string();
                if (fname.rfind("cookies-", 0) == 0) {
                    ++remaining_tmp;
                }
            }
        }
        TEST_CHECK(remaining_tmp == 0);

        // Second blocked request within 30 min: cookies.txt already expired & cooldown active -> 0 --cookies retries & 0 new discoveries.
        const int prev_total = total_calls;
        const int prev_disc = resolver.GetCookieDiscoveryCountForTesting();
        TEST_CHECK(prev_disc == 1);
        ResolveResult r2 = resolver.Resolve("https://www.youtube.com/watch?v=r52_a1_2");
        TEST_CHECK(!r2.success);
        TEST_CHECK(r2.error == ErrorCode::AuthRequired);
        TEST_CHECK(calls_with_cookies_flag == 1);
        TEST_CHECK(total_calls == prev_total + 1);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == prev_disc);
    }

    // A2: Private video with "--cookies-from-browser or --cookies" must NOT trigger discovery or spend cooldown
    TEST_PHASE("A2: Private video error does not trigger cookie discovery or consume cooldown");
    {
        clear_logs();
        const std::filesystem::path base2 = temp_root / "case_a2";
        const std::filesystem::path app2 = temp_root / "app_a2";
        const std::filesystem::path empty2 = temp_root / "empty_a2";
        std::filesystem::create_directories(base2, fec);
        std::filesystem::create_directories(empty2, fec);
#ifdef _WIN32
        create_cookie_db(app2 / "Mozilla" / "Firefox" / "Profiles" / "p2.default" / "cookies.sqlite", -10);
        SetBrowserCookieSearchBaseDirsForTesting(true, {app2.string(), empty2.string(), empty2.string()});
#else
        create_cookie_db(app2 / ".mozilla" / "firefox" / "p2.default" / "cookies.sqlite", -10);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty2.string(), empty2.string(), app2.string()});
#endif

        PluginConfig cfg = make_cfg(base2);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        int mode = 0; // 0 = private video error, 1 = real anti-bot until --cookies-from-browser
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            if (mode == 0) {
                out_stderr = "ERROR: [youtube] priv123: Private video. Sign in if you've been granted access to this video. Use --cookies-from-browser or --cookies for the authentication of your session.";
                return false;
            }
            if (!find_arg_val(args, "--cookies-from-browser").empty()) {
                out_stdout = valid_media_json;
                return true;
            }
            out_stderr = "ERROR: [youtube] bot123: Sign in to confirm you're not a bot";
            return false;
        });

        ResolveResult priv_res = resolver.Resolve("https://www.youtube.com/watch?v=r52_priv");
        TEST_CHECK(!priv_res.success);
        TEST_CHECK(priv_res.error == ErrorCode::ExtractionFailed);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 0);

        // Next genuine anti-bot challenge still runs discovery cleanly.
        mode = 1;
        ResolveResult bot_res = resolver.Resolve("https://www.youtube.com/watch?v=r52_bot");
        TEST_CHECK(bot_res.success);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
    }

    // A3: Root UID simulation adds Linux root guidance to Warn log; non-root omits it
    TEST_PHASE("A3: Root warning appended when running as root UID 0");
    {
        clear_logs();
        const std::filesystem::path empty3 = temp_root / "empty_a3";
        std::filesystem::create_directories(empty3, fec);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty3.string(), empty3.string(), empty3.string()});

        const std::filesystem::path base3 = temp_root / "case_a3";
        std::filesystem::create_directories(base3, fec);
        PluginConfig cfg = make_cfg(base3);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        resolver.SetYtDlpTestHook([](const std::vector<std::string>&, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            out_stderr = "ERROR: [youtube] r52a3: Sign in to confirm you're not a bot.";
            return false;
        });

        StreamResolver::SetSimulateRootUidForTesting(true);
        ResolveResult r_root = resolver.Resolve("https://www.youtube.com/watch?v=r52_a3_1");
        TEST_CHECK(!r_root.success);
        TEST_CHECK(r_root.error == ErrorCode::AuthRequired);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "The server is running as root, so only /root browser profiles are searched.") == 1);
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "Run the server as the same OS user") == 1);

        StreamResolver::SetSimulateRootUidForTesting(false);
        clear_logs();
        const std::filesystem::path base3b = temp_root / "case_a3_nonroot";
        std::filesystem::create_directories(base3b, fec);
        PluginConfig cfg3b = make_cfg(base3b);
        BinaryManager bm3b(cfg3b);
        init_bm(bm3b);
        MetadataCache cache3b(32, 300);
        StreamResolver resolver3b(cfg3b, bm3b, cache3b);
        resolver3b.SetYtDlpTestHook([](const std::vector<std::string>&, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            out_stderr = "ERROR: [youtube] r52a3b: Sign in to confirm you're not a bot.";
            return false;
        });
        ResolveResult r_user = resolver3b.Resolve("https://www.youtube.com/watch?v=r52_a3_2");
        TEST_CHECK(!r_user.success);
#ifdef _WIN32
        TEST_CHECK(count_logs_matching(LogSeverity::Warning, "The server is running as root") == 0);
#endif
    }

    // A4: Expired cookies.txt + 3 concurrent blocked requests with Firefox present -> discovery runs once, all succeed
    TEST_PHASE("A4: Concurrent requests with expired cookies.txt run discovery once without bogus expiration");
    {
        clear_logs();
        const std::filesystem::path app4 = temp_root / "app_a4";
        const std::filesystem::path empty4 = temp_root / "empty_a4";
        std::filesystem::create_directories(empty4, fec);
#ifdef _WIN32
        create_cookie_db(app4 / "Mozilla" / "Firefox" / "Profiles" / "p4.default" / "cookies.sqlite", -5);
        SetBrowserCookieSearchBaseDirsForTesting(true, {app4.string(), empty4.string(), empty4.string()});
#else
        create_cookie_db(app4 / ".mozilla" / "firefox" / "p4.default" / "cookies.sqlite", -5);
        SetBrowserCookieSearchBaseDirsForTesting(true, {empty4.string(), empty4.string(), app4.string()});
#endif

        const std::filesystem::path base4 = temp_root / "case_a4";
        std::filesystem::create_directories(base4, fec);
        {
            std::ofstream ofs(base4 / "cookies.txt", std::ios::binary | std::ios::trunc);
            ofs << "# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t0\tSID\tEXPIRED\n";
        }

        PluginConfig cfg = make_cfg(base4);
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);

        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            if (!find_arg_val(args, "--cookies-from-browser").empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                out_stdout = valid_media_json;
                return true;
            }
            out_stderr = "ERROR: [youtube] conc: Sign in to confirm you're not a bot.";
            return false;
        });

        std::array<ResolveResult, 3> results;
        std::array<std::thread, 3> threads;
        for (int i = 0; i < 3; ++i) {
            threads[static_cast<std::size_t>(i)] = std::thread([&, i]() {
                results[static_cast<std::size_t>(i)] = resolver.Resolve("https://www.youtube.com/watch?v=r52_conc_" + std::to_string(i));
            });
        }
        for (auto& t : threads) t.join();
        for (const auto& r : results) {
            TEST_CHECK(r.success);
        }
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
        TEST_CHECK(count_logs_matching(LogSeverity::Info, "using cookies from Firefox") == 1);
    }

    // A5: 60s total budget across discovery candidates stops cleanly before exhausting all 4 candidates
    TEST_PHASE("A5: Total cookie discovery timeout cap stops candidate loop and enters cooldown");
    {
        clear_logs();
        Logger::Instance().SetDebugEnabled(true);
        const std::filesystem::path app5 = temp_root / "app_a5";
        const std::filesystem::path local5 = temp_root / "local_a5";
        const std::filesystem::path home5 = temp_root / "home_a5";
#ifdef _WIN32
        create_cookie_db(app5 / "Mozilla" / "Firefox" / "Profiles" / "ff5.default" / "cookies.sqlite", -10);
        create_cookie_db(local5 / "Microsoft" / "Edge" / "User Data" / "Default" / "Network" / "Cookies", -20);
        create_cookie_db(local5 / "Google" / "Chrome" / "User Data" / "Default" / "Network" / "Cookies", -30);
        create_cookie_db(local5 / "BraveSoftware" / "Brave-Browser" / "User Data" / "Default" / "Network" / "Cookies", -40);
#else
        create_cookie_db(home5 / ".mozilla" / "firefox" / "ff5.default" / "cookies.sqlite", -10);
        create_cookie_db(home5 / ".config" / "microsoft-edge" / "Default" / "Cookies", -20);
        create_cookie_db(home5 / ".config" / "google-chrome" / "Default" / "Cookies", -30);
        create_cookie_db(home5 / ".config" / "BraveSoftware" / "Brave-Browser" / "Default" / "Cookies", -40);
#endif
        SetBrowserCookieSearchBaseDirsForTesting(true, {app5.string(), local5.string(), home5.string()});

        const std::filesystem::path base5 = temp_root / "case_a5";
        std::filesystem::create_directories(base5, fec);
        PluginConfig cfg = make_cfg(base5);
        cfg.debug_logging = true;
        BinaryManager bm(cfg);
        init_bm(bm);
        MetadataCache cache(32, 300);
        StreamResolver resolver(cfg, bm, cache);
        resolver.SetCookieDiscoveryBudgetMsForTesting(300);

        std::atomic<int> candidate_attempts{0};
        resolver.SetYtDlpTestHook([&](const std::vector<std::string>& args, std::string&, std::string& out_stderr, int, const std::atomic<bool>*) -> bool {
            if (!find_arg_val(args, "--cookies-from-browser").empty()) {
                ++candidate_attempts;
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
            }
            out_stderr = "ERROR: [youtube] cap5: Sign in to confirm you're not a bot.";
            return false;
        });

        ResolveResult r5 = resolver.Resolve("https://www.youtube.com/watch?v=r52_a5_cap");
        TEST_CHECK(!r5.success);
        TEST_CHECK(r5.error == ErrorCode::AuthRequired);
        TEST_CHECK(candidate_attempts.load() >= 1);
        TEST_CHECK(candidate_attempts.load() < 4);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
        TEST_CHECK(count_logs_matching(LogSeverity::Debug, "total timeout cap") >= 1);

        // Inside cooldown: next challenge does NOT re-run discovery.
        ResolveResult r5_cd = resolver.Resolve("https://www.youtube.com/watch?v=r52_a5_cd");
        TEST_CHECK(!r5_cd.success);
        TEST_CHECK(resolver.GetCookieDiscoveryCountForTesting() == 1);
    }

    std::filesystem::remove_all(temp_root, fec);
    std::cout << "[PASS] TestRound52PartAFixes\n";
}

void TestRound52PartBUnifiedConfig() {
    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    const bool prev_debug = Logger::Instance().IsDebugEnabled();
    Logger::Instance().SetDebugEnabled(false);
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto has_log = [&](LogSeverity sev, const std::string& needle) -> bool {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "sampaudiostream_r52_partb_tests";
    std::error_code fec;
    std::filesystem::remove_all(temp_root, fec);
    std::filesystem::create_directories(temp_root, fec);

    const std::string expected_golden_json =
        "{\n"
        "    \"network\":\n"
        "    {\n"
        "        \"bind_ip\": \"0.0.0.0\",\n"
        "        \"public_ip\": \"\",\n"
        "        \"http_port\": 0,\n"
        "        \"lan_aware_urls\": true,\n"
        "        \"allow_private_network_urls\": false,\n"
        "        \"max_http_connections\": 1024,\n"
        "        \"max_connections_per_ip\": 64\n"
        "    },\n"
        "    \"audio\":\n"
        "    {\n"
        "        \"mp3_bitrate_kbps\": 128,\n"
        "        \"mp3_sample_rate\": 48000,\n"
        "        \"transcoder_burst_ms\": 6000,\n"
        "        \"max_concurrent_streams\": 64,\n"
        "        \"spatial_update_interval_ms\": 200,\n"
        "        \"player_priority_order\": \"personal,channel,global\",\n"
        "        \"normalize_volume\": false,\n"
        "        \"live_title_poll_seconds\": 30\n"
        "    },\n"
        "    \"channels\":\n"
        "    {\n"
        "        \"max_channels\": 100,\n"
        "        \"max_queue_per_channel\": 50,\n"
        "        \"persist_channels\": true,\n"
        "        \"channel_restore_grace_seconds\": 300\n"
        "    },\n"
        "    \"extractor\":\n"
        "    {\n"
        "        \"bin_dir\": \"scriptfiles/SampAudioStream/bin\",\n"
        "        \"auto_download\": true,\n"
        "        \"auto_update\": true,\n"
        "        \"enable_js_runtime\": true,\n"
        "        \"js_runtime\": \"auto\",\n"
        "        \"cookies_file\": \"\",\n"
        "        \"cookies_from_browser\": \"auto\",\n"
        "        \"yt_dlp_cache_max_age_days\": 7,\n"
        "        \"max_search_results\": 10\n"
        "    },\n"
        "    \"performance\":\n"
        "    {\n"
        "        \"worker_threads\": 4,\n"
        "        \"max_concurrent_transcoders\": 32,\n"
        "        \"max_prefetch_mb\": 16,\n"
        "        \"cache_max_entries\": 256,\n"
        "        \"cache_ttl_seconds\": 3600\n"
        "    },\n"
        "    \"scripting\":\n"
        "    {\n"
        "        \"hook_native_audio\": true,\n"
        "        \"pawn_text_encoding\": \"cp1252\"\n"
        "    },\n"
        "    \"logging\":\n"
        "    {\n"
        "        \"debug\": false,\n"
        "        \"startup_banner\": true,\n"
        "        \"http_listen\": true,\n"
        "        \"public_ip_detected\": true,\n"
        "        \"extractor_status\": true\n"
        "    }\n"
        "}\n";

    // B2 & B3: SA-MP newly created config.json matches golden Allman 4-space output byte-for-byte & round-trips cleanly
    TEST_PHASE("B2/B3: SA-MP auto-created config matches exact Allman 4-space golden bytes and schema order");
    {
        clear_logs();
        const std::filesystem::path fresh_cfg_path = temp_root / "fresh_config.json";
        const PluginConfig created = ConfigManager::LoadOrCreate(fresh_cfg_path.string());
        TEST_CHECK(std::filesystem::exists(fresh_cfg_path));
        TEST_CHECK(!has_log(LogSeverity::Warning, "Config key"));
        TEST_CHECK(!has_log(LogSeverity::Error, "JSON"));

        std::ifstream ifs(fresh_cfg_path, std::ios::binary);
        const std::string actual_bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        TEST_CHECK(actual_bytes == expected_golden_json);

        // Re-read generated file and confirm exact default values without warnings
        clear_logs();
        const PluginConfig reloaded = ConfigManager::LoadOrCreate(fresh_cfg_path.string());
        TEST_CHECK(!has_log(LogSeverity::Warning, "Config key"));
        TEST_CHECK(reloaded.bind_ip == created.bind_ip);
        TEST_CHECK(reloaded.http_port == created.http_port);
        TEST_CHECK(reloaded.max_channels == 100);
        TEST_CHECK(reloaded.max_http_connections == 1024);
        TEST_CHECK(reloaded.max_connections_per_ip == 64);
        TEST_CHECK(reloaded.max_search_results == 10);
        TEST_CHECK(reloaded.debug_logging == false);
        TEST_CHECK(reloaded.js_runtime == "auto");
    }

    // B1: SA-MP reads all 7 groups (including new channels & moved logging.debug / extractor.max_search_results)
    TEST_PHASE("B1: SA-MP reads all 7 unified groups, string escaping round-trip, and reports <group>.<key> warnings");
    {
        clear_logs();
        PluginConfig custom;
        custom.bind_ip = "127.0.0.1";
        custom.public_ip = "203.0.113.42";
        custom.http_port = 8123;
        custom.lan_aware_urls = false;
        custom.allow_private_network_urls = true;
        custom.max_http_connections = 2048;
        custom.max_connections_per_ip = 128;
        custom.mp3_bitrate_kbps = 256;
        custom.mp3_sample_rate = 44100;
        custom.transcoder_burst_ms = 8500;
        custom.max_concurrent_streams = 32;
        custom.spatial_update_interval_ms = 120;
        custom.player_priority_order = "channel,personal,global";
        custom.live_title_poll_seconds = 45;
        custom.max_channels = 222;
        custom.max_queue_per_channel = 77;
        custom.persist_channels = false;
        custom.channel_restore_grace_seconds = 90;
        custom.bin_dir = "custom\\bin\"dir\"\\sub";
        custom.auto_download_extractor = false;
        custom.auto_update_extractor = false;
        custom.enable_js_runtime = false;
        custom.js_runtime = "quickjs";
        custom.cookies_file = "custom/cookies.txt";
        custom.cookies_from_browser = "off";
        custom.yt_dlp_cache_max_age_days = 14;
        custom.max_search_results = 25;
        custom.worker_threads = 8;
        custom.max_concurrent_transcoders = 16;
        custom.max_prefetch_mb = 24;
        custom.cache_max_entries = 512;
        custom.cache_ttl_seconds = 7200;
        custom.hook_native_audio = false;
        custom.pawn_text_encoding = "utf8";
        custom.debug_logging = true;
        custom.log_startup_banner = false;
        custom.log_http_listen = false;
        custom.log_public_ip_detected = false;
        custom.log_extractor_status = false;

        const std::filesystem::path custom_path = temp_root / "custom_config.json";
        TEST_CHECK(ConfigManager::Save(custom, custom_path.string()));
        const PluginConfig read_back = ConfigManager::LoadOrCreate(custom_path.string());
        TEST_CHECK(read_back.bind_ip == custom.bind_ip);
        TEST_CHECK(read_back.public_ip == custom.public_ip);
        TEST_CHECK(read_back.http_port == 8123);
        TEST_CHECK(!read_back.lan_aware_urls);
        TEST_CHECK(read_back.allow_private_network_urls);
        TEST_CHECK(read_back.max_http_connections == 2048);
        TEST_CHECK(read_back.max_connections_per_ip == 128);
        TEST_CHECK(read_back.mp3_bitrate_kbps == 256);
        TEST_CHECK(read_back.mp3_sample_rate == 44100);
        TEST_CHECK(read_back.transcoder_burst_ms == 8500);
        TEST_CHECK(read_back.max_concurrent_streams == 32);
        TEST_CHECK(read_back.spatial_update_interval_ms == 120);
        TEST_CHECK(read_back.player_priority_order == "channel,personal,global");
        TEST_CHECK(read_back.live_title_poll_seconds == 45);
        TEST_CHECK(read_back.max_channels == 222);
        TEST_CHECK(read_back.max_queue_per_channel == 77);
        TEST_CHECK(!read_back.persist_channels);
        TEST_CHECK(read_back.channel_restore_grace_seconds == 90);
        TEST_CHECK(read_back.bin_dir == custom.bin_dir);
        TEST_CHECK(!read_back.auto_download_extractor);
        TEST_CHECK(!read_back.auto_update_extractor);
        TEST_CHECK(!read_back.enable_js_runtime);
        TEST_CHECK(read_back.js_runtime == "quickjs");
        TEST_CHECK(read_back.cookies_file == "custom/cookies.txt");
        TEST_CHECK(read_back.cookies_from_browser == "off");
        TEST_CHECK(read_back.yt_dlp_cache_max_age_days == 14);
        TEST_CHECK(read_back.max_search_results == 25);
        TEST_CHECK(read_back.worker_threads == 8);
        TEST_CHECK(read_back.max_concurrent_transcoders == 16);
        TEST_CHECK(read_back.max_prefetch_mb == 24);
        TEST_CHECK(read_back.cache_max_entries == 512);
        TEST_CHECK(read_back.cache_ttl_seconds == 7200);
        TEST_CHECK(!read_back.hook_native_audio);
        TEST_CHECK(read_back.pawn_text_encoding == "utf8");
        TEST_CHECK(read_back.debug_logging);
        TEST_CHECK(!read_back.log_startup_banner);
        TEST_CHECK(!read_back.log_http_listen);
        TEST_CHECK(!read_back.log_public_ip_detected);
        TEST_CHECK(!read_back.log_extractor_status);

        // Verify validation warnings cite exact <group>.<key> paths
        clear_logs();
        const std::filesystem::path bad_path = temp_root / "bad_values.json";
        {
            std::ofstream ofs(bad_path, std::ios::binary | std::ios::trunc);
            ofs << R"({"network":{"max_http_connections":16,"max_connections_per_ip":9999},"channels":{"max_channels":5000},"extractor":{"max_search_results":99},"logging":{"debug":"bad_bool"},"scripting":{"pawn_text_encoding":"latin9"}})";
        }
        const PluginConfig bad_cfg = ConfigManager::LoadOrCreate(bad_path.string());
        TEST_CHECK(bad_cfg.max_http_connections == 1024);
        TEST_CHECK(bad_cfg.max_connections_per_ip == 64);
        TEST_CHECK(bad_cfg.max_channels == 100);
        TEST_CHECK(bad_cfg.max_search_results == 10);
        TEST_CHECK(bad_cfg.debug_logging == false);
        TEST_CHECK(bad_cfg.pawn_text_encoding == "cp1252");
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'network.max_http_connections'"));
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'network.max_connections_per_ip'"));
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'channels.max_channels'"));
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'extractor.max_search_results'"));
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'logging.debug'"));
        TEST_CHECK(has_log(LogSeverity::Warning, "Config key 'scripting.pawn_text_encoding'"));
    }

    // B4 & B5: open.mp dotted-key reader (including extractor.js_runtime = "quickjs") & default registration parity
    TEST_PHASE("B4/B5: open.mp reads all 7 dotted groups incl. extractor.js_runtime and defaults match SA-MP 1:1");
    {
        clear_logs();
        std::map<std::string, std::string> omp_strings;
        std::map<std::string, std::int64_t> omp_ints;
        std::map<std::string, bool> omp_bools;
        std::vector<std::string> registered_omp_keys;

        OmpConfigWriter writer;
        writer.set_string = [&](const char* k, const std::string& v) {
            registered_omp_keys.emplace_back(k);
            omp_strings[k] = v;
        };
        writer.set_int = [&](const char* k, std::int32_t v) {
            registered_omp_keys.emplace_back(k);
            omp_ints[k] = static_cast<std::int64_t>(v);
        };
        writer.set_bool = [&](const char* k, bool v) {
            registered_omp_keys.emplace_back(k);
            omp_bools[k] = v;
        };
        ConfigManager::ProvideOmpDefaults(writer);

        const std::vector<ConfigSchemaFieldInfo> schema = ConfigManager::GetUnifiedSchemaFields();
        TEST_CHECK(schema.size() == 40);
        TEST_CHECK(registered_omp_keys.size() == schema.size());
        for (std::size_t i = 0; i < schema.size(); ++i) {
            TEST_CHECK(registered_omp_keys[i] == ("sampaudiostream." + schema[i].full_key));
        }

        // Verify old ungrouped/prefixed keys are NOT registered
        auto is_registered = [&](const std::string& full) -> bool {
            return std::find(registered_omp_keys.begin(), registered_omp_keys.end(), full) != registered_omp_keys.end();
        };
        TEST_CHECK(!is_registered("sampaudiostream.debug"));
        TEST_CHECK(!is_registered("sampaudiostream.debug_logging"));
        TEST_CHECK(!is_registered("sampaudiostream.auto_download_extractor"));
        TEST_CHECK(!is_registered("sampaudiostream.log_startup_banner"));
        TEST_CHECK(!is_registered("sampaudiostream.bind_ip"));
        TEST_CHECK(!is_registered("sampaudiostream.audio.max_channels"));
        TEST_CHECK(!is_registered("sampaudiostream.scripting.debug_logging"));
        TEST_CHECK(!is_registered("sampaudiostream.performance.max_search_results"));

        // Override values across all 7 open.mp dotted groups, including Ajuste 1 bug extractor.js_runtime = "quickjs"
        omp_strings["sampaudiostream.network.bind_ip"] = "10.1.2.3";
        omp_strings["sampaudiostream.network.public_ip"] = "198.51.100.9";
        omp_ints["sampaudiostream.network.http_port"] = 7999;
        omp_bools["sampaudiostream.network.lan_aware_urls"] = false;
        omp_bools["sampaudiostream.network.allow_private_network_urls"] = true;
        omp_ints["sampaudiostream.network.max_http_connections"] = 4096;
        omp_ints["sampaudiostream.network.max_connections_per_ip"] = 256;
        omp_ints["sampaudiostream.audio.mp3_bitrate_kbps"] = 192;
        omp_ints["sampaudiostream.audio.mp3_sample_rate"] = 44100;
        omp_ints["sampaudiostream.audio.transcoder_burst_ms"] = 4500;
        omp_ints["sampaudiostream.audio.max_concurrent_streams"] = 48;
        omp_ints["sampaudiostream.audio.spatial_update_interval_ms"] = 150;
        omp_strings["sampaudiostream.audio.player_priority_order"] = "global,channel,personal";
        omp_bools["sampaudiostream.audio.normalize_volume"] = true;
        omp_ints["sampaudiostream.audio.live_title_poll_seconds"] = 75;
        omp_ints["sampaudiostream.channels.max_channels"] = 250;
        omp_ints["sampaudiostream.channels.max_queue_per_channel"] = 80;
        omp_bools["sampaudiostream.channels.persist_channels"] = false;
        omp_ints["sampaudiostream.channels.channel_restore_grace_seconds"] = 60;
        omp_strings["sampaudiostream.extractor.bin_dir"] = "omp_bin";
        omp_bools["sampaudiostream.extractor.auto_download"] = false;
        omp_bools["sampaudiostream.extractor.auto_update"] = false;
        omp_bools["sampaudiostream.extractor.enable_js_runtime"] = true;
        omp_strings["sampaudiostream.extractor.js_runtime"] = "quickjs";
        omp_strings["sampaudiostream.extractor.cookies_file"] = "omp_cookies.txt";
        omp_strings["sampaudiostream.extractor.cookies_from_browser"] = "off";
        omp_ints["sampaudiostream.extractor.yt_dlp_cache_max_age_days"] = 30;
        omp_ints["sampaudiostream.extractor.max_search_results"] = 20;
        omp_ints["sampaudiostream.performance.worker_threads"] = 6;
        omp_ints["sampaudiostream.performance.max_concurrent_transcoders"] = 24;
        omp_ints["sampaudiostream.performance.max_prefetch_mb"] = 32;
        omp_ints["sampaudiostream.performance.cache_max_entries"] = 128;
        omp_ints["sampaudiostream.performance.cache_ttl_seconds"] = 1800;
        omp_bools["sampaudiostream.scripting.hook_native_audio"] = false;
        omp_strings["sampaudiostream.scripting.pawn_text_encoding"] = "utf8";
        omp_bools["sampaudiostream.logging.debug"] = true;
        omp_bools["sampaudiostream.logging.startup_banner"] = false;
        omp_bools["sampaudiostream.logging.http_listen"] = false;
        omp_bools["sampaudiostream.logging.public_ip_detected"] = false;
        omp_bools["sampaudiostream.logging.extractor_status"] = false;

        OmpConfigReader reader;
        reader.get_string = [&](const char* k, std::string& out) -> bool {
            auto it = omp_strings.find(k);
            if (it == omp_strings.end()) return false;
            out = it->second;
            return true;
        };
        reader.get_int = [&](const char* k, std::int64_t& out) -> bool {
            auto it = omp_ints.find(k);
            if (it == omp_ints.end()) return false;
            out = it->second;
            return true;
        };
        reader.get_bool = [&](const char* k, bool& out) -> bool {
            auto it = omp_bools.find(k);
            if (it == omp_bools.end()) return false;
            out = it->second;
            return true;
        };

        const PluginConfig omp_loaded = ConfigManager::LoadFromOmp(reader);
        TEST_CHECK(omp_loaded.bind_ip == "10.1.2.3");
        TEST_CHECK(omp_loaded.public_ip == "198.51.100.9");
        TEST_CHECK(omp_loaded.http_port == 7999);
        TEST_CHECK(!omp_loaded.lan_aware_urls);
        TEST_CHECK(omp_loaded.allow_private_network_urls);
        TEST_CHECK(omp_loaded.max_http_connections == 4096);
        TEST_CHECK(omp_loaded.max_connections_per_ip == 256);
        TEST_CHECK(omp_loaded.mp3_bitrate_kbps == 192);
        TEST_CHECK(omp_loaded.mp3_sample_rate == 44100);
        TEST_CHECK(omp_loaded.transcoder_burst_ms == 4500);
        TEST_CHECK(omp_loaded.max_concurrent_streams == 48);
        TEST_CHECK(omp_loaded.spatial_update_interval_ms == 150);
        TEST_CHECK(omp_loaded.player_priority_order == "global,channel,personal");
        TEST_CHECK(omp_loaded.normalize_volume);
        TEST_CHECK(omp_loaded.live_title_poll_seconds == 75);
        TEST_CHECK(omp_loaded.max_channels == 250);
        TEST_CHECK(omp_loaded.max_queue_per_channel == 80);
        TEST_CHECK(!omp_loaded.persist_channels);
        TEST_CHECK(omp_loaded.channel_restore_grace_seconds == 60);
        TEST_CHECK(omp_loaded.bin_dir == "omp_bin");
        TEST_CHECK(!omp_loaded.auto_download_extractor);
        TEST_CHECK(!omp_loaded.auto_update_extractor);
        TEST_CHECK(omp_loaded.enable_js_runtime);
        TEST_CHECK(omp_loaded.js_runtime == "quickjs");
        TEST_CHECK(omp_loaded.cookies_file == "omp_cookies.txt");
        TEST_CHECK(omp_loaded.cookies_from_browser == "off");
        TEST_CHECK(omp_loaded.yt_dlp_cache_max_age_days == 30);
        TEST_CHECK(omp_loaded.max_search_results == 20);
        TEST_CHECK(omp_loaded.worker_threads == 6);
        TEST_CHECK(omp_loaded.max_concurrent_transcoders == 24);
        TEST_CHECK(omp_loaded.max_prefetch_mb == 32);
        TEST_CHECK(omp_loaded.cache_max_entries == 128);
        TEST_CHECK(omp_loaded.cache_ttl_seconds == 1800);
        TEST_CHECK(!omp_loaded.hook_native_audio);
        TEST_CHECK(omp_loaded.pawn_text_encoding == "utf8");
        TEST_CHECK(omp_loaded.debug_logging);
        TEST_CHECK(!omp_loaded.log_startup_banner);
        TEST_CHECK(!omp_loaded.log_http_listen);
        TEST_CHECK(!omp_loaded.log_public_ip_detected);
        TEST_CHECK(!omp_loaded.log_extractor_status);
    }

    std::filesystem::remove_all(temp_root, fec);
    Logger::Instance().Flush();
    Logger::Instance().SetDebugEnabled(prev_debug);
    Logger::Instance().SetSink(nullptr);
    std::cout << "[PASS] TestRound52PartBUnifiedConfig\n";
}

void TestRound53NetworkRetryAndPortWarn() {
    struct EnvGuard {
        bool prev_debug{false};
        EnvGuard() : prev_debug(Logger::Instance().IsDebugEnabled()) {}
        ~EnvGuard() {
            HttpClient::SetSecurityPolicy({});
            Logger::Instance().Flush();
            Logger::Instance().SetDebugEnabled(prev_debug);
            Logger::Instance().SetSink(nullptr);
        }
    } env_guard;

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    Logger::Instance().SetDebugEnabled(true);
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto count_logs = [&](LogSeverity sev, const std::string& needle) -> int {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        int n = 0;
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    };
    auto any_log_contains = [&](const std::string& needle) -> bool {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    // Valid minimal 44100 Hz 16-bit mono PCM WAV (~0.3 s = 26460 data bytes)
    auto make_wav_bytes = []() -> std::string {
        const std::uint32_t sample_rate = 44100;
        const std::uint16_t channels = 1;
        const std::uint16_t bits = 16;
        const std::uint32_t num_samples = 13230;
        const std::uint32_t data_bytes = num_samples * channels * (bits / 8);
        const std::uint32_t byte_rate = sample_rate * channels * (bits / 8);
        const std::uint16_t block_align = channels * (bits / 8);
        const std::uint32_t riff_size = 36 + data_bytes;

        std::string wav;
        wav.reserve(44 + data_bytes);
        auto append_u32 = [&](std::uint32_t v) {
            wav.push_back(static_cast<char>(v & 0xFF));
            wav.push_back(static_cast<char>((v >> 8) & 0xFF));
            wav.push_back(static_cast<char>((v >> 16) & 0xFF));
            wav.push_back(static_cast<char>((v >> 24) & 0xFF));
        };
        auto append_u16 = [&](std::uint16_t v) {
            wav.push_back(static_cast<char>(v & 0xFF));
            wav.push_back(static_cast<char>((v >> 8) & 0xFF));
        };
        wav.append("RIFF", 4);
        append_u32(riff_size);
        wav.append("WAVEfmt ", 8);
        append_u32(16);
        append_u16(1);
        append_u16(channels);
        append_u32(sample_rate);
        append_u32(byte_rate);
        append_u16(block_align);
        append_u16(bits);
        wav.append("data", 4);
        append_u32(data_bytes);
        for (std::uint32_t i = 0; i < num_samples; ++i) {
            std::int16_t s = static_cast<std::int16_t>((i % 100 < 50) ? 2000 : -2000);
            append_u16(static_cast<std::uint16_t>(s));
        }
        return wav;
    };
    const std::string wav_body = make_wav_bytes();
    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});

    // Helper: tiny local scriptable HTTP server for transcoder tests
    struct ScriptedHttpServer {
        asio::io_context io;
        asio::ip::tcp::acceptor acc{io};
        std::uint16_t port{0};
        std::atomic<bool> stop{false};
        std::atomic<int> req_count{0};
        std::mutex mtx;
        std::vector<std::shared_ptr<asio::ip::tcp::socket>> open_sockets;
        std::vector<std::thread> workers;
        std::thread srv_thread;

        void Start(std::function<void(int attempt_num, std::shared_ptr<asio::ip::tcp::socket> sock)> handler) {
            asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
            acc.open(ep.protocol());
            acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acc.bind(ep);
            acc.listen();
            port = acc.local_endpoint().port();
            srv_thread = std::thread([this, handler = std::move(handler)]() {
                while (!stop.load()) {
                    asio::error_code ec;
                    auto sock = std::make_shared<asio::ip::tcp::socket>(io);
                    acc.accept(*sock, ec);
                    if (ec || stop.load()) {
                        break;
                    }
                    const int num = req_count.fetch_add(1) + 1;
                    std::lock_guard<std::mutex> lk(mtx);
                    open_sockets.push_back(sock);
                    workers.emplace_back([this, sock, num, handler]() {
                        std::string req_hdrs;
                        std::array<char, 1024> buf{};
                        while (req_hdrs.find("\r\n\r\n") == std::string::npos) {
                            asio::error_code rec;
                            std::size_t n = sock->read_some(asio::buffer(buf), rec);
                            if (n > 0) req_hdrs.append(buf.data(), n);
                            if (rec) break;
                        }
                        handler(num, sock);
                    });
                }
            });
        }

        void Stop() {
            stop.store(true);
            {
                std::lock_guard<std::mutex> lk(mtx);
                for (auto& s : open_sockets) {
                    asio::error_code cec;
                    if (s && s->is_open()) {
                        s->shutdown(asio::ip::tcp::socket::shutdown_both, cec);
                        s->close(cec);
                    }
                }
            }
            StopTestAcceptor(acc, port);
            if (srv_thread.joinable()) srv_thread.join();
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& t : workers) {
                if (t.joinable()) t.join();
            }
            open_sockets.clear();
            workers.clear();
        }
    };

    auto hold_until_stopped = [](std::atomic<bool>& stop_flag, std::shared_ptr<asio::ip::tcp::socket> sock) {
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stop_flag.load() && std::chrono::steady_clock::now() < dl && sock->is_open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };

    auto rst_after_ms = [](std::atomic<bool>& stop_flag,
                           std::shared_ptr<asio::ip::tcp::socket> sock,
                           int delay_ms) {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (!stop_flag.load() && std::chrono::steady_clock::now() < dl && sock->is_open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        asio::error_code ec;
        sock->set_option(asio::socket_base::linger(true, 0), ec);
        sock->close(ec);
    };

    // 0. Verify production timing defaults & pure transient error classification (no network)
    TEST_PHASE("0. Production defaults and pure curl/WinHTTP transient error classification");
    {
        TranscoderConfig prod_def;
        TEST_CHECK(prod_def.stream_connect_timeout_ms == 15000);
        TEST_CHECK(prod_def.no_audio_timeout_ms == 20000);
        TEST_CHECK(prod_def.retry_backoff_first_ms == 500);
        TEST_CHECK(prod_def.retry_backoff_second_ms == 1500);

        for (int c : {28, 7, 6, 35, 52, 55, 56}) {
            TEST_CHECK(HttpClient::IsTransientCurlError(c));
        }
        for (int c : {0, 1, 3, 22, 23, 42, 47, 99}) {
            TEST_CHECK(!HttpClient::IsTransientCurlError(c));
        }

        for (unsigned long w : {12002UL, 12029UL, 12007UL, 12030UL, 12175UL, 12152UL}) {
            TEST_CHECK(HttpClient::IsTransientWinHttpError(w));
        }
        for (unsigned long w : {0UL, 5UL, 87UL, 12005UL, 12006UL, 12017UL, 12150UL}) {
            TEST_CHECK(!HttpClient::IsTransientWinHttpError(w));
        }
    }

    // 1. Transient RST once before first byte -> succeeds on 2nd request, same URL, 0 re-resolve
    TEST_PHASE("1. Single transient RST before first byte retries same URL once and succeeds");
    {
        clear_logs();
        ScriptedHttpServer srv;
        srv.Start([&](int attempt_num, std::shared_ptr<asio::ip::tcp::socket> sock) {
            if (attempt_num == 1) {
                rst_after_ms(srv.stop, sock, 100);
                return;
            }
            const std::string hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                std::to_string(wav_body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::error_code wec;
            asio::write(*sock, asio::buffer(hdr), wec);
            asio::write(*sock, asio::buffer(wav_body), wec);
        });

        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/track.wav";
        meta.source_type = SourceType::ExtractorStream;
        std::atomic<int> re_resolve_calls{0};
        TranscoderConfig cfg;
        cfg.target_sample_rate = 44100;
        cfg.target_bitrate_kbps = 128;
        cfg.pace_realtime = false;
        cfg.stream_connect_timeout_ms = 5000;
        cfg.retry_backoff_first_ms = 30;
        cfg.retry_backoff_second_ms = 80;
        cfg.no_audio_timeout_ms = 5000;
        cfg.re_resolve_callback = [&](const TrackMetadata&, const std::atomic<bool>&, TrackMetadata&, std::string&) -> bool {
            re_resolve_calls.fetch_add(1);
            return false;
        };

        auto buf = std::make_shared<AudioStreamBuffer>(512 * 1024);
        AudioTranscoder tc(meta, cfg, buf);
        std::atomic<bool> cancel{false};
        std::atomic<bool> pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        TEST_CHECK(tc.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(buf->TotalWritten() > 0);
        TEST_CHECK(srv.req_count.load() == 2);
        TEST_CHECK(re_resolve_calls.load() == 0);
        TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL (1/2)") == 1);
        srv.Stop();
    }

    // 2. Two transient RST failures, then accepted on 3rd attempt
    TEST_PHASE("2. Two transient RST failures before first byte, then succeeds on 3rd attempt");
    {
        clear_logs();
        ScriptedHttpServer srv;
        srv.Start([&](int attempt_num, std::shared_ptr<asio::ip::tcp::socket> sock) {
            if (attempt_num <= 2) {
                rst_after_ms(srv.stop, sock, 100);
                return;
            }
            const std::string hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                std::to_string(wav_body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::error_code wec;
            asio::write(*sock, asio::buffer(hdr), wec);
            asio::write(*sock, asio::buffer(wav_body), wec);
        });

        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/retry2.wav";
        meta.source_type = SourceType::DirectAudio;
        TranscoderConfig cfg;
        cfg.target_sample_rate = 44100;
        cfg.pace_realtime = false;
        cfg.stream_connect_timeout_ms = 5000;
        cfg.retry_backoff_first_ms = 30;
        cfg.retry_backoff_second_ms = 80;
        cfg.no_audio_timeout_ms = 5000;
        auto buf = std::make_shared<AudioStreamBuffer>(512 * 1024);
        AudioTranscoder tc(meta, cfg, buf);
        std::atomic<bool> cancel{false};
        std::atomic<bool> pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        TEST_CHECK(tc.Run(cancel, pause, vol, err, &ec));
        TEST_CHECK(buf->TotalWritten() > 0);
        TEST_CHECK(srv.req_count.load() == 3);
        TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL (1/2)") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL (2/2)") == 1);
        srv.Stop();
    }

    // 3. All 3 attempts fail -> final error reports "after 3 attempts" within bounded duration
    TEST_PHASE("3. Three consecutive transient RSTs fail with 'after 3 attempts'");
    {
        clear_logs();
        ScriptedHttpServer srv;
        srv.Start([&](int, std::shared_ptr<asio::ip::tcp::socket> sock) {
            rst_after_ms(srv.stop, sock, 100);
        });

        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/hang3.wav";
        meta.source_type = SourceType::DirectAudio;
        TranscoderConfig cfg;
        cfg.target_sample_rate = 44100;
        cfg.pace_realtime = false;
        cfg.stream_connect_timeout_ms = 5000;
        cfg.retry_backoff_first_ms = 30;
        cfg.retry_backoff_second_ms = 80;
        cfg.no_audio_timeout_ms = 5000;
        auto buf = std::make_shared<AudioStreamBuffer>(512 * 1024);
        AudioTranscoder tc(meta, cfg, buf);
        std::atomic<bool> cancel{false};
        std::atomic<bool> pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        const auto t0 = std::chrono::steady_clock::now();
        TEST_CHECK(!tc.Run(cancel, pause, vol, err, &ec));
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        TEST_CHECK(ec == ErrorCode::NetworkError);
        TEST_CHECK(srv.req_count.load() == 3);
        TEST_CHECK(err.find("after 3 attempts") != std::string::npos);
        TEST_CHECK(elapsed_ms >= 300 && elapsed_ms < 4000);
        srv.Stop();
    }

    // 4. Zero retry for HTTP 403, HTTP 404, URL not allowed, and cancellation
    TEST_PHASE("4. Zero retries on HTTP 403, HTTP 404, URL not allowed, and cancellation");
    {
        for (int status_to_send : {403, 404}) {
            clear_logs();
            ScriptedHttpServer srv;
            srv.Start([&, status_to_send](int, std::shared_ptr<asio::ip::tcp::socket> sock) {
                const std::string hdr =
                    "HTTP/1.1 " + std::to_string(status_to_send) +
                    " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                asio::error_code wec;
                asio::write(*sock, asio::buffer(hdr), wec);
            });
            TrackMetadata meta;
            meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/err.wav";
            meta.source_type = SourceType::DirectAudio;
            TranscoderConfig cfg;
            cfg.stream_connect_timeout_ms = 5000;
            cfg.no_audio_timeout_ms = 5000;
            auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
            AudioTranscoder tc(meta, cfg, buf);
            std::atomic<bool> cancel{false}, pause{false};
            std::atomic<float> vol{100.0f};
            std::string err;
            ErrorCode ec = ErrorCode::None;
            TEST_CHECK(!tc.Run(cancel, pause, vol, err, &ec));
            TEST_CHECK(srv.req_count.load() == 1);
            TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL") == 0);
            srv.Stop();
        }

        // SSRF blocked ("URL not allowed")
        clear_logs();
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{false, "0.0.0.0", "203.0.113.10", 7778});
        TrackMetadata ssrf_meta;
        ssrf_meta.direct_media_url = "http://127.0.0.1:19999/blocked.wav";
        ssrf_meta.source_type = SourceType::DirectAudio;
        TranscoderConfig ssrf_cfg;
        auto sbuf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        AudioTranscoder stc(ssrf_meta, ssrf_cfg, sbuf);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string serr;
        ErrorCode sec = ErrorCode::None;
        TEST_CHECK(!stc.Run(cancel, pause, vol, serr, &sec));
        TEST_CHECK(sec == ErrorCode::InvalidUrl);
        TEST_CHECK(serr == "URL not allowed");
        TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL") == 0);
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});

        // Cancel during initial hung attempt -> zero retries
        clear_logs();
        ScriptedHttpServer srv_c;
        srv_c.Start([&](int, std::shared_ptr<asio::ip::tcp::socket> sock) {
            hold_until_stopped(srv_c.stop, sock);
        });
        TrackMetadata cmeta;
        cmeta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv_c.port) + "/cancel0.wav";
        cmeta.source_type = SourceType::DirectAudio;
        TranscoderConfig ccfg;
        ccfg.stream_connect_timeout_ms = 5000;
        ccfg.retry_backoff_first_ms = 50;
        ccfg.retry_backoff_second_ms = 150;
        ccfg.no_audio_timeout_ms = 5000;
        auto cbuf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        AudioTranscoder ctc(cmeta, ccfg, cbuf);
        std::atomic<bool> c_cancel{false}, c_pause{false};
        std::atomic<float> c_vol{100.0f};
        std::string cerr_str;
        std::thread killer([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            c_cancel.store(true);
            HttpClient::PollCancellations();
        });
        TEST_CHECK(ctc.Run(c_cancel, c_pause, c_vol, cerr_str, nullptr));
        killer.join();
        TEST_CHECK(srv_c.req_count.load() == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "retrying same URL") == 0);
        srv_c.Stop();
    }

    // 5. Cancellation during retry backoff wait stops immediately with zero callbacks on AudioEngine
    TEST_PHASE("5. Cancellation during retry backoff terminates cleanly without extra requests or callbacks");
    {
        clear_logs();
        ScriptedHttpServer srv;
        srv.Start([&](int, std::shared_ptr<asio::ip::tcp::socket> sock) {
            rst_after_ms(srv.stop, sock, 100);
        });

        const std::filesystem::path temp_b5 =
            std::filesystem::temp_directory_path() / "sampaudiostream_r53_cancel_backoff";
        std::error_code fec;
        std::filesystem::remove_all(temp_b5, fec);
        std::filesystem::create_directories(temp_b5, fec);

        MockPlatformHost host;
        host.players = {{1, {0.0f, 0.0f, 0.0f}, 0, 0}};
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.base_dir = temp_b5.string();
        cfg.bin_dir = (temp_b5 / "bin").string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = false;
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = false;
        cfg.debug_logging = true;
        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());

        // Test AudioTranscoder directly on backoff cancel first, plus verify StopAudioStreamForPlayer aborts without error callbacks.
        std::atomic<bool> cancel{false}, pause{false}, stop_flush5{false};
        std::atomic<std::int64_t> cancelled_at_ns{0};
        Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
            {
                std::lock_guard<std::mutex> lk(log_mtx);
                logs.emplace_back(sev, msg);
            }
            if (msg.find("retrying same URL (1/2)") != std::string::npos) {
                cancelled_at_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                cancel.store(true);
            }
        });
        std::thread flush_pump5([&]() {
            while (!stop_flush5.load()) {
                Logger::Instance().Flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        });
        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/backoff.wav";
        meta.source_type = SourceType::DirectAudio;
        TranscoderConfig tcfg;
        tcfg.stream_connect_timeout_ms = 5000;
        tcfg.retry_backoff_first_ms = 250;
        tcfg.retry_backoff_second_ms = 100;
        tcfg.no_audio_timeout_ms = 5000;
        auto buf = std::make_shared<AudioStreamBuffer>(256 * 1024);
        AudioTranscoder tc(meta, tcfg, buf);
        std::atomic<float> vol{100.0f};
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok5 = tc.Run(cancel, pause, vol, err, nullptr);
        const auto done_tp = std::chrono::steady_clock::now();
        stop_flush5.store(true);
        if (flush_pump5.joinable()) flush_pump5.join();
        TEST_CHECK(ok5);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(done_tp - t0).count();
        const std::int64_t cancel_ns = cancelled_at_ns.load();
        TEST_CHECK(cancel_ns > 0);
        const auto since_cancel_ms =
            (std::chrono::duration_cast<std::chrono::nanoseconds>(done_tp.time_since_epoch()).count() - cancel_ns) / 1000000;
        (void)ms;
        TEST_CHECK(srv.req_count.load() == 1);
        TEST_CHECK(since_cancel_ms < 250);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.error_events.empty());
            TEST_CHECK(host.ready_stream_ids.empty());
        }

        Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
            std::lock_guard<std::mutex> lk(log_mtx);
            logs.emplace_back(sev, msg);
        });
        engine.Shutdown();
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});
        std::filesystem::remove_all(temp_b5, fec);
        srv.Stop();
    }

    // 6. Short no_audio_timeout_ms does NOT abort before 3rd attempt when two transient failures occur
    TEST_PHASE("6. Watchdog resets across transient retries so 3rd attempt succeeds");
    {
        clear_logs();
        ScriptedHttpServer srv;
        srv.Start([&](int attempt_num, std::shared_ptr<asio::ip::tcp::socket> sock) {
            if (attempt_num <= 2) {
                rst_after_ms(srv.stop, sock, 700);
                return;
            }
            const std::string hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                std::to_string(wav_body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::error_code wec;
            asio::write(*sock, asio::buffer(hdr), wec);
            asio::write(*sock, asio::buffer(wav_body), wec);
        });

        TrackMetadata meta;
        meta.direct_media_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/wd.wav";
        meta.source_type = SourceType::DirectAudio;
        TranscoderConfig cfg;
        cfg.target_sample_rate = 44100;
        cfg.pace_realtime = false;
        cfg.stream_connect_timeout_ms = 5000;
        cfg.retry_backoff_first_ms = 30;
        cfg.retry_backoff_second_ms = 80;
        // One attempt (~700 ms) < 1200 ms, while two consecutive attempts (~1430 ms) would abort without epoch reset.
        cfg.no_audio_timeout_ms = 1200;
        auto buf = std::make_shared<AudioStreamBuffer>(512 * 1024);
        AudioTranscoder tc(meta, cfg, buf);
        std::atomic<bool> cancel{false}, pause{false};
        std::atomic<float> vol{100.0f};
        std::string err;
        ErrorCode ec = ErrorCode::None;
        const bool ok6 = tc.Run(cancel, pause, vol, err, &ec);
        TEST_CHECK(ok6);
        TEST_CHECK(srv.req_count.load() == 3);
        TEST_CHECK(buf->TotalWritten() > 0);
        srv.Stop();
    }

    // 7. Firewall / unconnected client warning (Ajuste 3)
    TEST_PHASE("7. Unconnected client Warn: fires when unconnected, silent when connected/stopped/passthrough");
    {
        ScriptedHttpServer media_srv;
        media_srv.Start([&](int, std::shared_ptr<asio::ip::tcp::socket> sock) {
            const std::string hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                std::to_string(wav_body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::error_code wec;
            asio::write(*sock, asio::buffer(hdr), wec);
            asio::write(*sock, asio::buffer(wav_body), wec);
        });

        const std::filesystem::path temp_b7 =
            std::filesystem::temp_directory_path() / "sampaudiostream_r53_port_warn";
        std::error_code fec;
        std::filesystem::remove_all(temp_b7, fec);
        std::filesystem::create_directories(temp_b7, fec);

        auto make_engine_cfg = [&]() {
            PluginConfig c;
            c.bind_ip = "127.0.0.1";
            c.public_ip = "127.0.0.1";
            c.http_port = 0;
            c.base_dir = temp_b7.string();
            c.bin_dir = (temp_b7 / "bin").string();
            c.auto_download_extractor = false;
            c.auto_update_extractor = false;
            c.enable_js_runtime = false;
            c.allow_private_network_urls = true;
            c.persist_channels = false;
            c.debug_logging = false;
            return c;
        };

        auto pump_until = [](AudioEngine& eng, int ms, std::function<bool()> pred = nullptr) {
            const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < dl) {
                eng.Tick();
                if (pred && pred()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            return pred ? pred() : false;
        };

        const std::string wav_url = "http://127.0.0.1:" + std::to_string(media_srv.port) + "/fw.wav";

        // 7b: Client connects -> zero Warn
        {
            clear_logs();
            Logger::Instance().SetDebugEnabled(false);
            MockPlatformHost host;
            host.players = {{5, {0.0f, 0.0f, 0.0f}, 0, 0}};
            AudioEngine engine(host, make_engine_cfg());
            engine.SetConnectTimeoutWarningMsForTesting(150);
            TEST_CHECK(engine.Initialize());

            StreamOptions opts;
            opts.process_direct_audio_files = true;
            TEST_CHECK(engine.PlayAudioStreamForPlayer(5, wav_url, opts) != INVALID_STREAM_ID);

            std::string delivered_url;
            TEST_CHECK(pump_until(engine, 2000, [&]() {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& ev : host.events) {
                    if (ev.play && ev.player_id == 5 && ev.url.find("/stream/") != std::string::npos) {
                        delivered_url = ev.url;
                        return true;
                    }
                }
                return false;
            }));

            // Connect a real raw TCP socket (simulating GTA client; avoids plugin's own self-loopback HttpClient guard)
            const auto slash_pos = delivered_url.find("/stream/");
            TEST_CHECK(slash_pos != std::string::npos);
            const std::string target_path = delivered_url.substr(slash_pos);
            bool client_connected = false;
            {
                asio::io_context cio;
                asio::ip::tcp::socket csock(cio);
                csock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), engine.GetHttpPort()));
                const std::string req = "GET " + target_path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
                asio::write(csock, asio::buffer(req));
                std::array<char, 256> hdr_buf{};
                asio::error_code rec;
                const std::size_t n = csock.read_some(asio::buffer(hdr_buf), rec);
                const std::string resp_head(hdr_buf.data(), n);
                client_connected = (resp_head.find("200") != std::string::npos);
            }
            TEST_CHECK(client_connected);
            pump_until(engine, 250);
            TEST_CHECK(count_logs(LogSeverity::Warning, "did not connect to the audio stream") == 0);
            engine.Shutdown();
        }

        // 7c: Stopped before timeout -> zero Warn
        {
            clear_logs();
            Logger::Instance().SetDebugEnabled(false);
            MockPlatformHost host;
            host.players = {{5, {0.0f, 0.0f, 0.0f}, 0, 0}};
            AudioEngine engine(host, make_engine_cfg());
            engine.SetConnectTimeoutWarningMsForTesting(150);
            TEST_CHECK(engine.Initialize());

            StreamOptions opts;
            opts.process_direct_audio_files = true;
            TEST_CHECK(engine.PlayAudioStreamForPlayer(5, wav_url, opts) != INVALID_STREAM_ID);
            TEST_CHECK(pump_until(engine, 2000, [&]() {
                std::lock_guard<std::mutex> lk(host.mtx);
                return !host.events.empty();
            }));
            engine.StopAudioStreamForPlayer(5);
            pump_until(engine, 220);
            TEST_CHECK(count_logs(LogSeverity::Warning, "did not connect to the audio stream") == 0);
            engine.Shutdown();
        }

        // 7d: Direct HTTP passthrough URL -> zero Warn
        {
            clear_logs();
            Logger::Instance().SetDebugEnabled(false);
            MockPlatformHost host;
            host.players = {{5, {0.0f, 0.0f, 0.0f}, 0, 0}};
            AudioEngine engine(host, make_engine_cfg());
            engine.SetConnectTimeoutWarningMsForTesting(80);
            TEST_CHECK(engine.Initialize());

            StreamOptions opts;
            opts.process_direct_audio_files = false;
            TEST_CHECK(engine.PlayAudioStreamForPlayer(5, wav_url, opts) != INVALID_STREAM_ID);
            pump_until(engine, 160);
            TEST_CHECK(count_logs(LogSeverity::Warning, "did not connect to the audio stream") == 0);
            engine.Shutdown();
        }

        // 7a: Unconnected proxied stream -> emits Warn even with debug OFF, omits token/URL
        {
            clear_logs();
            Logger::Instance().SetDebugEnabled(false);
            MockPlatformHost host;
            host.players = {{5, {0.0f, 0.0f, 0.0f}, 0, 0}};
            AudioEngine engine(host, make_engine_cfg());
            engine.SetConnectTimeoutWarningMsForTesting(150);
            TEST_CHECK(engine.Initialize());

            StreamOptions opts;
            opts.process_direct_audio_files = true;
            TEST_CHECK(engine.PlayAudioStreamForPlayer(5, wav_url, opts) != INVALID_STREAM_ID);
            std::string delivered_url;
            TEST_CHECK(pump_until(engine, 2000, [&]() {
                std::lock_guard<std::mutex> lk(host.mtx);
                for (const auto& ev : host.events) {
                    if (ev.play && ev.player_id == 5 && ev.url.find("/stream/") != std::string::npos) {
                        delivered_url = ev.url;
                        return true;
                    }
                }
                return false;
            }));

            TEST_CHECK(pump_until(engine, 1000, [&]() {
                return count_logs(LogSeverity::Warning, "Player 5 did not connect to the audio stream within 15 s") >= 1;
            }));
            TEST_CHECK(pump_until(engine, 500, [&]() {
                std::lock_guard<std::mutex> lk(host.mtx);
                return !host.no_connect_events.empty();
            }));
            TEST_CHECK(count_logs(LogSeverity::Warning, "common causes") == 1);
            const std::string port_tok = "(HTTP port " + std::to_string(engine.GetHttpPort()) + "/TCP)";
            TEST_CHECK(count_logs(LogSeverity::Warning, port_tok) == 1);
            TEST_CHECK(count_logs(LogSeverity::Warning, "'network.public_ip'") == 1);
            TEST_CHECK(count_logs(LogSeverity::Warning, "/stream/") == 0);
            engine.Shutdown();
        }

        // 7e: Two unconnected players emit two OnPlayerAudioStreamNoConnect callbacks and a single throttled Warn
        {
            clear_logs();
            Logger::Instance().SetDebugEnabled(false);
            MockPlatformHost host;
            host.players = {{5, {0.0f, 0.0f, 0.0f}, 0, 0}, {6, {0.0f, 0.0f, 0.0f}, 0, 0}};
            AudioEngine engine(host, make_engine_cfg());
            engine.SetConnectTimeoutWarningMsForTesting(150);
            TEST_CHECK(engine.Initialize());

            StreamOptions opts;
            opts.process_direct_audio_files = true;
            const StreamId sid5 = engine.PlayAudioStreamForPlayer(5, wav_url, opts);
            const StreamId sid6 = engine.PlayAudioStreamForPlayer(6, wav_url, opts);
            TEST_CHECK(sid5 != INVALID_STREAM_ID && sid6 != INVALID_STREAM_ID);

            TEST_CHECK(pump_until(engine, 2500, [&]() {
                std::lock_guard<std::mutex> lk(host.mtx);
                return host.no_connect_events.size() >= 2;
            }));
            {
                std::lock_guard<std::mutex> lk(host.mtx);
                TEST_CHECK(host.no_connect_events.size() == 2);
                bool seen5 = false;
                bool seen6 = false;
                for (const auto& ev : host.no_connect_events) {
                    if (ev.player_id == 5 && ev.source_type == static_cast<std::int32_t>(AudioStreamSourceType::Personal) && ev.source_id == sid5) {
                        seen5 = true;
                    } else if (ev.player_id == 6 && ev.source_type == static_cast<std::int32_t>(AudioStreamSourceType::Personal) && ev.source_id == sid6) {
                        seen6 = true;
                    }
                }
                TEST_CHECK(seen5 && seen6);
            }
            TEST_CHECK(count_logs(LogSeverity::Warning, "did not connect to the audio stream within 15 s") == 1);
            engine.Shutdown();
        }

        std::filesystem::remove_all(temp_b7, fec);
        media_srv.Stop();
    }

    // 8. Transport stage debug log includes error/timings/ip/host and NEVER leaks URL query string
    TEST_PHASE("8. Debug transport log includes stage/error/timings/host and excludes query string");
    {
        clear_logs();
        Logger::Instance().SetDebugEnabled(true);
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "127.0.0.1", "127.0.0.1", 0});
        asio::io_context port_io;
        asio::ip::tcp::acceptor probe_acc(port_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const std::uint16_t closed_port = probe_acc.local_endpoint().port();
        probe_acc.close();

        HttpRequestDiagnostics diag;
        std::string err;
        std::atomic<bool> cancel{false};
        const std::string secret_url =
            "http://127.0.0.1:" + std::to_string(closed_port) + "/stream.webm?sig=SECRET_QUERY_TOKEN_53";
        TEST_CHECK(!HttpClient::StreamGet(
            secret_url,
            {{"Authorization", "Bearer SECRET_HEADER_53"}},
            [](const std::uint8_t*, std::size_t) { return true; },
            cancel,
            err,
            nullptr,
            5000,
            &diag));
        TEST_CHECK(diag.transient_transport_error);
        TEST_CHECK(!any_log_contains("SECRET_QUERY_TOKEN_53"));
        TEST_CHECK(!any_log_contains("SECRET_HEADER_53"));
#ifdef _WIN32
        TEST_CHECK(count_logs(LogSeverity::Debug, "HTTP transport failed (winhttp):") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "stage=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "host=127.0.0.1") == 1);
#else
        TEST_CHECK(count_logs(LogSeverity::Debug, "HTTP transport failed: curl error") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "dns=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "connect=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "tls=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "first_byte=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "total=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "ip=") == 1);
        TEST_CHECK(count_logs(LogSeverity::Debug, "host=127.0.0.1") == 1);
#endif
    }

    std::cout << "[PASS] TestRound53NetworkRetryAndPortWarn\n";
}

void TestRound54HttpConnectionLimits() {
    struct EnvGuard {
        bool prev_debug{false};
        EnvGuard() : prev_debug(Logger::Instance().IsDebugEnabled()) {}
        ~EnvGuard() {
            Logger::Instance().Flush();
            Logger::Instance().SetDebugEnabled(prev_debug);
            Logger::Instance().SetSink(nullptr);
        }
    } env_guard;

    std::mutex log_mtx;
    std::vector<std::pair<LogSeverity, std::string>> logs;
    Logger::Instance().SetDebugEnabled(false);
    Logger::Instance().SetSink([&](LogSeverity sev, const std::string& msg) {
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.emplace_back(sev, msg);
    });
    auto clear_logs = [&]() {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        logs.clear();
    };
    auto count_logs = [&](LogSeverity sev, const std::string& needle) -> int {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        int n = 0;
        for (const auto& kv : logs) {
            if (kv.first == sev && kv.second.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    };
    auto any_log_contains = [&](const std::string& needle) -> bool {
        Logger::Instance().Flush();
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& kv : logs) {
            if (kv.second.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    auto wait_until = [](std::function<bool()> pred, int max_ms = 2000) -> bool {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
        while (std::chrono::steady_clock::now() < dl) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        return pred();
    };

    // 1. Defaults & out-of-range fallback warnings
    TEST_PHASE("1. Config defaults (1024 / 64) and out-of-range validation warnings");
    {
        PluginConfig def;
        TEST_CHECK(def.max_http_connections == 1024);
        TEST_CHECK(def.max_connections_per_ip == 64);

        clear_logs();
        PluginConfig c_low = def;
        c_low.max_http_connections = 63;
        c_low.max_connections_per_ip = 0;
        ConfigManager::Sanitize(c_low);
        TEST_CHECK(c_low.max_http_connections == 1024);
        TEST_CHECK(c_low.max_connections_per_ip == 64);
        TEST_CHECK(count_logs(LogSeverity::Warning, "Config key 'network.max_http_connections'") == 1);
        TEST_CHECK(count_logs(LogSeverity::Warning, "Config key 'network.max_connections_per_ip'") == 1);

        clear_logs();
        PluginConfig c_high = def;
        c_high.max_http_connections = 8193;
        c_high.max_connections_per_ip = 4097;
        ConfigManager::Sanitize(c_high);
        TEST_CHECK(c_high.max_http_connections == 1024);
        TEST_CHECK(c_high.max_connections_per_ip == 64);
        TEST_CHECK(count_logs(LogSeverity::Warning, "Config key 'network.max_http_connections'") == 1);
        TEST_CHECK(count_logs(LogSeverity::Warning, "Config key 'network.max_connections_per_ip'") == 1);
    }

    auto open_streaming_client = [](asio::io_context& io,
                                    std::uint16_t port,
                                    const std::string& path,
                                    std::unique_ptr<asio::ip::tcp::socket>& out_sock) -> bool {
        out_sock = std::make_unique<asio::ip::tcp::socket>(io);
        asio::error_code ec;
        out_sock->connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        if (ec) return false;
        const std::string req = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        asio::write(*out_sock, asio::buffer(req), ec);
        if (ec) return false;
        std::array<char, 512> buf{};
        std::string hdr;
        while (hdr.find("\r\n\r\n") == std::string::npos) {
            const std::size_t n = out_sock->read_some(asio::buffer(buf), ec);
            if (n > 0) hdr.append(buf.data(), n);
            if (ec) break;
        }
        return hdr.find("HTTP/1.1 200 OK") != std::string::npos;
    };

    // 2. Per-IP limit = 2: 3rd rejected + 1 Warn even with debug OFF; 4th within 1 min emits 0 new Warns
    TEST_PHASE("2. Per-IP connection limit rejects 3rd/4th with rate-limited Warn");
    {
        clear_logs();
        Logger::Instance().SetDebugEnabled(false);

        HttpStreamingServer srv;
        srv.SetConnectionLimits(10, 2);
        std::string err;
        TEST_CHECK(srv.Start("127.0.0.1", 0, err, false));

        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        const std::array<std::uint8_t, 32> frame{};
        buf->Write(frame.data(), frame.size());
        srv.RegisterEndpoint(StreamEndpointInfo{
            "secret_tok_ip", "Title", "Artist", 128, false, 48 * 1024, buf, "Station"});

        asio::io_context io;
        std::unique_ptr<asio::ip::tcp::socket> c1, c2, c3, c4;
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_ip.mp3", c1));
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_ip.mp3", c2));
        TEST_CHECK(wait_until([&]() { return srv.GetActiveConnectionCount("secret_tok_ip") == 2; }));

        TEST_CHECK(!open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_ip.mp3", c3));
        const std::string expected_warn =
            "HTTP audio connection from 127.0.0.1 rejected: per-IP limit reached (2/2). "
            "Players sharing this IP (mobile carrier CGNAT, LAN house) beyond this limit hear nothing; "
            "raise 'network.max_connections_per_ip' if needed.";
        TEST_CHECK(wait_until([&]() { return count_logs(LogSeverity::Warning, expected_warn) == 1; }));
        TEST_CHECK(!any_log_contains("secret_tok_ip"));

        TEST_CHECK(!open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_ip.mp3", c4));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        TEST_CHECK(count_logs(LogSeverity::Warning, expected_warn) == 1);

        c1->close();
        TEST_CHECK(wait_until([&]() { return srv.GetActiveConnectionCount("secret_tok_ip") == 1; }));
        std::unique_ptr<asio::ip::tcp::socket> c5;
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_ip.mp3", c5));

        buf->Close();
        srv.Stop();
    }

    // 3. Total limit = 3: 4th rejected + 1 Warn; 5th within 1 min emits 0 new Warns
    TEST_PHASE("3. Total connection limit rejects 4th/5th with rate-limited Warn");
    {
        clear_logs();
        Logger::Instance().SetDebugEnabled(false);

        HttpStreamingServer srv;
        srv.SetConnectionLimits(3, 64);
        std::string err;
        TEST_CHECK(srv.Start("127.0.0.1", 0, err, false));

        auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
        const std::array<std::uint8_t, 32> frame{};
        buf->Write(frame.data(), frame.size());
        srv.RegisterEndpoint(StreamEndpointInfo{
            "secret_tok_tot", "Title", "Artist", 128, false, 48 * 1024, buf, "Station"});

        asio::io_context io;
        std::unique_ptr<asio::ip::tcp::socket> c1, c2, c3, c4, c5;
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_tot.mp3", c1));
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_tot.mp3", c2));
        TEST_CHECK(open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_tot.mp3", c3));
        TEST_CHECK(wait_until([&]() { return srv.GetActiveConnectionCount("secret_tok_tot") == 3; }));

        TEST_CHECK(!open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_tot.mp3", c4));
        const std::string expected_warn =
            "HTTP audio connection rejected: total connection limit reached (3/3); "
            "raise 'network.max_http_connections' if more simultaneous listeners are expected.";
        TEST_CHECK(wait_until([&]() { return count_logs(LogSeverity::Warning, expected_warn) == 1; }));
        TEST_CHECK(!any_log_contains("secret_tok_tot"));

        TEST_CHECK(!open_streaming_client(io, srv.BoundPort(), "/stream/secret_tok_tot.mp3", c5));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        TEST_CHECK(count_logs(LogSeverity::Warning, expected_warn) == 1);

        buf->Close();
        srv.Stop();
    }

    // 4. AudioEngine applies PluginConfig limits to HttpStreamingServer
    TEST_PHASE("4. AudioEngine passes PluginConfig connection limits to HttpStreamingServer");
    {
        clear_logs();
        Logger::Instance().SetDebugEnabled(false);

        const std::string wav_body = BuildTestWavBytes(44100, 1500);
        asio::io_context srv_io;
        asio::ip::tcp::acceptor media_acc(
            srv_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const std::uint16_t media_port = media_acc.local_endpoint().port();
        std::atomic<bool> media_stop{false};
        std::thread media_thr([&]() {
            while (!media_stop.load()) {
                asio::error_code aec;
                asio::ip::tcp::socket msock(srv_io);
                media_acc.accept(msock, aec);
                if (aec || media_stop.load()) break;
                std::string req;
                std::array<char, 512> tmp{};
                while (req.find("\r\n\r\n") == std::string::npos) {
                    asio::error_code rec;
                    const std::size_t n = msock.read_some(asio::buffer(tmp), rec);
                    if (n > 0) req.append(tmp.data(), n);
                    if (rec) break;
                }
                const std::string hdr =
                    "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                    std::to_string(wav_body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::error_code wec;
                asio::write(msock, asio::buffer(hdr), wec);
                asio::write(msock, asio::buffer(wav_body), wec);
            }
        });

        const std::filesystem::path temp_r54 =
            std::filesystem::temp_directory_path() / "sampaudiostream_r54_conn_limits";
        std::error_code fec;
        std::filesystem::remove_all(temp_r54, fec);
        std::filesystem::create_directories(temp_r54, fec);

        MockPlatformHost host;
        host.players = {{7, {0.0f, 0.0f, 0.0f}, 0, 0}};
        PluginConfig cfg;
        cfg.bind_ip = "127.0.0.1";
        cfg.public_ip = "127.0.0.1";
        cfg.http_port = 0;
        cfg.base_dir = temp_r54.string();
        cfg.bin_dir = (temp_r54 / "bin").string();
        cfg.auto_download_extractor = false;
        cfg.auto_update_extractor = false;
        cfg.enable_js_runtime = false;
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = false;
        cfg.debug_logging = false;
        cfg.max_http_connections = 3;
        cfg.max_connections_per_ip = 2;

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());
        ChannelId ch = engine.CreateChannel("LimitChan", 7);
        TEST_CHECK(ch != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.SetChannelLoopMode(ch, ChannelLoopMode::Track));
        TEST_CHECK(engine.JoinChannel(7, ch));
        TEST_CHECK(engine.PlayChannel(ch, "http://127.0.0.1:" + std::to_string(media_port) + "/chan.wav"));

        std::string stream_path;
        TEST_CHECK(wait_until([&]() {
            engine.Tick();
            std::lock_guard<std::mutex> lk(host.mtx);
            for (const auto& ev : host.events) {
                if (ev.play && ev.player_id == 7) {
                    const auto p = ev.url.find("/channel/");
                    if (p != std::string::npos) {
                        stream_path = ev.url.substr(p);
                        return true;
                    }
                }
            }
            return false;
        }, 5000));

        asio::io_context cio;
        std::unique_ptr<asio::ip::tcp::socket> s1, s2, s3;
        TEST_CHECK(open_streaming_client(cio, engine.GetHttpPort(), stream_path, s1));
        TEST_CHECK(open_streaming_client(cio, engine.GetHttpPort(), stream_path, s2));
        TEST_CHECK(!open_streaming_client(cio, engine.GetHttpPort(), stream_path, s3));
        TEST_CHECK(wait_until([&]() {
            return count_logs(LogSeverity::Warning, "per-IP limit reached (2/2)") == 1;
        }));

        engine.Shutdown();
        media_stop.store(true);
        StopTestAcceptor(media_acc, media_port);
        if (media_thr.joinable()) media_thr.join();
        std::filesystem::remove_all(temp_r54, fec);
    }

    std::cout << "[PASS] TestRound54HttpConnectionLimits\n";
}

void TestRound63Features() {
    std::error_code fec;
    const std::filesystem::path r63_dir =
        std::filesystem::temp_directory_path() / "sampaudiostream_r63_tests";
    std::filesystem::remove_all(r63_dir, fec);
    std::filesystem::create_directories(r63_dir, fec);

    // 1. ExtractIcyStreamTitle
    TEST_PHASE("1. ExtractIcyStreamTitle parsing, encoding, truncation, and control-char stripping");
    {
        std::string title;
        TEST_CHECK(ExtractIcyStreamTitle("StreamTitle='Simple Title';", title));
        TEST_CHECK(title == "Simple Title");

        // Artist - Title with cp1252 accents -> converted to UTF-8
        const std::string cp1252_icy = "StreamTitle='Jo\xE3o - Can\xE7\xE3o';StreamUrl='';";
        TEST_CHECK(ExtractIcyStreamTitle(cp1252_icy, title));
        TEST_CHECK(title == "Jo\xC3\xA3o - Can\xC3\xA7\xC3\xA3o");

        // Already valid UTF-8 stays valid UTF-8
        TEST_CHECK(ExtractIcyStreamTitle("StreamTitle='Caf\xC3\xA9 - M\xC3\xBAsica';", title));
        TEST_CHECK(title == "Caf\xC3\xA9 - M\xC3\xBAsica");

        // Empty title returns true with empty output
        TEST_CHECK(ExtractIcyStreamTitle("StreamTitle='';", title));
        TEST_CHECK(title.empty());

        // Missing closing delimiter returns false
        TEST_CHECK(!ExtractIcyStreamTitle("StreamTitle='Incomplete", title));
        TEST_CHECK(!ExtractIcyStreamTitle("NoMarker='Value';", title));

        // > 256 UTF-8 bytes truncated cleanly without splitting multibyte sequences
        std::string long_raw = "StreamTitle='";
        for (int i = 0; i < 140; ++i) {
            long_raw += "\xC3\xA9"; // 2-byte UTF-8 sequence (280 bytes total)
        }
        long_raw += "';";
        TEST_CHECK(ExtractIcyStreamTitle(long_raw, title));
        TEST_CHECK(title.size() == 256);
        TEST_CHECK((title.size() % 2) == 0);

        // Control chars stripped (except normal space)
        TEST_CHECK(ExtractIcyStreamTitle("StreamTitle='One\x01\r\nTwo\tThree\x7F';", title));
        TEST_CHECK(title == "OneTwoThree");
    }

    // 2. IsLiveTitlePollCandidate
    TEST_PHASE("2. IsLiveTitlePollCandidate filter");
    {
        TrackMetadata mp3_icecast;
        mp3_icecast.extractor_name = "direct";
        mp3_icecast.direct_media_url = "https://radio.example.com/stream.mp3";
        mp3_icecast.is_live = false;
        TEST_CHECK(IsLiveTitlePollCandidate(mp3_icecast, 30));

        TrackMetadata extensionless;
        extensionless.extractor_name = "direct";
        extensionless.direct_media_url = "https://radio.example.com:8000/mount";
        extensionless.is_live = true;
        TEST_CHECK(IsLiveTitlePollCandidate(extensionless, 30));

        TrackMetadata yt_live;
        yt_live.extractor_name = "youtube";
        yt_live.direct_media_url = "https://cdn.example.com/live";
        yt_live.is_live = true;
        TEST_CHECK(!IsLiveTitlePollCandidate(yt_live, 30));

        TrackMetadata hls;
        hls.extractor_name = "direct";
        hls.direct_media_url = "https://radio.example.com/playlist.m3u8?token=1";
        hls.is_live = true;
        TEST_CHECK(!IsLiveTitlePollCandidate(hls, 30));

        TEST_CHECK(!IsLiveTitlePollCandidate(mp3_icecast, 0));
    }

    // 3 & 5c. FetchIcyStreamTitle against HttpStreamingServer & BuildIcyMetaFrame with live_title
    TEST_PHASE("3/5c. FetchIcyStreamTitle live ICY endpoints, live_title framing, UpdateEndpointMetadata clear");
    {
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "0.0.0.0", "203.0.113.10", 0});
        HttpStreamingServer srv;
        std::string err;
        TEST_CHECK(srv.Start("127.0.0.1", 0, err, false));

        auto buf = std::make_shared<AudioStreamBuffer>(128 * 1024);
        std::vector<std::uint8_t> payload(36000, 0x55);
        buf->Write(payload.data(), payload.size());

        StreamEndpointInfo ep;
        ep.token = "r63_icy";
        ep.title = "Title A";
        ep.artist = "";
        ep.bitrate_kbps = 128;
        ep.buffer = buf;
        srv.RegisterEndpoint(ep);

        const std::string ep_url = "http://127.0.0.1:" + std::to_string(srv.BoundPort()) + "/stream/r63_icy.mp3";
        std::atomic<bool> cancel{false};
        std::string out_title;
        TEST_CHECK(FetchIcyStreamTitle(ep_url, {}, cancel, out_title) == IcyPollResult::Title);
        TEST_CHECK(out_title == "Title A");

        srv.UpdateEndpointMetadata("r63_icy", "Title B", "");
        TEST_CHECK(FetchIcyStreamTitle(ep_url, {}, cancel, out_title) == IcyPollResult::Title);
        TEST_CHECK(out_title == "Title B");

        // live_title override appears in ICY frame, and UpdateEndpointMetadata zeroes live_title
        srv.UpdateEndpointLiveTitle("r63_icy", "Live Override Title");
        TEST_CHECK(FetchIcyStreamTitle(ep_url, {}, cancel, out_title) == IcyPollResult::Title);
        TEST_CHECK(out_title == "Live Override Title");

        srv.UpdateEndpointMetadata("r63_icy", "Track Title", "Artist");
        TEST_CHECK(FetchIcyStreamTitle(ep_url, {}, cancel, out_title) == IcyPollResult::Title);
        TEST_CHECK(out_title == "Artist - Track Title");

        cancel.store(true);
        TEST_CHECK(FetchIcyStreamTitle(ep_url, {}, cancel, out_title) == IcyPollResult::Failed);

        buf->Close();
        srv.Stop();
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{false, "0.0.0.0", "", 0});
    }

    // 4. FetchIcyStreamTitle against finite Content-Length endpoint -> NotLive without reading body
    TEST_PHASE("4. FetchIcyStreamTitle returns NotLive for Content-Length finite files");
    {
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{true, "0.0.0.0", "203.0.113.10", 0});
        asio::io_context srv_io;
        asio::ip::tcp::acceptor acc(
            srv_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const std::uint16_t port = acc.local_endpoint().port();
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> body_bytes_sent{0};
        std::thread thr([&]() {
            while (!stop.load()) {
                asio::error_code aec;
                asio::ip::tcp::socket sock(srv_io);
                acc.accept(sock, aec);
                if (aec || stop.load()) break;
                std::string req;
                std::array<char, 512> tmp{};
                while (req.find("\r\n\r\n") == std::string::npos) {
                    asio::error_code rec;
                    std::size_t n = sock.read_some(asio::buffer(tmp), rec);
                    if (n > 0) req.append(tmp.data(), n);
                    if (rec) break;
                }
                const std::string wav = BuildTestWavBytes(44100, 100);
                const std::string resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: audio/wav\r\n"
                    "Content-Length: 10000000\r\n"
                    "Connection: close\r\n\r\n" +
                    wav;
                asio::error_code wec;
                std::size_t written = asio::write(sock, asio::buffer(resp), wec);
                body_bytes_sent.store(written);
                sock.close(wec);
            }
        });

        std::atomic<bool> cancel{false};
        std::string title = "untouched";
        const IcyPollResult res = FetchIcyStreamTitle(
            "http://127.0.0.1:" + std::to_string(port) + "/x.wav", {}, cancel, title);

        stop.store(true);
        StopTestAcceptor(acc, port);
        if (thr.joinable()) thr.join();
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{false, "0.0.0.0", "", 0});

        TEST_CHECK(res == IcyPollResult::NotLive);
        TEST_CHECK(body_bytes_sent.load() < 100000);
    }

    // 7. Config sanitization for live_title_poll_seconds
    TEST_PHASE("7. PluginConfig live_title_poll_seconds defaults and clamping (0, 5->10, 999->300)");
    {
        PluginConfig def_cfg;
        TEST_CHECK(def_cfg.live_title_poll_seconds == 30);

        auto load_poll_val = [&](std::int64_t val, const char* fname) {
            const auto p = r63_dir / fname;
            {
                std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
                ofs << "{\"audio\":{\"live_title_poll_seconds\":" << val << "}}";
            }
            return ConfigManager::LoadOrCreate(p.string()).live_title_poll_seconds;
        };
        TEST_CHECK(load_poll_val(0, "cfg0.json") == 0);
        TEST_CHECK(load_poll_val(5, "cfg5.json") == 10);
        TEST_CHECK(load_poll_val(999, "cfg999.json") == 300);
    }

    // 5a/5b & 6 & 8. Live title natives, MoveChannelQueueItem, polling deduplication & NotLive stop
    TEST_PHASE("5/6/8. Natives Get/SetLiveTitle, MoveChannelQueueItem order/labels, and ICY polling callbacks");
    {
        std::atomic<int> radio_poll_count{0};
        std::atomic<bool> radio_use_title_two{false};
        std::atomic<int> vod_icy_probe_count{0};
        const std::string wav_long = BuildTestWavBytes(44100, 12000);

        asio::io_context srv_io;
        asio::ip::tcp::acceptor acc(
            srv_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const std::uint16_t port = acc.local_endpoint().port();
        std::atomic<bool> stop{false};
        std::mutex srv_mtx;
        std::vector<std::thread> srv_workers;
        std::vector<std::shared_ptr<asio::ip::tcp::socket>> srv_sockets;

        auto make_icy_packet = [](const std::string& stitle) {
            std::string raw = "StreamTitle='" + stitle + "';";
            std::size_t blocks = (raw.size() + 15) / 16;
            std::string pkt;
            pkt.push_back(static_cast<char>(blocks));
            pkt.append(raw);
            pkt.append(blocks * 16 - raw.size(), '\0');
            return pkt;
        };

        std::thread thr([&]() {
            while (!stop.load()) {
                asio::error_code aec;
                auto sock = std::make_shared<asio::ip::tcp::socket>(srv_io);
                acc.accept(*sock, aec);
                if (aec || stop.load()) break;
                std::lock_guard<std::mutex> lk(srv_mtx);
                srv_sockets.push_back(sock);
                srv_workers.emplace_back([sock, &radio_poll_count, &radio_use_title_two, &vod_icy_probe_count, &wav_long, &make_icy_packet]() {
                    std::string req;
                    std::array<char, 1024> tmp{};
                    while (req.find("\r\n\r\n") == std::string::npos) {
                        asio::error_code rec;
                        std::size_t n = sock->read_some(asio::buffer(tmp), rec);
                        if (n > 0) req.append(tmp.data(), n);
                        if (rec) break;
                    }
                    if (req.find("\r\n\r\n") == std::string::npos) {
                        asio::error_code sec;
                        sock->close(sec);
                        return;
                    }
                    const bool wants_icy = (req.find("Icy-MetaData: 1") != std::string::npos) ||
                                           (req.find("icy-metadata: 1") != std::string::npos);
                    if (req.find("/vod.wav") != std::string::npos) {
                        if (wants_icy) vod_icy_probe_count.fetch_add(1);
                        const std::string resp =
                            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                            std::to_string(wav_long.size()) + "\r\nConnection: close\r\n\r\n" + wav_long;
                        asio::error_code ec;
                        asio::write(*sock, asio::buffer(resp), ec);
                    } else {
                        if (wants_icy) {
                            radio_poll_count.fetch_add(1);
                            const std::string title = radio_use_title_two.load() ? "Song Two" : "Song One";
                            const std::string audio_chunk(16000, '\0');
                            const std::string body = audio_chunk + make_icy_packet(title);
                            const std::string resp =
                                "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: Test FM\r\nicy-metaint: 16000\r\nConnection: close\r\n\r\n" +
                                body;
                            asio::error_code ec;
                            asio::write(*sock, asio::buffer(resp), ec);
                        } else {
                            // Main stream connection serving WAV so channel/session stays Playing during test
                            const std::string resp =
                                "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                std::to_string(wav_long.size()) + "\r\nConnection: close\r\n\r\n" + wav_long;
                            asio::error_code ec;
                            asio::write(*sock, asio::buffer(resp), ec);
                        }
                    }
                    asio::error_code sec;
                    sock->close(sec);
                });
            }
        });

        MockPlatformHost host;
        host.players.push_back(PlayerSnapshot{0, {}, 0, 0});
        host.players.push_back(PlayerSnapshot{1, {}, 0, 0});
        host.player_names[0] = "Owner";
        host.player_names[1] = "Listener";

        PluginConfig cfg;
        cfg.base_dir = r63_dir.string();
        cfg.allow_private_network_urls = true;
        cfg.persist_channels = false;
        cfg.hook_native_audio = true;
        cfg.pawn_text_encoding = "utf8";
        cfg.live_title_poll_seconds = 30;
        cfg.debug_logging = false;

        AudioEngine engine(host, cfg);
        TEST_CHECK(engine.Initialize());

        auto wait_until = [&](auto&& pred, int timeout_ms = 5000) -> bool {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                engine.Tick();
                if (pred()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            engine.Tick();
            return pred();
        };

        // 5/6: Create channel with 1 active track + 3 queued items (with non-cp1252 UTF-8 label)
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        ChannelId ch = engine.CreateChannel("ChannelFM", 0);
        TEST_CHECK(ch != INVALID_CHANNEL_ID);
        TEST_CHECK(engine.JoinChannel(0, ch));
        TEST_CHECK(engine.PlayChannel(ch, base + "/radio"));

        TEST_CHECK(wait_until([&]() {
            return engine.GetChannelState(ch) == StreamState::Playing;
        }));

        // Initial automatic poll should receive "Song One" once and emit 1 channel callback
        TEST_CHECK(wait_until([&]() {
            std::lock_guard<std::mutex> lk(host.mtx);
            return !host.channel_live_title_events.empty();
        }));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_live_title_events.size() == 1);
            TEST_CHECK(host.channel_live_title_events[0].channel_id == ch);
            TEST_CHECK(host.channel_live_title_events[0].title == "Song One");
        }
        std::string ch_live;
        TEST_CHECK(engine.GetChannelLiveTitle(ch, ch_live) && ch_live == "Song One");

        // Second poll with same title ("Song One") emits NO second callback
        const int polls_before_same = radio_poll_count.load();
        engine.PollLiveTitlesAtForTesting(std::chrono::steady_clock::now() + std::chrono::seconds(45));
        TEST_CHECK(wait_until([&]() {
            return radio_poll_count.load() > polls_before_same;
        }));
        PumpEngineTicks(engine, 6);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_live_title_events.size() == 1);
        }

        // Third poll with "Song Two" emits second callback
        radio_use_title_two.store(true);
        const int polls_before_two = radio_poll_count.load();
        engine.PollLiveTitlesAtForTesting(std::chrono::steady_clock::now() + std::chrono::seconds(90));
        TEST_CHECK(wait_until([&]() {
            std::lock_guard<std::mutex> lk(host.mtx);
            return host.channel_live_title_events.size() == 2;
        }));
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_live_title_events[1].title == "Song Two");
        }

        // Manual SetChannelLiveTitle updates GetChannelLiveTitle, fires NO callback, preserves name & track title
        TEST_CHECK(engine.SetChannelLiveTitle(ch, "Manual Override\x01"));
        TEST_CHECK(engine.GetChannelLiveTitle(ch, ch_live) && ch_live == "Manual Override");
        ChannelInfo cinfo;
        TEST_CHECK(engine.GetChannelInfo(ch, cinfo));
        TEST_CHECK(cinfo.name == "ChannelFM");
        TEST_CHECK(cinfo.current_track_title != "Manual Override");
        PumpEngineTicks(engine, 4);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.channel_live_title_events.size() == 2);
        }
        TEST_CHECK(engine.SetChannelLiveTitle(ch, ""));
        TEST_CHECK(engine.GetChannelLiveTitle(ch, ch_live) && ch_live.empty());
        TEST_CHECK(!engine.SetChannelLiveTitle(99999, "Bad"));

        // MoveChannelQueueItem checks (items 0, 1, 2 with Unicode non-cp1252 in label)
        TEST_CHECK(engine.QueueChannel(ch, base + "/t0.mp3", "", "Label Zero"));
        TEST_CHECK(engine.QueueChannel(ch, base + "/t1.mp3", "", "Label One"));
        TEST_CHECK(engine.QueueChannel(ch, base + "/t2.mp3", "", "Label \xE6\x97\xA5\xE6\x9C\xAC"));
        TEST_CHECK(engine.MoveChannelQueueItem(ch, 1, 1)); // from == to no-op
        TEST_CHECK(!engine.MoveChannelQueueItem(ch, 99, 0));
        TEST_CHECK(!engine.MoveChannelQueueItem(ch, 0, 99));
        TEST_CHECK(!engine.MoveChannelQueueItem(99999, 0, 1));

        TEST_CHECK(engine.MoveChannelQueueItem(ch, 2, 0));
        std::string q0, q1, q2, l0, l1, l2;
        TEST_CHECK(engine.GetChannelQueueItem(ch, 0, q0) && q0.find("/t2.mp3") != std::string::npos);
        TEST_CHECK(engine.GetChannelQueueItemLabel(ch, 0, l0) && l0 == "Label \xE6\x97\xA5\xE6\x9C\xAC");
        TEST_CHECK(engine.GetChannelQueueItem(ch, 1, q1) && q1.find("/t0.mp3") != std::string::npos);
        TEST_CHECK(engine.GetChannelQueueItemLabel(ch, 1, l1) && l1 == "Label Zero");
        TEST_CHECK(engine.GetChannelQueueItem(ch, 2, q2) && q2.find("/t1.mp3") != std::string::npos);
        TEST_CHECK(engine.GetChannelQueueItemLabel(ch, 2, l2) && l2 == "Label One");

        TEST_CHECK(engine.MoveChannelQueueItem(ch, 0, 2));
        TEST_CHECK(engine.GetChannelQueueItem(ch, 0, q0) && q0.find("/t0.mp3") != std::string::npos);
        TEST_CHECK(engine.GetChannelQueueItem(ch, 2, q2) && q2.find("/t2.mp3") != std::string::npos);
        TEST_CHECK(engine.GetChannelQueueItemLabel(ch, 2, l2) && l2 == "Label \xE6\x97\xA5\xE6\x9C\xAC");

        // Personal stream: Set/GetStreamLiveTitle & NotLive stops future polls
        StreamId sid = engine.PlayAudioStreamForPlayer(1, base + "/vod.wav", StreamOptions{});
        TEST_CHECK(sid != INVALID_STREAM_ID);
        TEST_CHECK(wait_until([&]() {
            TrackMetadata m;
            StreamState st = StreamState::Idle;
            return engine.GetStreamInfo(sid, m, st) && st == StreamState::Playing && vod_icy_probe_count.load() >= 1;
        }));
        const int probes_after_first = vod_icy_probe_count.load();
        engine.PollLiveTitlesAtForTesting(std::chrono::steady_clock::now() + std::chrono::seconds(120));
        PumpEngineTicks(engine, 8);
        TEST_CHECK(vod_icy_probe_count.load() == probes_after_first);

        const std::size_t stream_cbs_before = [&]() {
            std::lock_guard<std::mutex> lk(host.mtx);
            return host.live_title_events.size();
        }();
        TEST_CHECK(engine.SetStreamLiveTitle(sid, "Custom Personal Title\n"));
        std::string st_live;
        TEST_CHECK(engine.GetStreamLiveTitle(sid, st_live) && st_live == "Custom Personal Title");
        PumpEngineTicks(engine, 4);
        {
            std::lock_guard<std::mutex> lk(host.mtx);
            TEST_CHECK(host.live_title_events.size() == stream_cbs_before);
        }
        TEST_CHECK(engine.SetStreamLiveTitle(sid, ""));
        TEST_CHECK(engine.GetStreamLiveTitle(sid, st_live) && st_live.empty());
        TEST_CHECK(!engine.SetStreamLiveTitle(99999, "Bad"));

        engine.DestroyChannel(ch);
        engine.StopAudioStreamForPlayer(1);
        TEST_CHECK(!engine.SetChannelLiveTitle(ch, "Destroyed"));
        TEST_CHECK(!engine.SetStreamLiveTitle(sid, "Stopped"));

        engine.Shutdown();
        stop.store(true);
        StopTestAcceptor(acc, port);
        if (thr.joinable()) thr.join();
        CloseTestSockets(srv_sockets, srv_mtx);
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            to_join.swap(srv_workers);
        }
        for (auto& w : to_join) {
            if (w.joinable()) w.join();
        }
    }

    std::filesystem::remove_all(r63_dir, fec);
    std::cout << "[PASS] TestRound63Features\n";
}

#define RUN_TEST(fn)                                                                          \
    do {                                                                                      \
        g_current_test.store(#fn, std::memory_order_release);                                 \
        g_current_phase.store("(start)", std::memory_order_release);                          \
        auto _t0 = std::chrono::steady_clock::now();                                          \
        g_test_start_ms.store(                                                                \
            std::chrono::duration_cast<std::chrono::milliseconds>(_t0.time_since_epoch()).count(), \
            std::memory_order_release);                                                       \
        std::cout << "[RUN ] " #fn "\n";                                                      \
        fn();                                                                                 \
        auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(                     \
            std::chrono::steady_clock::now() - _t0).count();                                  \
        std::cout << "[TIME] " #fn " " << _ms << " ms\n";                                     \
    } while (0)

int main(int argc, char** argv) {
    int child_exit_code = 0;
    if (TryHandleChildMode(argc, argv, child_exit_code)) {
        return child_exit_code;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::thread watchdog_thread([]() {
        while (!g_tests_done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (g_tests_done.load(std::memory_order_acquire)) break;
            std::int64_t start_ms = g_test_start_ms.load(std::memory_order_acquire);
            if (start_ms <= 0) continue;
            std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((now_ms - start_ms) >= 90000) {
                const char* t = g_current_test.load(std::memory_order_acquire);
                const char* p = g_current_phase.load(std::memory_order_acquire);
                std::cerr << "[HANG] " << (t ? t : "?") << " stuck in phase '" << (p ? p : "?") << "' for 90 s\n";
#ifdef _WIN32
                WriteHangMinidump();
#endif
                std::_Exit(3);
            }
        }
    });

    RUN_TEST(TestRingBuffer);
    RUN_TEST(TestRingBufferGenerationCancellation);
    RUN_TEST(TestBinaryManagerUpdateDecision);
    RUN_TEST(TestRound2EngineChecks);
    RUN_TEST(TestMetadataCache);
    RUN_TEST(TestUrlClassifier);
    RUN_TEST(TestConfigManager);
    RUN_TEST(TestHttpServer);
    RUN_TEST(TestHttpServerSlowWriterAndDisconnect);
    RUN_TEST(TestRound4HttpPlayedOffsetAndStopTeardown);
    RUN_TEST(TestRound5ConcurrencyReplacementAndPreResolvePause);
    RUN_TEST(TestRound6UnpauseBeforeReady);
    RUN_TEST(TestRound8Fase1MalformedMedia);
    RUN_TEST(TestRound8Fase2Ssrf);
    RUN_TEST(TestRound8Fase3Fixes);
    RUN_TEST(TestRound9EarlyDecoderCloseNotNetworkError);
    RUN_TEST(TestRound10HlsLiveToleranceAndVod);
    RUN_TEST(TestRound11HlsLiveSequenceAndStall);
    RUN_TEST(TestRound12HlsDiscontinuityAndPmtSwitch);
    RUN_TEST(TestRound13HlsStaleCdnPlaylist);
    RUN_TEST(TestRound24LibcurlInstallHint);
    RUN_TEST(TestRound27Fase1RawMp3LiveAndFlac);
    RUN_TEST(TestRound27Fase2HlsFormatsAndSilentWatchdog);
    RUN_TEST(TestRound27Fase3DnsVsPolicyBlock);
    RUN_TEST(TestRound27Fase4MaxPrefetchMb);
    RUN_TEST(TestRound27Fase5Cp1252AndEmoji);
    RUN_TEST(TestRound27Fase6SyncHttpShortcut);
    RUN_TEST(TestRound27Fase8LanAwareUrls);
    RUN_TEST(TestRound27Fase9IcyFraming);
    RUN_TEST(TestRound27Fase10LoggingToggles);
    RUN_TEST(TestV120ProcessingCallbackAndToggle);
    RUN_TEST(TestV120SetDistanceAndMp3FrameSync);
    RUN_TEST(TestV120ProcessDirectAudioFiles);
    RUN_TEST(TestV120SetPosAndAutoAttach);
    RUN_TEST(TestV200CompleteChannelSystem);
    RUN_TEST(TestV200CreatorReconnectRenameAndPolicyGuards);
    RUN_TEST(TestV200YtDlpCachePruning);
    RUN_TEST(TestV200EncodingPassthroughVolumeAndChannelOps);
    RUN_TEST(TestRound31SkipChannelConcurrencyPreservesQueue);
    RUN_TEST(TestRound32CallbacksLimitsAndOptionals);
    RUN_TEST(TestRound32PositionalChannelsQueueManipulationAndLoop);
    RUN_TEST(TestRound32ChannelPersistenceAndLifecycle);
    RUN_TEST(TestRound33AuditedFixes);
    RUN_TEST(TestRound35FeaturesPart1To8);
    RUN_TEST(TestRound35SeekAndTranscoder);
    RUN_TEST(TestRound36AuditedFixes);
    RUN_TEST(TestRound37ExtractorSettledErrorMessage);
    RUN_TEST(TestRound39WinHttpCancellationAndTimeouts);
    RUN_TEST(TestRound41ExtractorReResolveAndPersonalLog);
    RUN_TEST(TestRound42HttpDiagnosticsAndDenoRuntime);
    RUN_TEST(TestRound43WarningsUpgradeAndHealthTimeout);
    RUN_TEST(TestRound44DenoTimeoutNotPermanent);
    RUN_TEST(TestRound45WinHttpCookiesFreshSessionAndAb);
    RUN_TEST(TestRound46VerboseYtDlpPortraitDumpAndEnvDiag);
    RUN_TEST(TestRound47AvailableAtAndEarly403Retry);
    RUN_TEST(TestRound48NoAudioWatchdog);
    RUN_TEST(TestRound49WaitingCallbacks);
    RUN_TEST(TestRound51YouTubeAutoCookies);
    RUN_TEST(TestRound52PartAFixes);
    RUN_TEST(TestRound52PartBUnifiedConfig);
    RUN_TEST(TestRound53NetworkRetryAndPortWarn);
    RUN_TEST(TestRound54HttpConnectionLimits);
    RUN_TEST(TestThreadPoolAndScheduler);
    RUN_TEST(TestAudioEngineArbitrationAndDisconnect);
    RUN_TEST(TestTranscoderWaveAndBadMp4);
    RUN_TEST(TestSpatialPerformanceBenchmark);
    RUN_TEST(TestRound60Features);
    RUN_TEST(TestRound63Features);
    g_tests_done.store(true, std::memory_order_release);
    if (watchdog_thread.joinable()) watchdog_thread.join();
    std::cout << "All core unit tests passed!\n";
    return 0;
}
