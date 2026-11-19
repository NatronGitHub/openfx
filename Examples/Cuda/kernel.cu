// The CUDA kernel of the Cuda example (cuda.cpp), compiled by nvcc.
// Software License: see cuda.cpp (The Open Effects Association Ltd, BSD-3-Clause).

// One thread per pixel of the render window (width x height). Each image is given by its device
// pointer, the offset of the first pixel of the window and the row length, in floats (see
// deviceLayout in cuda.cpp): float RGBA pixels, rows of increasing y.
__global__ void GainKernel(int p_Width, int p_Height, float p_RGain, float p_GGain, float p_BGain,
                           const float* p_Input, int p_InOffset, int p_InRow,
                           float* p_Output, int p_OutOffset, int p_OutRow)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if ((x < p_Width) && (y < p_Height))
    {
        const float* in = p_Input + p_InOffset + y * p_InRow + x * 4;
        float* out = p_Output + p_OutOffset + y * p_OutRow + x * 4;
        out[0] = in[0] * p_RGain;
        out[1] = in[1] * p_GGain;
        out[2] = in[2] * p_BGain;
        out[3] = in[3];
    }
}

// 0 (success), 1 (failed) or 2 (out of memory), see renderCuda in cuda.cpp
static int KernelStatus(cudaError_t p_Error)
{
    if (p_Error == cudaSuccess)
    {
        return 0;
    }
    return (p_Error == cudaErrorMemoryAllocation) ? 2 : 1;
}

// Launch the kernel on p_Stream, the host's cudaStream_t (kOfxImageEffectPropCudaStream), or on
// the default stream if it is NULL. Returns 0, 1 or 2 (see KernelStatus).
int RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Gain,
                  const float* p_Input, int p_InOffset, int p_InRow,
                  float* p_Output, int p_OutOffset, int p_OutRow)
{
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do (and an empty grid is a launch error)
    }
    // 2D blocks: the grid's y dimension is limited to 65535 blocks
    const dim3 threads(32, 8, 1);
    const dim3 blocks((p_Width + threads.x - 1) / threads.x, (p_Height + threads.y - 1) / threads.y, 1);
    cudaStream_t stream = static_cast<cudaStream_t>(p_Stream);

    // ofxGPURender.h, kOfxImageEffectPropCudaStream: "the plug-in SHOULD ensure that its render
    // action enqueues any asynchronous CUDA operations onto the supplied queue", and "SHOULD NOT
    // wait for final asynchronous operations to complete before returning from the render
    // action, and SHOULD NOT call cudaDeviceSynchronize() at any time."
    GainKernel<<<blocks, threads, 0, stream>>>(p_Width, p_Height, p_Gain[0], p_Gain[1], p_Gain[2],
                                               p_Input, p_InOffset, p_InRow,
                                               p_Output, p_OutOffset, p_OutRow);
    // launch errors. cudaPeekAtLastError doesn't clear the error state, which belongs to the
    // host's thread too: an earlier error of the host's own work is left for it to see (the
    // render then also fails, and the host may retry on the CPU).
    cudaError_t error = cudaPeekAtLastError();
    if ((error == cudaSuccess) && !stream)
    {
        // "If not set: the plug-in SHOULD ensure that any asynchronous operations it enqueues
        // have completed before returning from the render action." (a host that supports CUDA
        // render but not CUDA streams)
        error = cudaStreamSynchronize(0);
    }
    return KernelStatus(error);
}
