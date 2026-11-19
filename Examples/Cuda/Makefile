# CUDA render example, Linux (on Windows, use CMake with MSVC; macOS has no CUDA): needs nvcc and
# the CUDA runtime (CUDAPATH). cuda.cpp itself has no CUDA dependency; kernel.cu is compiled by nvcc.
CUDAPATH ?= /usr/local/cuda
NVCC ?= $(CUDAPATH)/bin/nvcc
# GPU architectures: every major one (nvcc >= 11.5); e.g. CUDA_ARCH= for nvcc's default
CUDA_ARCH ?= -arch=all-major
NVCFLAGS ?= $(CUDA_ARCH) -Xcompiler -fPIC
ifeq ($(shell uname -s),Linux)
# the static CUDA runtime, as CMake's default (CMAKE_CUDA_RUNTIME_LIBRARY): the plug-in doesn't
# depend on a libcudart.so, which it would have no rpath to find
CUDA_LIBS ?= -L$(CUDAPATH)/lib64 -lcudart_static -ldl -lrt -pthread
else
CUDA_LIBS ?= -L$(CUDAPATH)/lib -lcudart
endif
LDFLAGS_ADD = $(CUDA_LIBS)

PLUGINOBJECTS = cuda.o kernel.o
PLUGINNAME = cuda
PATHTOROOT = ..

include ../Makefile.master

$(OBJECTPATH)/%.o : %.cu
	mkdir -p $(OBJECTPATH)
	$(NVCC) -c $(NVCFLAGS) $< -o $@
