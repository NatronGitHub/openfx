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

// The other formats (bytes, shorts, and floats with 1 or 3 components): packed rows of p_Width
// pixels of p_Components components of type PIX. Integers (maxValue 255 or 65535) are clamped to
// [0, maxValue] and rounded to the nearest value, floats are not clamped (as GPUGain.cpp's
// scaledComponent). __fmul_rn: a product rounded to float, never fused with the + 0.5f into a
// multiply-add (nvcc's default --fmad=true), which could round a result one step away from the
// CPU path's.
struct Gains
{
    float v[4];
};

template <class PIX, int maxValue>
__device__ PIX ScaledComponent(PIX p_Value, float p_Gain)
{
    const float v = __fmul_rn(static_cast<float>(p_Value), p_Gain);
    if (maxValue == 1)
    {
        return static_cast<PIX>(v);
    }
    return static_cast<PIX>((v > 0.f ? (v < maxValue ? v : static_cast<float>(maxValue)) : 0.f) + 0.5f);
}

template <class PIX, int maxValue>
__global__ void GainAdjustFormatKernel(int p_Width, int p_Height, int p_Components, Gains p_Gain, const PIX* p_Input, PIX* p_Output)
{
   const int x = blockIdx.x * blockDim.x + threadIdx.x;
   const int y = blockIdx.y * blockDim.y + threadIdx.y;

   if ((x < p_Width) && (y < p_Height))
   {
       const int index = ((y * p_Width) + x) * p_Components;

       for (int c = 0; c < p_Components; ++c)
       {
           p_Output[index + c] = ScaledComponent<PIX, maxValue>(p_Input[index + c], p_Gain.v[c]);
       }
   }
}

template <class PIX, int maxValue>
static void LaunchFormat(dim3 p_Blocks, dim3 p_Threads, cudaStream_t p_Stream, int p_Width, int p_Height, int p_Components,
                         const float* p_Gain, const void* p_Input, void* p_Output)
{
    const Gains gain = { { p_Gain[0], p_Gain[1], p_Gain[2], p_Gain[3] } };
    GainAdjustFormatKernel<PIX, maxValue><<<p_Blocks, p_Threads, 0, p_Stream>>>(
        p_Width, p_Height, p_Components, gain, static_cast<const PIX*>(p_Input), static_cast<PIX*>(p_Output));
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

// returns 0 (success), 1 (failed) or 2 (out of memory). p_Components: 1, 3 or 4; p_ComponentBytes:
// 1 (bytes), 2 (shorts) or 4 (floats). Float RGBA uses GainAdjustKernel, the others
// GainAdjustFormatKernel.
int RunCudaKernel(void* p_Stream, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                  const float* p_Gain, const void* p_Input, void* p_Output)
{
    if (((p_Components != 1) && (p_Components != 3) && (p_Components != 4)) ||
        ((p_ComponentBytes != 1) && (p_ComponentBytes != 2) && (p_ComponentBytes != 4)))
    {
        return 1;
    }
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do (and an empty grid is a launch error)
    }
    dim3 threads(128, 1, 1);
    dim3 blocks(((p_Width + threads.x - 1) / threads.x), p_Height, 1);
    cudaStream_t stream = static_cast<cudaStream_t>(p_Stream);

    if ((p_Components == 4) && (p_ComponentBytes == 4))
    {
        GainAdjustKernel<<<blocks, threads, 0, stream>>>(p_Width, p_Height, p_Gain[0], p_Gain[1], p_Gain[2], p_Gain[3],
                                                         static_cast<const float*>(p_Input), static_cast<float*>(p_Output));
    }
    else if (p_ComponentBytes == 1)
    {
        LaunchFormat<unsigned char, 255>(blocks, threads, stream, p_Width, p_Height, p_Components, p_Gain, p_Input, p_Output);
    }
    else if (p_ComponentBytes == 2)
    {
        LaunchFormat<unsigned short, 65535>(blocks, threads, stream, p_Width, p_Height, p_Components, p_Gain, p_Input, p_Output);
    }
    else
    {
        LaunchFormat<float, 1>(blocks, threads, stream, p_Width, p_Height, p_Components, p_Gain, p_Input, p_Output);
    }
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
