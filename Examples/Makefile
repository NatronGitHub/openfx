SUBDIRS = Basic ChoiceParams ColourSpace Custom DepthConverter DrawSuite Invert OpenGL Overlay Rectangle Test

# GPU render examples (OFX 1.5, ofxGPURender.h), each with a CPU fallback:
#   HAVE_METAL=1  Metal (macOS only, on by default there)
#   HAVE_OPENCL=1 OpenCL (needs the OpenCL headers and library: OPENCL_HOME, see OpenCL/Makefile)
#   HAVE_CUDA=1   CUDA (needs nvcc and the CUDA runtime: CUDAPATH, see Cuda/Makefile)
HAVE_OPENCL ?= 0
HAVE_CUDA ?= 0
ifeq ($(shell uname -s),Darwin)
HAVE_METAL ?= 1
else
override HAVE_METAL = 0
endif
ifeq ($(HAVE_METAL),1)
SUBDIRS += Metal
endif
ifeq ($(HAVE_OPENCL),1)
SUBDIRS += OpenCL
endif
ifeq ($(HAVE_CUDA),1)
SUBDIRS += Cuda
endif

all: subdirs

.PHONY: subdirs clean $(SUBDIRS)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean :
	for i in $(SUBDIRS) ; do \
	  $(MAKE) -C $$i clean; \
	done
