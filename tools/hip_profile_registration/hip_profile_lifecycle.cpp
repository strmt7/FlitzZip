// FlitzZip development instrumentation. The repository AGPL-3.0 license applies.
extern "C" {
// Provide LLVM's explicit-initialization sentinel instead of its automatic
// atexit initializer: DLL detach holds the Windows loader lock and cannot drain HIP.
int __llvm_profile_runtime = 0;

// Purpose: Initialize the profiler's configured filename outside DLL entrypoints.
// Inputs: Process-scoped LLVM profiling environment. Outputs: Native filename state; no exit callback registration.
void __llvm_profile_initialize_file();

// Purpose: Drain the actual registered device counters through the matching ROCm runtime.
// Inputs: Live admitted HIP module and completed device work. Outputs: Native profiles and zero on success.
int __llvm_profile_hip_collect_device_data();

// Purpose: Finish device measurement before the owning executable releases its kernel DLL.
// Inputs: Module remains loaded and all operations have completed. Outputs: Native collection status; no exceptions.
__declspec(dllexport) int flitzzip_hip_profile_finish_v1() noexcept {
    __llvm_profile_initialize_file();
    return __llvm_profile_hip_collect_device_data();
}
}
