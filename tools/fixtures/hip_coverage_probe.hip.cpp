#include <hip/hip_runtime.h>

#include <algorithm>
#include <array>
#include <iostream>

// Purpose: Supply independent device hit and miss controls across multiple workgroups.
// Inputs: 192 allocated integers and a binary selector. Outputs: One known store per lane without shared writes.
__global__ void coverage_counter_probe(int* output, int selector) {
    const auto index = blockIdx.x * blockDim.x + threadIdx.x;
    if (selector != 0) {
        output[index] = 17;
    } else {
        output[index] = 23;
    }
}

// Purpose: Qualify real GPU counters using 768 bytes and three independent workgroups.
// Inputs: No argument selects false; one selects true. Outputs: Exact readback and device architecture, or failure.
int main(int argc, char**) {
    int count = 0;
    hipDeviceProp_t properties{};
    if (argc > 2 || hipGetDeviceCount(&count) != hipSuccess || count < 1 || hipSetDevice(0) != hipSuccess ||
        hipGetDeviceProperties(&properties, 0) != hipSuccess) {
        return 10;
    }
    int* device = nullptr;
    std::array<int, 192> output{};
    if (hipMalloc(&device, sizeof(output)) != hipSuccess) {
        return 11;
    }
    const int selector = argc == 2 ? 1 : 0;
    coverage_counter_probe<<<3, 64>>>(device, selector);
    const auto launch = hipGetLastError();
    const auto copied =
        launch == hipSuccess ? hipMemcpy(output.data(), device, sizeof(output), hipMemcpyDeviceToHost) : launch;
    const auto freed = hipFree(device);
    if (launch != hipSuccess || copied != hipSuccess || freed != hipSuccess) {
        std::cerr << "launch=" << hipGetErrorName(launch) << " copy=" << hipGetErrorName(copied)
                  << " free=" << hipGetErrorName(freed) << '\n';
        return 12;
    }
    const auto expected = selector == 1 ? 17 : 23;
    if (!std::ranges::all_of(output, [expected](int value) { return value == expected; })) {
        return 13;
    }
    std::cout << "{\"selector\":" << selector << ",\"value\":" << expected << ",\"lanes\":" << output.size()
              << ",\"architecture\":\"" << properties.gcnArchName << "\"}\n";
    return 0;
}
