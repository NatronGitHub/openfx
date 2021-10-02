CUDAPATH ?= /usr/local/cuda
NVCC ?= $(CUDAPATH)/bin/nvcc
CUDA_LIBS ?= -L$(CUDAPATH)/lib -lcuda -lcudart
CXXFLAGS_ADD = -DOFX_EXTENSIONS_RESOLVE -I$(CUDAPATH)/include
LDFLAGS_ADD = $(CUDA_LIBS)

PLUGINOBJECTS = cuda.o kernel.o
PLUGINNAME = cuda
PATHTOROOT = ..

include ../Makefile.master

$(OBJECTPATH)/%.o : %.cu
	mkdir -p $(OBJECTPATH)
	$(NVCC) -c $(NVCFLAGS) $< -o $@
