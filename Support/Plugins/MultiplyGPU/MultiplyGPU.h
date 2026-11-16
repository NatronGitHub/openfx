//
//  MultiplyGPU.h
//

#ifndef MultiplyGPU_h
#define MultiplyGPU_h

namespace OFX {
namespace Plugin {
namespace MultiplyGPU {
#ifdef HAVE_CUDA
void RunCUDAKernel(int p_Width, int p_Height, const float* p_Value, const float* p_Input, float* p_Output);
#endif
#ifdef HAVE_OPENCL
void RunOpenCLKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Value, const float* p_Input, float* p_Output);
#endif
}
}
}

#endif /* MultiplyGPU_h */
