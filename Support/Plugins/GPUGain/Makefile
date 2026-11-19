# Copyright OpenFX and contributors to the OpenFX project.
# SPDX-License-Identifier: BSD-3-Clause

# GPUGain: CPU render, plus optional GPU render paths, off unless enabled
# (the CPU path always works, also on hosts without GPU render support):
#   HAVE_METAL=1  Metal (macOS only, on by default there)
#   HAVE_CUDA=1   CUDA (needs nvcc, CUDAPATH)
#   HAVE_OPENCL=1 OpenCL buffers and images
HAVE_CUDA ?= 0
HAVE_OPENCL ?= 0
ifeq ($(shell uname -s),Darwin)
HAVE_METAL ?= 1
else
HAVE_METAL = 0
endif

PLUGINOBJECTS = GPUGain.o
PLUGINNAME = GPUGain
PATHTOROOT = ../..

ifeq ($(HAVE_METAL),1)
PLUGINOBJECTS += MetalKernel.o
METAL_CXXFLAGS ?= -DHAVE_METAL
METAL_LIBS ?= -framework Metal -framework Foundation -framework QuartzCore
endif

ifeq ($(HAVE_CUDA),1)
PLUGINOBJECTS += CudaKernel.o

CUDAPATH ?= /usr/local/cuda
NVCC ?= $(CUDAPATH)/bin/nvcc
CUDA_CXXFLAGS ?= -D HAVE_CUDA -I$(CUDAPATH)/include
# GPU architectures: every major one (nvcc >= 11.5); e.g. CUDA_ARCH= for nvcc's default
CUDA_ARCH ?= -arch=all-major
# nvcc flags for CudaKernel.cu (Makefile.master's %.cu rule uses CUFLAGS)
CUFLAGS ?= $(CUDA_CXXFLAGS) $(CUDA_ARCH) -Xcompiler -fPIC
ifeq ($(shell uname -s),Darwin)
CUDA_LIBS ?= -L$(CUDAPATH)/lib -lcudart
else
CUDA_LIBS ?= -L$(CUDAPATH)/lib64 -lcudart
endif
endif

ifeq ($(HAVE_OPENCL),1)
PLUGINOBJECTS += OpenCLKernel.o

OPENCL_CXXFLAGS ?= -DHAVE_OPENCL
ifeq ($(shell uname -s),Darwin)
OPENCL_LIBS ?= -framework OpenCL
else
OPENCL_LIBS ?= -lOpenCL
endif
endif

CXXFLAGS_ADD = $(METAL_CXXFLAGS) $(CUDA_CXXFLAGS) $(OPENCL_CXXFLAGS)
LDFLAGS_ADD = $(METAL_LIBS) $(CUDA_LIBS) $(OPENCL_LIBS)

include ../Makefile.master
