# The C++ plug-in support library. Plug-in repositories (openfx-misc, ...) usually
# compile these sources directly into each plug-in; this target is for CMake users.
# ofxsHWNDInteract.cpp is only meaningful on Windows (Vegas HWND interacts);
# elsewhere it compiles to nothing.
set(OFX_SUPPORT_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB OFX_SUPPORT_LIBRARY_FILES "${OFX_SUPPORT_LIBRARY_DIR}/*.cpp")

add_library(OfxSupport STATIC
	${OFX_HEADER_FILES}
	${OFX_SUPPORT_HEADER_FILES}
	${OFX_SUPPORT_LIBRARY_FILES})

set_target_properties(OfxSupport PROPERTIES
	LINKER_LANGUAGE CXX
	POSITION_INDEPENDENT_CODE ON
	EXPORT_NAME Support)

# The sources are C++98-compatible; consumers need C++11 or later. The library is built with the
# plug-ins' standard (OFX_CXX_STANDARD, default 17, cmake/OpenFX.cmake), since it is linked into them.
target_compile_features(OfxSupport PUBLIC cxx_std_11)
set_target_properties(OfxSupport PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(OfxSupport PUBLIC ${OFX_DEFINITIONS} $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>)
target_link_libraries(OfxSupport PUBLIC OfxApi)
# Plugins link this library in and should export only the OFX entry points,
# which ofxCore.h declares with OfxExport, so hide everything else.
set_target_properties(OfxSupport PROPERTIES CXX_VISIBILITY_PRESET hidden
                                            VISIBILITY_INLINES_HIDDEN ON)

# Support/Plugins/include (ofxsProcessing.H, ofxsImageBlender.H...) is used by most plug-ins, and
# the Makefiles always add it (Support/Plugins/Makefile.master)
target_include_directories(OfxSupport PUBLIC
	$<BUILD_INTERFACE:${OFX_SUPPORT_HEADER_DIR}>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../Plugins/include>
	$<INSTALL_INTERFACE:include/Support>
	$<INSTALL_INTERFACE:include/Support/Plugins>)

# the plug-ins' optimization flags (OFX_OPTFLAG), so that a plug-in and its Support code match
ofx_target_optflag(OfxSupport)

add_library(openfx::Support ALIAS OfxSupport)
