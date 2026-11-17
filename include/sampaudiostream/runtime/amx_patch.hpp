#pragma once

#include <amx/amx.h>
#include <cstddef>
#include <cstdint>

namespace sampaudiostream {

inline AMX_FUNCSTUBNT* GetAmxNativeEntry(AMX* amx, int index) {
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    auto* natives = reinterpret_cast<AMX_FUNCSTUBNT*>(
        amx->base + static_cast<std::size_t>(hdr->natives));
    return &natives[index];
}

inline AMX_NATIVE FindAmxNativeFunc(AMX* amx, const char* name) {
#if INTPTR_MAX == INT32_MAX
    if (!amx || !amx->base || !name) return nullptr;
    int idx = -1;
    if (amx_FindNative(amx, name, &idx) != AMX_ERR_NONE || idx < 0) {
        return nullptr;
    }
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    if (hdr->defsize != sizeof(AMX_FUNCSTUBNT)) {
        auto* stub = reinterpret_cast<AMX_FUNCSTUB*>(
            amx->base + static_cast<std::size_t>(hdr->natives) +
            static_cast<std::size_t>(idx) * static_cast<std::size_t>(hdr->defsize));
        return reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(stub->address));
    }
    AMX_FUNCSTUBNT* entry = GetAmxNativeEntry(amx, idx);
    return reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(entry->address));
#else
    (void)amx;
    (void)name;
    return nullptr;
#endif
}

inline bool PatchAmxNative(AMX* amx, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original) {
#if INTPTR_MAX == INT32_MAX
    if (!amx || !amx->base || !name || !replacement) return false;
    int idx = -1;
    if (amx_FindNative(amx, name, &idx) != AMX_ERR_NONE || idx < 0) {
        return false;
    }
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    if (hdr->defsize != sizeof(AMX_FUNCSTUBNT)) {
        auto* stub = reinterpret_cast<AMX_FUNCSTUB*>(
            amx->base + static_cast<std::size_t>(hdr->natives) +
            static_cast<std::size_t>(idx) * static_cast<std::size_t>(hdr->defsize));
        auto orig = reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(stub->address));
        if (orig && orig != replacement && out_original && !*out_original) {
            *out_original = orig;
        }
        stub->address = static_cast<ucell>(reinterpret_cast<std::uintptr_t>(replacement));
        return true;
    }

    AMX_FUNCSTUBNT* entry = GetAmxNativeEntry(amx, idx);
    auto orig = reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(entry->address));
    if (orig && orig != replacement && out_original && !*out_original) {
        *out_original = orig;
    }
    entry->address = static_cast<ucell>(reinterpret_cast<std::uintptr_t>(replacement));
    return true;
#else
    (void)amx;
    (void)name;
    (void)replacement;
    (void)out_original;
    return false;
#endif
}

} // namespace sampaudiostream
