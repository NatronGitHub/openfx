// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

__global__ void GainAdjustKernel(int p_Width, int p_Height, float p_GainR, float p_GainG, float p_GainB, float p_GainA, const float* p_Input, float* p_Output)
{
   const int x = blockIdx.x * blockDim.x + threadIdx.x;
   const int y = blockIdx.y * blockDim.y + threadIdx.y;

   if ((x < p_Width) && (y < p_Height))
   {
       const int index = ((y * p_Width) + x) * 4;

       p_Output[index + 0] = p_Input[index + 0] * p_GainR;
       p_Output[index + 1] = p_Input[index + 1] * p_GainG;
       p_Output[index + 2] = p_Input[index + 2] * p_GainB;
       p_Output[index + 3] = p_Input[index + 3] * p_GainA;
   }
}

// the status of a CUDA error: 0 (success), 1 (failed), 2 (out of memory), see GPUGain.cpp
static int KernelStatus(cudaError_t p_Error)
{
    if (p_Error == cudaSuccess)
    {
        return 0;
    }
    return (p_Error == cudaErrorMemoryAllocation) ? 2 : 1;
}

// returns 0 (success), 1 (failed) or 2 (out of memory)
int RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Gain, const float* p_Input, float* p_Output)
{
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do (and an empty grid is a launch error)
    }
    dim3 threads(128, 1, 1);
    dim3 blocks(((p_Width + threads.x - 1) / threads.x), p_Height, 1);
    cudaStream_t stream = static_cast<cudaStream_t>(p_Stream);

    GainAdjustKernel<<<blocks, threads, 0, stream>>>(p_Width, p_Height, p_Gain[0], p_Gain[1], p_Gain[2], p_Gain[3], p_Input, p_Output);
    cudaError_t error = cudaGetLastError();
    if (error != cudaSuccess)
    {
        return KernelStatus(error);
    }

    // kOfxImageEffectPropCudaStream (ofxGPURender.h): "If not set: the plug-in SHOULD ensure
    // that any asynchronous operations it enqueues have completed before returning from the
    // render action." (a host that supports CUDA render but not CUDA streams)
    if (!stream)
    {
        error = cudaStreamSynchronize(0);
    }
    return KernelStatus(error);
}
