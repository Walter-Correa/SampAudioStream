#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/audio/transcoder.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/network/http_client.hpp"
#include "sampaudiostream/network/http_server.hpp"
#include "sampaudiostream/runtime/platform_host.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

#define TEST_CHECK(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "[FAIL] Check failed: " #cond " at line " << __LINE__ << "\n";      \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

using namespace sampaudiostream;

namespace {

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
};

struct MockErrorEvent {
    StreamId stream_id{INVALID_STREAM_ID};
    std::int32_t error_code{0};
    std::string message;
};

class MockPlatformHost final : public IPlatformHost {
public:
    mutable std::mutex mtx;
    std::vector<PlayerSnapshot> players;
    std::vector<MockAudioEvent> events;
    std::vector<StreamId> ready_stream_ids;
    std::vector<MockErrorEvent> error_events;

    void Log(LogSeverity, const std::string&) override {}
    std::int32_t GetServerPort() const override { return 7777; }
    std::string GetServerBindIp() const override { return "127.0.0.1"; }
    bool TracksDisconnectEvents() const override { return false; }

    bool IsPlayerConnected(PlayerId player_id) const override {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return true;
        }
        return false;
    }

    std::vector<PlayerId> GetConnectedPlayers() const override {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<PlayerId> ids;
        for (const auto& p : players) ids.push_back(p.id);
        return ids;
    }

    std::vector<PlayerSnapshot> GetConnectedPlayerSnapshots() const override {
        std::lock_guard<std::mutex> lock(mtx);
        return players;
    }

    bool GetPlayerPos(PlayerId player_id, Vector3& out_pos) const override {
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
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return p.world_id;
        }
        return 0;
    }

    std::int32_t GetPlayerInterior(PlayerId player_id) const override {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& p : players) {
            if (p.id == player_id) return p.interior_id;
        }
        return 0;
    }

    std::int32_t GetPlayerVehicleId(PlayerId) const override { return 0; }
    bool GetVehiclePos(std::int32_t, Vector3&) const override { return false; }
    mutable std::thread::id expected_host_thread{std::this_thread::get_id()};
    mutable std::atomic<bool> get_ip_off_main_thread{false};
    std::string player_ip_override{"127.0.0.1"};
    std::string GetPlayerIp(PlayerId) const override {
        if (std::this_thread::get_id() != expected_host_thread) {
            get_ip_off_main_thread.store(true);
        }
        return player_ip_override;
    }

    void SendPlayAudioStream(PlayerId player_id, const std::string& url, const Vector3&, float, bool) override {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back({player_id, true, url});
    }

    void SendStopAudioStream(PlayerId player_id) override {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back({player_id, false, ""});
    }

    void DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) override {
        if (name == "OnAudioStreamReady" && args.size() >= 2) {
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

    // 2. SA_PauseChannel(id, false) must NOT transition Idle/Stopped channels to Playing
    PluginConfig ok_cfg;
    ok_cfg.bind_ip = "127.0.0.1";
    ok_cfg.public_ip = "127.0.0.1";
    ok_cfg.http_port = 0;
    ok_cfg.auto_download_extractor = false;

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

        AudioEngine lim_engine(host, lim_cfg);
        TEST_CHECK(lim_engine.Initialize());
        ChannelId ch1 = lim_engine.CreateChannel("ch-lim-1");
        ChannelId ch2 = lim_engine.CreateChannel("ch-lim-2");
        TEST_CHECK(lim_engine.PlayChannel(ch1, "https://example.com/slow.mp3"));
        // Active channel holds the sole transcoder slot -> second play & extra personal stream rejected
        TEST_CHECK(!lim_engine.PlayChannel(ch2, "https://example.com/slow2.mp3"));
        TEST_CHECK(!lim_engine.QueueChannel(ch2, "https://example.com/q.mp3"));
        TEST_CHECK(lim_engine.QueueChannel(ch1, "https://example.com/q_ok.mp3"));
        TEST_CHECK(lim_engine.PlayAudioStreamForPlayer(0, "https://example.com/s.mp3", StreamOptions{}) == INVALID_STREAM_ID);
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
    // Queue had 2 items; 3rd skip must fail (queue shrunk by exactly 1 per skip)
    TEST_CHECK(!engine.SkipChannel(ch1));

    // Idle ch2 queues while slot 1 is free, then limit=2 fills -> SkipChannel fails cleanly without losing track
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    for (int t = 0; t < 15; ++t) {
        engine.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ChannelId ch2 = engine.CreateChannel("r5-ch2");
    TEST_CHECK(ch2 != INVALID_CHANNEL_ID);
    TEST_CHECK(pump_until([&]() {
        return engine.QueueChannel(ch2, base + "/keep.wav");
    }, 2000));
    s1 = engine.PlayAudioStreamForPlayer(1, base + "/fill.wav", opts);
    TEST_CHECK(s1 != INVALID_STREAM_ID);
    TEST_CHECK(pump_until([&]() {
        TrackMetadata m1;
        StreamState st1 = StreamState::Idle;
        return engine.GetStreamInfo(s1, m1, st1) && st1 == StreamState::Playing;
    }, 2000));

    TEST_CHECK(!engine.SkipChannel(ch2));
    TEST_CHECK(engine.StopAudioStreamForPlayer(1));
    TEST_CHECK(pump_until([&]() {
        return engine.SkipChannel(ch2); // Track was preserved and plays once slot opens!
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

    // HTTPS must NOT bypass via sync shortcut (still goes async Resolving first)
    {
        std::lock_guard<std::mutex> lk(host.mtx);
        host.events.clear();
    }
    StreamId sid_https = engine.PlayAudioStreamForPlayer(0, "https://radio.example.com/stream.mp3", StreamOptions{});
    TEST_CHECK(sid_https != INVALID_STREAM_ID);
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

#define RUN_TEST(fn)                                                                          \
    do {                                                                                      \
        std::cout << "[RUN ] " #fn "\n";                                                      \
        fn();                                                                                 \
    } while (0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

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
    RUN_TEST(TestThreadPoolAndScheduler);
    RUN_TEST(TestAudioEngineArbitrationAndDisconnect);
    RUN_TEST(TestTranscoderWaveAndBadMp4);
    RUN_TEST(TestSpatialPerformanceBenchmark);
    std::cout << "All core unit tests passed!\n";
    return 0;
}
