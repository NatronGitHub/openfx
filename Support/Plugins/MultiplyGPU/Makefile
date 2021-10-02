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
CUDA_LIBS ?= -L$(CUDAPATH)/lib -lcuda -lcudart -F/Library/Frameworks -framework CUDA
endif

ifeq ($(HAVE_OPENCL),1)
PLUGINOBJECTS += MultiplyOpenCL.o

OPENCL_CXXFLAGS ?= -DHAVE_OPENCL
OPENCL_LIBS ?= -framework OpenCL
endif

CXXFLAGS_ADD = $(CUDA_CXXFLAGS) $(OPENCL_CXXFLAGS) -DOFX_EXTENSIONS_RESOLVE
LDFLAGS_ADD = $(CUDA_LIBS) $(OPENCL_LIBS)



include ../Makefile.master
