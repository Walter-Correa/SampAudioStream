#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"
#include "sampaudiostream/runtime/omp_host.hpp"
#include "sampaudiostream/runtime/samp_host.hpp"

#include <plugincommon.h>
#include <sdk.hpp>

#include <memory>

extern void* pAMXFunctions;

namespace {
std::unique_ptr<sampaudiostream::SampHost> g_samp_host;
std::unique_ptr<sampaudiostream::AudioEngine> g_samp_engine;
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

    sampaudiostream::PluginConfig cfg =
        sampaudiostream::ConfigManager::LoadOrCreate("scriptfiles/samp-audio-stream/config.json");

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
        return false;
    }

    if (logprintf) {
        logprintf("  SampAudioStream v1.0.0 loaded (HTTP port: %u)", g_samp_engine->GetHttpPort());
    }
    return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
    sampaudiostream::SetActiveAudioEngine(nullptr);
    if (g_samp_engine) {
        g_samp_engine->Shutdown();
        g_samp_engine.reset();
    }
    if (g_samp_host) {
        g_samp_host->SetEngine(nullptr);
        g_samp_host.reset();
    }
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
