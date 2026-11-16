#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/network/http_server.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace sampaudiostream;

void TestRingBuffer() {
    AudioStreamBuffer buf(16);
    std::uint8_t data1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf.Write(data1, sizeof(data1));
    assert(buf.TotalWritten() == 10);

    std::uint64_t off = 0;
    std::uint8_t out[16]{};
    std::size_t n = buf.Read(off, out, 6, 0);
    assert(n == 6);
    assert(off == 6);
    assert(out[0] == 1 && out[5] == 6);

    // Overwrite wrap-around beyond capacity 16
    std::uint8_t data2[14] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    buf.Write(data2, sizeof(data2));
    assert(buf.TotalWritten() == 24);
    assert(buf.EarliestAvailableOffset() == 8);

    // Reader at old offset 6 should snap forward to 8
    n = buf.Read(off, out, 16, 0);
    assert(n == 16);
    assert(off == 24);
    assert(out[0] == 9 && out[15] == 24);
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
    assert(cache.GetTrack("k1").has_value()); // k1 becomes MRU, k2 becomes LRU
    cache.PutTrack("k3", t3);                // evicts k2
    assert(!cache.GetTrack("k2").has_value());
    assert(cache.GetTrack("k1")->title == "Song 1");
    assert(cache.GetTrack("k3")->title == "Song 3");
    std::cout << "[PASS] TestMetadataCache\n";
}

void TestUrlClassifier() {
    assert(StreamResolver::LooksLikeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    assert(StreamResolver::LooksLikeUrl("http://radio.example.com:8000/stream.mp3"));
    assert(!StreamResolver::LooksLikeUrl("Linkin Park Numb"));

    assert(StreamResolver::IsKnownDirectAudioExtension("http://site.com/music.mp3?token=123"));
    assert(StreamResolver::IsKnownDirectAudioExtension("https://site.com/audio.OGG"));
    assert(!StreamResolver::IsKnownDirectAudioExtension("https://www.youtube.com/watch?v=abc"));
    std::cout << "[PASS] TestUrlClassifier\n";
}

void TestConfigManager() {
    std::string temp_cfg = "build_test_config.json";
    std::error_code ec;
    std::filesystem::remove(temp_cfg, ec);

    PluginConfig cfg = ConfigManager::LoadOrCreate(temp_cfg);
    assert(cfg.mp3_bitrate_kbps == 128);
    assert(cfg.hook_native_audio == true);
    assert(std::filesystem::exists(temp_cfg));

    cfg.http_port = 8899;
    assert(ConfigManager::Save(cfg, temp_cfg));
    PluginConfig loaded = ConfigManager::LoadOrCreate(temp_cfg);
    assert(loaded.http_port == 8899);

    std::filesystem::remove(temp_cfg, ec);
    std::cout << "[PASS] TestConfigManager\n";
}

void TestHttpServer() {
    HttpStreamingServer srv;
    std::string err;
    assert(srv.Start("127.0.0.1", 0, err));
    assert(srv.IsRunning());
    assert(srv.BoundPort() > 0);

    auto buf = std::make_shared<AudioStreamBuffer>(64 * 1024);
    srv.RegisterEndpoint(StreamEndpointInfo{"abc123", "Test Track", "Tester", 128, false, 0, buf});
    srv.UnregisterEndpoint("abc123");
    srv.Stop();
    assert(!srv.IsRunning());
    std::cout << "[PASS] TestHttpServer\n";
}

int main() {
    TestRingBuffer();
    TestMetadataCache();
    TestUrlClassifier();
    TestConfigManager();
    TestHttpServer();
    std::cout << "All core unit tests passed!\n";
    return 0;
}
