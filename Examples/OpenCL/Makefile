PLUGINOBJECTS = oclplugin.o
PLUGINNAME = oclplugin
PATHTOROOT = ..
# the OpenCL SDK: MSYS2's prefix on MinGW (MINGW_PREFIX, set by the MSYS2 shells), else /usr
OPENCL_HOME ?= $(if $(MINGW_PREFIX),$(MINGW_PREFIX),/usr)
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
