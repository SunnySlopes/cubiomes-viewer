#ifndef SR_GPU_LOADER_H
#define SR_GPU_LOADER_H
#include "gpu_api.h"
#if defined(_WIN32)
#include <windows.h>
#include <string>
#endif
// Load only an optional backend next to the executable, with a versioned ABI.
inline int sr_try_gpu(const SrParams *p,void *ctx,sr_result_fn result,
                      sr_progress_fn progress,sr_gpu_control_fn control){
#if defined(_WIN32)
 wchar_t path[32768];DWORD size=GetModuleFileNameW(nullptr,path,32768);
 if(!size||size>=32768)return SR_GPU_UNAVAILABLE;
 std::wstring file(path);file=file.substr(0,file.find_last_of(L"\\/")+1)+L"slimeradar_cuda.dll";
 // Keep the backend loaded for the process lifetime, retaining its PTX cache.
 static HMODULE dll=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!dll)return SR_GPU_UNAVAILABLE;
 auto fn=reinterpret_cast<sr_gpu_search_fn>(GetProcAddress(dll,"sr_search_gpu_v1"));
 return fn?fn(p,ctx,result,progress,control):SR_GPU_UNAVAILABLE;
#else
 return SR_GPU_UNAVAILABLE;
#endif
}
#endif
