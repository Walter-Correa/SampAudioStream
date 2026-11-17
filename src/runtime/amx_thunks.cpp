#include <amx/amx.h>
#include <plugincommon.h>

#include <cstdint>

void* pAMXFunctions = nullptr;

namespace {
template <typename FnPtr>
FnPtr GetAmxExport(int index) {
    return reinterpret_cast<FnPtr*>(pAMXFunctions)[index];
}
} // namespace

uint16_t* AMXAPI amx_Align16(uint16_t* v) {
    using Fn = uint16_t* (AMXAPI*)(uint16_t*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Align16)(v);
}

uint32_t* AMXAPI amx_Align32(uint32_t* v) {
    using Fn = uint32_t* (AMXAPI*)(uint32_t*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Align32)(v);
}

#if defined _I64_MAX || defined HAVE_I64
uint64_t* AMXAPI amx_Align64(uint64_t* v) {
    using Fn = uint64_t* (AMXAPI*)(uint64_t*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Align64)(v);
}
#endif

int AMXAPI amx_Allot(AMX* amx, int cells, cell* amx_addr, cell** phys_addr) {
    using Fn = int (AMXAPI*)(AMX*, int, cell*, cell**);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Allot)(amx, cells, amx_addr, phys_addr);
}

int AMXAPI amx_Callback(AMX* amx, cell index, cell* result, cell* params) {
    using Fn = int (AMXAPI*)(AMX*, cell, cell*, cell*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Callback)(amx, index, result, params);
}

int AMXAPI amx_Cleanup(AMX* amx) {
    using Fn = int (AMXAPI*)(AMX*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Cleanup)(amx);
}

int AMXAPI amx_Clone(AMX* amxClone, AMX* amxSource, void* data) {
    using Fn = int (AMXAPI*)(AMX*, AMX*, void*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Clone)(amxClone, amxSource, data);
}

int AMXAPI amx_Exec(AMX* amx, cell* retval, int index) {
    using Fn = int (AMXAPI*)(AMX*, cell*, int);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Exec)(amx, retval, index);
}

int AMXAPI amx_FindNative(AMX* amx, const char* name, int* index) {
    using Fn = int (AMXAPI*)(AMX*, const char*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_FindNative)(amx, name, index);
}

int AMXAPI amx_FindPublic(AMX* amx, const char* funcname, int* index) {
    using Fn = int (AMXAPI*)(AMX*, const char*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_FindPublic)(amx, funcname, index);
}

int AMXAPI amx_FindPubVar(AMX* amx, const char* varname, cell* amx_addr) {
    using Fn = int (AMXAPI*)(AMX*, const char*, cell*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_FindPubVar)(amx, varname, amx_addr);
}

int AMXAPI amx_FindTagId(AMX* amx, cell tag_id, char* tagname) {
    using Fn = int (AMXAPI*)(AMX*, cell, char*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_FindTagId)(amx, tag_id, tagname);
}

int AMXAPI amx_Flags(AMX* amx, uint16_t* flags) {
    using Fn = int (AMXAPI*)(AMX*, uint16_t*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Flags)(amx, flags);
}

int AMXAPI amx_GetAddr(AMX* amx, cell amx_addr, cell** phys_addr) {
    using Fn = int (AMXAPI*)(AMX*, cell, cell**);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetAddr)(amx, amx_addr, phys_addr);
}

int AMXAPI amx_GetNative(AMX* amx, int index, char* funcname) {
    using Fn = int (AMXAPI*)(AMX*, int, char*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetNative)(amx, index, funcname);
}

int AMXAPI amx_GetPublic(AMX* amx, int index, char* funcname) {
    using Fn = int (AMXAPI*)(AMX*, int, char*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetPublic)(amx, index, funcname);
}

int AMXAPI amx_GetPubVar(AMX* amx, int index, char* varname, cell* amx_addr) {
    using Fn = int (AMXAPI*)(AMX*, int, char*, cell*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetPubVar)(amx, index, varname, amx_addr);
}

int AMXAPI amx_GetString(char* dest, const cell* source, int use_wchar, size_t size) {
    using Fn = int (AMXAPI*)(char*, const cell*, int, size_t);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetString)(dest, source, use_wchar, size);
}

int AMXAPI amx_GetTag(AMX* amx, int index, char* tagname, cell* tag_id) {
    using Fn = int (AMXAPI*)(AMX*, int, char*, cell*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetTag)(amx, index, tagname, tag_id);
}

int AMXAPI amx_GetUserData(AMX* amx, long tag, void** ptr) {
    using Fn = int (AMXAPI*)(AMX*, long, void**);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_GetUserData)(amx, tag, ptr);
}

int AMXAPI amx_Init(AMX* amx, void* program) {
    using Fn = int (AMXAPI*)(AMX*, void*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Init)(amx, program);
}

int AMXAPI amx_InitJIT(AMX* amx, void* reloc_table, void* native_code) {
    using Fn = int (AMXAPI*)(AMX*, void*, void*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_InitJIT)(amx, reloc_table, native_code);
}

int AMXAPI amx_MemInfo(AMX* amx, long* codesize, long* datasize, long* stackheap) {
    using Fn = int (AMXAPI*)(AMX*, long*, long*, long*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_MemInfo)(amx, codesize, datasize, stackheap);
}

int AMXAPI amx_NameLength(AMX* amx, int* length) {
    using Fn = int (AMXAPI*)(AMX*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NameLength)(amx, length);
}

AMX_NATIVE_INFO* AMXAPI amx_NativeInfo(const char* name, AMX_NATIVE func) {
    using Fn = AMX_NATIVE_INFO* (AMXAPI*)(const char*, AMX_NATIVE);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NativeInfo)(name, func);
}

int AMXAPI amx_NumNatives(AMX* amx, int* number) {
    using Fn = int (AMXAPI*)(AMX*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NumNatives)(amx, number);
}

int AMXAPI amx_NumPublics(AMX* amx, int* number) {
    using Fn = int (AMXAPI*)(AMX*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NumPublics)(amx, number);
}

int AMXAPI amx_NumPubVars(AMX* amx, int* number) {
    using Fn = int (AMXAPI*)(AMX*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NumPubVars)(amx, number);
}

int AMXAPI amx_NumTags(AMX* amx, int* number) {
    using Fn = int (AMXAPI*)(AMX*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_NumTags)(amx, number);
}

int AMXAPI amx_Push(AMX* amx, cell value) {
    using Fn = int (AMXAPI*)(AMX*, cell);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Push)(amx, value);
}

int AMXAPI amx_PushArray(AMX* amx, cell* amx_addr, cell** phys_addr, const cell array[], int numcells) {
    using Fn = int (AMXAPI*)(AMX*, cell*, cell**, const cell[], int);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_PushArray)(amx, amx_addr, phys_addr, array, numcells);
}

int AMXAPI amx_PushString(AMX* amx, cell* amx_addr, cell** phys_addr, const char* string, int pack, int use_wchar) {
    using Fn = int (AMXAPI*)(AMX*, cell*, cell**, const char*, int, int);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_PushString)(amx, amx_addr, phys_addr, string, pack, use_wchar);
}

int AMXAPI amx_RaiseError(AMX* amx, int error) {
    using Fn = int (AMXAPI*)(AMX*, int);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_RaiseError)(amx, error);
}

int AMXAPI amx_Register(AMX* amx, const AMX_NATIVE_INFO* nativelist, int number) {
    using Fn = int (AMXAPI*)(AMX*, const AMX_NATIVE_INFO*, int);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Register)(amx, nativelist, number);
}

int AMXAPI amx_Release(AMX* amx, cell amx_addr) {
    using Fn = int (AMXAPI*)(AMX*, cell);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_Release)(amx, amx_addr);
}

int AMXAPI amx_SetCallback(AMX* amx, AMX_CALLBACK callback) {
    using Fn = int (AMXAPI*)(AMX*, AMX_CALLBACK);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_SetCallback)(amx, callback);
}

int AMXAPI amx_SetDebugHook(AMX* amx, AMX_DEBUG debug) {
    using Fn = int (AMXAPI*)(AMX*, AMX_DEBUG);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_SetDebugHook)(amx, debug);
}

int AMXAPI amx_SetString(cell* dest, const char* source, int pack, int use_wchar, size_t size) {
    using Fn = int (AMXAPI*)(cell*, const char*, int, int, size_t);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_SetString)(dest, source, pack, use_wchar, size);
}

int AMXAPI amx_SetUserData(AMX* amx, long tag, void* ptr) {
    using Fn = int (AMXAPI*)(AMX*, long, void*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_SetUserData)(amx, tag, ptr);
}

int AMXAPI amx_StrLen(const cell* cstring, int* length) {
    using Fn = int (AMXAPI*)(const cell*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_StrLen)(cstring, length);
}

int AMXAPI amx_UTF8Check(const char* string, int* length) {
    using Fn = int (AMXAPI*)(const char*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_UTF8Check)(string, length);
}

int AMXAPI amx_UTF8Get(const char* string, const char** endptr, cell* value) {
    using Fn = int (AMXAPI*)(const char*, const char**, cell*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_UTF8Get)(string, endptr, value);
}

int AMXAPI amx_UTF8Len(const char* cstr, int* length) {
    using Fn = int (AMXAPI*)(const char*, int*);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_UTF8Len)(cstr, length);
}

int AMXAPI amx_UTF8Put(char* string, char** endptr, int maxchars, cell value) {
    using Fn = int (AMXAPI*)(char*, char**, int, cell);
    return GetAmxExport<Fn>(PLUGIN_AMX_EXPORT_UTF8Put)(string, endptr, maxchars, value);
}
