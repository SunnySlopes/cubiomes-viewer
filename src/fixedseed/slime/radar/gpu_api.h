#ifndef SR_GPU_API_H
#define SR_GPU_API_H
#include "slimerander.h"
// Optional DLL ABI. The control callback runs on the caller's worker thread.
typedef int (*sr_gpu_control_fn)(void *);
typedef int (*sr_gpu_search_fn)(const SrParams *, void *, sr_result_fn,
                                sr_progress_fn, sr_gpu_control_fn);
#define SR_GPU_UNAVAILABLE 3
#define SR_GPU_FAILED 4
#endif
