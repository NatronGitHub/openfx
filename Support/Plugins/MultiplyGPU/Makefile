HAVE_CUDA ?= 0
HAVE_OPENGL ?= 0

PLUGINOBJECTS = MultiplyGPU.o
PLUGINNAME = MultiplyGPU
PATHTOROOT = ../..

ifeq ($(HAVE_CUDA),1)
PLUGINOBJECTS += MultiplyCUDA.o

CUDAPATH ?= /usr/local/cuda
NVCC ?= $(CUDAPATH)/bin/nvcc
CUDA_CXXFLAGS ?= -D HAVE_CUDA -I$(CUDAPATH)/include
# nvcc flags for MultiplyCUDA.cu (Makefile.master's %.cu rule uses CUFLAGS)
CUFLAGS ?= $(CUDA_CXXFLAGS) -Xcompiler -fPIC
ifeq ($(shell uname -s),Darwin)
CUDA_LIBS ?= -L$(CUDAPATH)/lib -lcuda -lcudart -F/Library/Frameworks -framework CUDA
else
CUDA_LIBS ?= -L$(CUDAPATH)/lib64 -lcudart
endif
endif

ifeq ($(HAVE_OPENCL),1)
PLUGINOBJECTS += MultiplyOpenCL.o

OPENCL_CXXFLAGS ?= -DHAVE_OPENCL
ifeq ($(shell uname -s),Darwin)
OPENCL_LIBS ?= -framework OpenCL
else
OPENCL_LIBS ?= -lOpenCL
endif
endif

CXXFLAGS_ADD = $(CUDA_CXXFLAGS) $(OPENCL_CXXFLAGS)
LDFLAGS_ADD = $(CUDA_LIBS) $(OPENCL_LIBS)



include ../Makefile.master
