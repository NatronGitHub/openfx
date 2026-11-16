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

# The library compiles as C++98; consumers use C++11 or later. It is built with the plug-ins'
# standard (OFX_CXX_STANDARD, cmake/OpenFX.cmake), since it is linked into them.
target_compile_features(OfxSupport PUBLIC cxx_std_11)
set_target_properties(OfxSupport PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(OfxSupport PUBLIC ${OFX_DEFINITIONS})
target_link_libraries(OfxSupport PUBLIC OfxApi)
# Support/Plugins/include (ofxsProcessing.H, ofxsImageBlender.H...) is used by most plug-ins, and
# the Makefiles always add it (Support/Plugins/Makefile.master)
target_include_directories(OfxSupport PUBLIC
	$<BUILD_INTERFACE:${OFX_SUPPORT_HEADER_DIR}>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../Plugins/include>
	$<INSTALL_INTERFACE:include/Support>
	$<INSTALL_INTERFACE:include/Support/Plugins>)

add_library(openfx::Support ALIAS OfxSupport)
