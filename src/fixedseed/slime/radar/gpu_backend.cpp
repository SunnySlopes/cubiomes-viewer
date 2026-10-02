// Optional Windows CUDA driver/NVRTC backend. No dependency in the Qt executable.
#include "gpu_api.h"
#include "slime.h"
#include <windows.h>
#include <cuda.h>
#include <nvrtc.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <mutex>
#include <memory>
#include <vector>

static const char *kernel=R"cuda(
typedef unsigned long long U;
__device__ int slime(U x,U z){
 U s=((x+z)^987234911ULL^0x5deece66dULL)&0xffffffffffffULL;
 for(;;){s=(s*0x5deece66dULL+11ULL)&0xffffffffffffULL;
 unsigned b=s>>17, v=b%10; if(b<2147483640U)return v==0;}
}
extern "C" __global__ void prefix(const U *xs,const U *zs,unsigned short *out,int width){
 __shared__ int sums[8];
 int lane=threadIdx.x&31,warp=threadIdx.x>>5,col=blockIdx.x*256+threadIdx.x,row=blockIdx.y;
 int v=slime(xs[col],zs[row]);
 for(int k=1;k<32;k<<=1){int a=__shfl_up_sync(0xffffffff,v,k);if(lane>=k)v+=a;}
 if(lane==31)sums[warp]=v;__syncthreads();
 for(int i=0;i<warp;i++)v+=sums[i];out[(U)row*width+col]=v;
}
__device__ int segment(const unsigned short *row,int lo,int hi){
 int left=lo-1,tile=hi/256;
 if(left<0)return row[hi];
 if(left/256==tile)return row[hi]-row[left];
 return row[(left/256+1)*256-1]-row[left]+row[hi];
}
struct Hit{int x,z;unsigned count;};
extern "C" __global__ void score(const unsigned short *ps,int width,int nx,int nz,
 const int *runs,int threshold,int bx,int bz,Hit *hits,unsigned *count){
 int x=blockIdx.x*blockDim.x+threadIdx.x;if(x>=nx)return;
 int box=0;for(int row=0;row<17;row++)box+=segment(ps+(U)row*width,x,x+16);
 for(int z=0;z<nz;z++){
  // Square count is an exact upper bound for its subset, the chunk annulus.
  if(box>=threshold){int total=0;
   for(int k=0;k<20;k++)total+=segment(ps+(U)(z+runs[3*k])*width,x+runs[3*k+1],x+runs[3*k+2]);
   if(total>=threshold){unsigned j=atomicAdd(count,1U);hits[j]={bx+x,bz+z,(unsigned)total};}
  }
  if(z+1<nz)box+=segment(ps+(U)(z+17)*width,x,x+16)-segment(ps+(U)z*width,x,x+16);
 }
}
)cuda";

struct GPU {
 HMODULE driver=nullptr,rtc=nullptr;CUcontext context=nullptr,previous=nullptr;
 CUmodule module=nullptr;CUdevice device=0;bool retained=false;nvrtcProgram program=nullptr;
 CUdeviceptr xs=0,zs=0,ps=0,runs=0,hits=0,count=0;
#define API(name) decltype(&::name) f_##name=nullptr
 API(cuInit);API(cuDeviceGet);API(cuDeviceGetAttribute);API(cuDevicePrimaryCtxRetain);
 API(cuDevicePrimaryCtxRelease);API(cuCtxGetCurrent);API(cuCtxSetCurrent);
 API(cuModuleLoadData);API(cuModuleGetFunction);API(cuModuleUnload);
 API(cuMemAlloc);API(cuMemFree);API(cuMemcpyHtoD);API(cuMemcpyDtoH);
 API(cuLaunchKernel);API(cuCtxSynchronize);
 API(nvrtcCreateProgram);API(nvrtcCompileProgram);API(nvrtcGetPTXSize);API(nvrtcGetPTX);
 API(nvrtcGetProgramLogSize);API(nvrtcGetProgramLog);API(nvrtcDestroyProgram);
#undef API
 template<class T> void symbol(T &target,HMODULE dll,const char *name){
  target=reinterpret_cast<T>(GetProcAddress(dll,name));if(!target)throw std::runtime_error(name);
 }
 void check(int code){if(code)throw std::runtime_error("CUDA/NVRTC error "+std::to_string(code));}
 GPU(){ }
 void init(){
  driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  // Resolve NVRTC beside this DLL, never from the current working directory.
  HMODULE own=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
       reinterpret_cast<LPCWSTR>(&kernel),&own);
  wchar_t path[32768];GetModuleFileNameW(own,path,32768);std::wstring file(path);
  file=file.substr(0,file.find_last_of(L"\\/")+1)+L"nvrtc64_120_0.dll";
  rtc=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!driver||!rtc)throw std::runtime_error("NVIDIA driver or NVRTC DLL unavailable");
#define D(name,entry) symbol(f_##name,driver,entry)
  D(cuInit,"cuInit");D(cuDeviceGet,"cuDeviceGet");D(cuDeviceGetAttribute,"cuDeviceGetAttribute");
  D(cuDevicePrimaryCtxRetain,"cuDevicePrimaryCtxRetain");D(cuDevicePrimaryCtxRelease,"cuDevicePrimaryCtxRelease_v2");
  D(cuCtxGetCurrent,"cuCtxGetCurrent");D(cuCtxSetCurrent,"cuCtxSetCurrent");
  D(cuModuleLoadData,"cuModuleLoadData");D(cuModuleGetFunction,"cuModuleGetFunction");D(cuModuleUnload,"cuModuleUnload");
  D(cuMemAlloc,"cuMemAlloc_v2");D(cuMemFree,"cuMemFree_v2");D(cuMemcpyHtoD,"cuMemcpyHtoD_v2");D(cuMemcpyDtoH,"cuMemcpyDtoH_v2");
  D(cuLaunchKernel,"cuLaunchKernel");D(cuCtxSynchronize,"cuCtxSynchronize");
#undef D
#define N(name) symbol(f_##name,rtc,#name)
  N(nvrtcCreateProgram);N(nvrtcCompileProgram);N(nvrtcGetPTXSize);N(nvrtcGetPTX);
  N(nvrtcGetProgramLogSize);N(nvrtcGetProgramLog);N(nvrtcDestroyProgram);
#undef N
  check(f_cuInit(0));check(f_cuDeviceGet(&device,0));check(f_cuCtxGetCurrent(&previous));
  check(f_cuDevicePrimaryCtxRetain(&context,device));retained=true;check(f_cuCtxSetCurrent(context));
  int major=0,minor=0;check(f_cuDeviceGetAttribute(&major,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,device));
  check(f_cuDeviceGetAttribute(&minor,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,device));
  // PTX for a widely supported architecture also runs on newer NVIDIA devices.
  std::string arch="--gpu-architecture=compute_"+std::to_string(std::min(major*10+minor,75));
  const char *opts[]={arch.c_str(),"--std=c++11"};
  static std::mutex mutex;static std::vector<char> ptx;
  std::lock_guard<std::mutex> lock(mutex);
  if(ptx.empty()){
  check(f_nvrtcCreateProgram(&program,kernel,"slime_gpu.cu",0,nullptr,nullptr));
  int rc=f_nvrtcCompileProgram(program,2,opts);
  if(rc){size_t size=0;f_nvrtcGetProgramLogSize(program,&size);std::vector<char> log(size);f_nvrtcGetProgramLog(program,log.data());fprintf(stderr,"NVRTC: %s\n",log.data());check(rc);}
  size_t size=0;check(f_nvrtcGetPTXSize(program,&size));ptx.resize(size);
  if(f_nvrtcGetPTX(program,ptx.data())){ptx.clear();throw std::runtime_error("PTX generation failed");}
  }
  check(f_cuModuleLoadData(&module,ptx.data()));
 }
 void alloc(CUdeviceptr &p,size_t bytes){check(f_cuMemAlloc(&p,bytes));}
 void put(CUdeviceptr p,const void *data,size_t size){check(f_cuMemcpyHtoD(p,data,size));}
 void get(void *data,CUdeviceptr p,size_t size){check(f_cuMemcpyDtoH(data,p,size));}
 void launch(CUfunction f,unsigned gx,unsigned gy,void **args){check(f_cuLaunchKernel(f,gx,gy,1,256,1,1,0,nullptr,args,nullptr));}
 ~GPU(){
  if(context&&f_cuCtxSetCurrent)f_cuCtxSetCurrent(context);
  if(f_cuMemFree)for(auto p:{xs,zs,ps,runs,hits,count})if(p)f_cuMemFree(p);
  if(module&&f_cuModuleUnload)f_cuModuleUnload(module);
  if(retained&&f_cuDevicePrimaryCtxRelease)f_cuDevicePrimaryCtxRelease(device);
  if(context&&f_cuCtxSetCurrent)f_cuCtxSetCurrent(previous);
  if(program&&f_nvrtcDestroyProgram)f_nvrtcDestroyProgram(&program);
  if(rtc)FreeLibrary(rtc);if(driver)FreeLibrary(driver);
 }
};

extern "C" __declspec(dllexport) int sr_search_gpu_v1(const SrParams *p,void *ctx,
 sr_result_fn result,sr_progress_fn progress,sr_gpu_control_fn control){
 if(!p||!result||!control||p->x1<p->x0||p->z1<p->z0||sr_verify_donut_runs())return SR_ERR_PARAM;
 if(p->x1==p->x0||p->z1==p->z0)return SR_OK;
 bool started=false;
 try{
  static std::mutex sessionMutex;std::lock_guard<std::mutex> sessionLock(sessionMutex);
  static std::unique_ptr<GPU> cached;
  constexpr int NX=65536,NZ=128,W=((NX+16+255)/256)*256,H=NZ+16;
  if(!cached){
   auto gpu=std::make_unique<GPU>();gpu->init();
   gpu->alloc(gpu->xs,W*8);gpu->alloc(gpu->zs,H*8);gpu->alloc(gpu->ps,size_t(W)*H*2);
   gpu->alloc(gpu->runs,SR_DONUT_RUNS*sizeof(SrDonutRun));gpu->alloc(gpu->hits,size_t(NX)*NZ*sizeof(SrResult));gpu->alloc(gpu->count,4);
   gpu->check(gpu->f_cuCtxSetCurrent(gpu->previous));cached=std::move(gpu);
  }
  GPU &gpu=*cached;
  struct Current{GPU &g;CUcontext before=nullptr;Current(GPU &g):g(g){g.check(g.f_cuCtxGetCurrent(&before));g.check(g.f_cuCtxSetCurrent(g.context));}~Current(){g.f_cuCtxSetCurrent(before);}} current(gpu);
  SrDonutRun runs[SR_DONUT_RUNS];sr_build_donut_runs(runs);gpu.put(gpu.runs,runs,sizeof(runs));
  CUfunction prefix=nullptr,score=nullptr;gpu.check(gpu.f_cuModuleGetFunction(&prefix,gpu.module,"prefix"));gpu.check(gpu.f_cuModuleGetFunction(&score,gpu.module,"score"));
  std::vector<uint64_t> xs(W),zs(H);std::vector<SrResult> hits;
  const uint64_t total=uint64_t(int64_t(p->x1)-p->x0)*uint64_t(int64_t(p->z1)-p->z0);uint64_t done=0;
  for(int64_t bz=p->z0;bz<p->z1;bz+=NZ)for(int64_t bx=p->x0;bx<p->x1;bx+=NX){
   if(progress)progress(ctx,done,total);if(!control(ctx))return SR_OK;
   int nx=int(std::min<int64_t>(NX,int64_t(p->x1)-bx)),nz=int(std::min<int64_t>(NZ,int64_t(p->z1)-bz));
   int width=((nx+16+255)/256)*256,bxi=int(bx),bzi=int(bz),thr=p->threshold;
   for(int i=0;i<width;i++)xs[i]=sr_xterm(bxi-8+i);
   for(int i=0;i<nz+16;i++)zs[i]=sr_zbase(p->world_seed,bzi-8+i);
   gpu.put(gpu.xs,xs.data(),width*8);gpu.put(gpu.zs,zs.data(),(nz+16)*8);
   unsigned count=0;gpu.put(gpu.count,&count,4);
   void *a[]={&gpu.xs,&gpu.zs,&gpu.ps,&width};started=true;gpu.launch(prefix,width/256,nz+16,a);
   void *b[]={&gpu.ps,&width,&nx,&nz,&gpu.runs,&thr,&bxi,&bzi,&gpu.hits,&gpu.count};gpu.launch(score,(nx+255)/256,1,b);
   gpu.check(gpu.f_cuCtxSynchronize());gpu.get(&count,gpu.count,4);
   if(count>unsigned(nx*nz))throw std::runtime_error("invalid GPU result count");
   hits.resize(count);if(count)gpu.get(hits.data(),gpu.hits,count*sizeof(SrResult));
   if(progress)progress(ctx,done,total);if(!control(ctx))return SR_OK;
   for(const auto &h:hits)result(ctx,h);done+=uint64_t(nx)*nz;
  }
  if(progress)progress(ctx,total,total);return SR_OK;
 }catch(const std::exception &e){fprintf(stderr,"Slime GPU: %s\n",e.what());return started?SR_GPU_FAILED:SR_GPU_UNAVAILABLE;}
}
