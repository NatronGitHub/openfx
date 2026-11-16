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

# The library compiles as C++98; consumers use C++11 or later.
target_compile_features(OfxSupport PUBLIC cxx_std_11)
target_compile_definitions(OfxSupport PUBLIC ${OFX_DEFINITIONS})
target_link_libraries(OfxSupport PUBLIC OfxApi)

# Plugins link this library in and should export only the OFX entry points,
# which ofxCore.h declares with OfxExport, so hide everything else.
set_target_properties(OfxSupport PROPERTIES CXX_VISIBILITY_PRESET hidden
                                            VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(OfxSupport PUBLIC
	$<BUILD_INTERFACE:${OFX_SUPPORT_HEADER_DIR}>
	$<INSTALL_INTERFACE:include/Support>)

add_library(openfx::Support ALIAS OfxSupport)
