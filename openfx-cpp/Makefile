# Minimal build for openfx-cpp (the header-only C++17 property/suite layer),
# usable without the CMake/conan build (NatronGitHub/openfx#80).
#
#   make check     compile every openfx-cpp header on its own, and the examples
#   make testprops build the TestProps plug-in bundle (examples/test-props)
#   make regen     regenerate the generated headers with scripts/gen-props.py
#                  (needs Python 3 with PyYAML; set PYTHON=...)
#   make clean
#
# On macOS, build with MacPorts: `port_switch; make -C openfx-cpp CXX=clang++-mp-22`.
# Extra flags (e.g. -DOFX_EXTENSIONS_NATRON) go in EXTRA_CXXFLAGS.

CXX ?= c++
CXXSTD ?= c++17
PYTHON ?= python3
OS := $(shell uname -s)

ROOT := $(abspath ..)
INCLUDES := -I$(ROOT)/include -I$(ROOT)/openfx-cpp/include -I$(ROOT)/openfx-cpp/third_party
CXXFLAGS ?= -O2 -Wall
CXXFLAGS += -std=$(CXXSTD) $(INCLUDES) $(EXTRA_CXXFLAGS)
ifeq ($(OS),Darwin)
  SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
  ifneq ($(SDKROOT),)
    CXXFLAGS += -isysroot $(SDKROOT)
  endif
  BUNDLE_ARCH := MacOS
  BUNDLE_LDFLAGS := -bundle
else
  BUNDLE_ARCH := Linux-$(shell uname -m | sed 's/x86_64/x86-64/')
  BUNDLE_LDFLAGS := -shared
endif

BUILD := build
HEADERS := $(wildcard include/openfx/*.h)
TESTPROPS := $(BUILD)/TestProps.ofx.bundle/Contents/$(BUNDLE_ARCH)/TestProps.ofx

.PHONY: all check testprops regen clean

all: check testprops

check:
	@mkdir -p $(BUILD)
	@set -e; for h in $(notdir $(HEADERS)); do \
	  echo "#include <openfx/$$h>" > $(BUILD)/check_$$h.cpp; \
	  echo "  CXX  openfx/$$h"; \
	  $(CXX) $(CXXFLAGS) -fsyntax-only $(BUILD)/check_$$h.cpp; \
	done
	@echo "  CXX  examples/host-specific-props/example-usage.cpp"
	@$(CXX) $(CXXFLAGS) -Iexamples/host-specific-props -fsyntax-only examples/host-specific-props/example-usage.cpp

testprops: $(TESTPROPS)

$(TESTPROPS): examples/test-props/testProperties.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC -fvisibility=hidden $(BUNDLE_LDFLAGS) $< -o $@
	cp examples/test-props/Info.plist $(BUILD)/TestProps.ofx.bundle/Contents/

regen:
	cd $(ROOT) && $(PYTHON) scripts/gen-props.py -v

clean:
	rm -rf $(BUILD)
