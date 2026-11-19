PLUGINOBJECTS = oclplugin.o
PLUGINNAME = oclplugin
PATHTOROOT = ..
OPENCL_HOME ?= /usr
# OpenCL headers and library (the OpenCL framework on macOS)
CXXFLAGS_ADD = -I$(OPENCL_HOME)/include
LDFLAGS_ADD = $(OPENCL_LIBS)

include ../Makefile.master

ifneq ($(MINGW_OS),)
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
