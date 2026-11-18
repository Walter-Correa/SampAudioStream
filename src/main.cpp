#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"
#include "sampaudiostream/runtime/instance_guard.hpp"
#include "sampaudiostream/runtime/omp_host.hpp"
#include "sampaudiostream/runtime/samp_host.hpp"

#include <plugincommon.h>
#include <sdk.hpp>

#include <memory>

extern void* pAMXFunctions;

namespace {
std::unique_ptr<sampaudiostream::SampHost> g_samp_host;
std::unique_ptr<sampaudiostream::AudioEngine> g_samp_engine;
bool g_samp_instance_acquired = false;

void CleanupSampRuntime() {
    sampaudiostream::SetActiveAudioEngine(nullptr);
    if (g_samp_engine) {
        g_samp_engine->NotifyLifecycleStopping();
        g_samp_engine->Shutdown();
        g_samp_engine.reset();
    }
    sampaudiostream::Logger::Instance().Flush();
    sampaudiostream::Logger::Instance().SetSink(nullptr);
    if (g_samp_host) {
        g_samp_host->SetEngine(nullptr);
        g_samp_host.reset();
    }
    if (g_samp_instance_acquired) {
        sampaudiostream::RuntimeInstanceGuard::Release();
        g_samp_instance_acquired = false;
    }
}
} // namespace

// ---------------- open.mp Native Component Entry Point ----------------

COMPONENT_ENTRY_POINT() {
    return new sampaudiostream::OmpHost();
}

// ---------------- Legacy SA-MP Plugin Entry Points ----------------

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
    return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** ppData) {
    if (!ppData) return false;

    pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
    auto logprintf = reinterpret_cast<sampaudiostream::SampHost::LogPrintfFn>(ppData[PLUGIN_DATA_LOGPRINTF]);

    std::string guard_err;
    if (!sampaudiostream::RuntimeInstanceGuard::Acquire("SA-MP Plugin", guard_err)) {
        if (logprintf) logprintf("[SampAudioStream] [ERROR] %s", guard_err.c_str());
        return false;
    }
    g_samp_instance_acquired = true;

    const std::string cfg_path = "scriptfiles/SampAudioStream/config.json";
    sampaudiostream::PluginConfig cfg = sampaudiostream::ConfigManager::LoadOrCreate(cfg_path);

    g_samp_host = std::make_unique<sampaudiostream::SampHost>(logprintf);
    g_samp_host->SetHookNativeAudio(cfg.hook_native_audio);

    sampaudiostream::Logger::Instance().SetSink([](sampaudiostream::LogSeverity sev, const std::string& msg) {
        if (g_samp_host) g_samp_host->Log(sev, msg);
    });

    g_samp_engine = std::make_unique<sampaudiostream::AudioEngine>(*g_samp_host, cfg);
    g_samp_host->SetEngine(g_samp_engine.get());
    sampaudiostream::SetActiveAudioEngine(g_samp_engine.get());

    if (!g_samp_engine->Initialize()) {
        if (logprintf) logprintf("[SampAudioStream] Failed to initialize AudioEngine.");
        sampaudiostream::Logger::Instance().Flush();
        CleanupSampRuntime();
        return false;
    }

    g_samp_engine->WaitForStartupReadinessAndLog(
        [&]() {
            if (logprintf && cfg.log_startup_banner) {
                logprintf("[SampAudioStream] SampAudioStream v2.0.0 [SA-MP] loaded (config: %s, HTTP port: %u)",
                          cfg_path.c_str(), g_samp_engine->GetHttpPort());
            }
        },
        /*flush_before_banner=*/true);
    return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
    CleanupSampRuntime();
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx) {
    if (g_samp_host) {
        g_samp_host->OnAmxLoad(amx);
    }
    return AMX_ERR_NONE;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx) {
    if (g_samp_host) {
        g_samp_host->OnAmxUnload(amx);
    }
    return AMX_ERR_NONE;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() {
    if (g_samp_host) {
        g_samp_host->OnProcessTick();
    }
}
