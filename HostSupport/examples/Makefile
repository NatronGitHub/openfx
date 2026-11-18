DEBUG ?= false
# expat (the system one): compiler and linker flags from pkg-config, or EXPAT_INCLUDE=<dir> and
# EXPAT_LIBS="-L<dir> -lexpat"
ifdef EXPAT_INCLUDE
  EXPAT_CFLAGS ?= -I$(EXPAT_INCLUDE)
else
  EXPAT_CFLAGS ?= $(shell pkg-config --cflags expat 2>/dev/null)
endif
EXPAT_LIBS ?= $(shell pkg-config --libs expat 2>/dev/null || echo -lexpat)
OBJSUF ?= .o
LIBSUF ?= .a
LIBPREFIX ?= lib
LIBNAME ?= ofxHost
OS = $(shell uname)
DEBUG ?= false
OFXSLIB = $(LIBPREFIX)$(LIBNAME)$(LIBSUF)

ifeq ($(DEBUG), true)
  DST_DIR = $(OS)-debug
  OPTIMISE ?= -g -Wall
else ifeq ($(DEBUG), instrument)
  DST_DIR = $(OS)-instrument
  OPTIMISE ?= -g -O3 -Wall
else
  DST_DIR = $(OS)-release
  OPTIMISE ?= -O2 -Wall
endif

INCFLAGS = -I../include -I../../include $(EXPAT_CFLAGS)
CXXFLAGS = $(INCFLAGS) $(OPTIMISE)

HOST_DEMO_FILES = $(DST_DIR)/hostDemo.o \
	$(DST_DIR)/hostDemoClipInstance.o     \
	$(DST_DIR)/hostDemoEffectInstance.o   \
	$(DST_DIR)/hostDemoHostDescriptor.o   \
	$(DST_DIR)/hostDemoParamInstance.o    

all : $(DST_DIR)/hostDemo $(DST_DIR)/cacheDemo $(DST_DIR)/propsetDump

clean :
	rm -f $(DST_DIR)/*.o $(DST_DIR)/cacheDemo $(DST_DIR)/hostDemo $(DST_DIR)/propsetDump
	cd ..; make clean DEBUG=$(DEBUG) EXPAT_CFLAGS="$(EXPAT_CFLAGS)" OBJSUF=$(OBJSUF) LIBSUF=$(LIBSUF) \
	LIBPREFIX=$(LIBPREFIX) LIBNAME=$(LIBNAME); 

$(OFXSLIB):
	cd ..; make DEBUG=$(DEBUG) EXPAT_CFLAGS="$(EXPAT_CFLAGS)" OBJSUF=$(OBJSUF) LIBSUF=$(LIBSUF) \
	LIBPREFIX=$(LIBPREFIX) LIBNAME=$(LIBNAME); 


# the objects depend on the hostDemo headers (MyEffectInstance and MyClipInstance are shared)
HOST_DEMO_HEADERS = $(wildcard hostDemo*.h)

$(HOST_DEMO_FILES) : $(DST_DIR)/%.o : %.cpp $(HOST_DEMO_HEADERS)
	mkdir -p $(DST_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(DST_DIR)/cacheDemo : cacheDemo.cpp $(OFXSLIB)
	mkdir -p $(DST_DIR)
	$(CXX) $(CXXFLAGS) cacheDemo.cpp -o $(DST_DIR)/cacheDemo -L../$(DST_DIR) -lofxHost $(EXPAT_LIBS) -ldl

$(DST_DIR)/hostDemo : $(HOST_DEMO_FILES)  $(OFXSLIB)
	mkdir -p $(DST_DIR)
	$(CXX) $(CXXFLAGS) $(HOST_DEMO_FILES) -o $(DST_DIR)/hostDemo -L../$(DST_DIR) -lofxHost $(EXPAT_LIBS) -ldl

# propsetDump: dump the property sets HostSupport creates (used by scripts/audit-propsets.py)
PROPSET_DUMP_FILES = $(DST_DIR)/propsetDump.o \
	$(DST_DIR)/hostDemoClipInstance.o     \
	$(DST_DIR)/hostDemoEffectInstance.o   \
	$(DST_DIR)/hostDemoHostDescriptor.o   \
	$(DST_DIR)/hostDemoParamInstance.o

$(DST_DIR)/propsetDump.o : propsetDump.cpp $(HOST_DEMO_HEADERS)
	mkdir -p $(DST_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(DST_DIR)/propsetDump : $(PROPSET_DUMP_FILES) $(OFXSLIB)
	mkdir -p $(DST_DIR)
	$(CXX) $(CXXFLAGS) $(PROPSET_DUMP_FILES) -o $(DST_DIR)/propsetDump -L../$(DST_DIR) -lofxHost $(EXPAT_LIBS) -ldl
