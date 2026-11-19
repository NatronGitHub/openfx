# Example plug-ins written against the raw C API (no Support library).
# Also built by Examples/Makefile.
set(PLUGINS
	Basic
	ChoiceParams
	ColourSpace
	Custom
	DepthConverter
	DrawSuite
	Invert
	OpenGL
	Overlay
	Rectangle
	Test)
# GPU render examples (OFX 1.5, ofxGPURender.h), each with a CPU fallback: OpenCL and CUDA with
# OFX_SUPPORTS_OPENCLRENDER and OFX_SUPPORTS_CUDARENDER, Metal on macOS
if(OFX_SUPPORTS_OPENCLRENDER)
	list(APPEND PLUGINS OpenCL)
endif()
if(OFX_SUPPORTS_CUDARENDER)
	list(APPEND PLUGINS Cuda)
endif()
if(APPLE)
	list(APPEND PLUGINS Metal)
endif()

foreach(PLUGIN IN LISTS PLUGINS)
	file(GLOB PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cpp")
	if(PLUGIN STREQUAL "Cuda")
		file(GLOB CUDA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cu")
		list(APPEND PLUGIN_SOURCES ${CUDA_SOURCES})
	endif()
	if(PLUGIN STREQUAL "Metal")
		# Objective-C++, compiled by the C++ compiler (the OBJCXX language isn't enabled, so CMake
		# compiles .mm files as CXX on Apple, and clang treats them as Objective-C++)
		list(APPEND PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/kernel.mm")
	endif()

	set(TGT example-${PLUGIN})
	add_ofx_plugin(${TGT} ${PLUGIN})
	target_sources(${TGT} PRIVATE ${PLUGIN_SOURCES})
	target_link_libraries(${TGT} PRIVATE OfxApi OpenGL::GL)
	target_include_directories(${TGT} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	if(PLUGIN STREQUAL "OpenCL")
		if(APPLE)
			target_link_libraries(${TGT} PRIVATE "-framework OpenCL")
		else()
			target_link_libraries(${TGT} PRIVATE OpenCL::Headers OpenCL::OpenCL)
		endif()
	endif()
	if(PLUGIN STREQUAL "Metal")
		target_link_libraries(${TGT} PRIVATE "-framework Metal" "-framework Foundation")
	endif()
endforeach()
