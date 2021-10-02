PLUGINOBJECTS = oclplugin.o
PLUGINNAME = oclplugin
PATHTOROOT = ..
OPENCL_HOME ?= /usr
CXXFLAGS_ADD = -DOFX_EXTENSIONS_RESOLVE -I$(OPENCL_HOME)/include
LDFLAGS_ADD = $(OPENCL_LIBS)

include ../Makefile.master

ifeq ($(OS),$(filter $(OS),MINGW64_NT-6.1 MINGW32_NT-6.1))
  OPENCL_LIBS= -L$(OPENCL_HOME)/lib -lOpenCL
endif
ifeq ($(OS),Linux)
  OPENCL_LIBS= -L$(OPENCL_HOME)/lib -lOpenCL
endif
ifeq ($(OS),FreeBSD)
  OPENCL_LIBS= -L$(OPENCL_HOME)/lib -lOpenCL
endif
ifeq ($(OS),Darwin)
  OPENCL_LIBS= -framework OpenCL
endif
