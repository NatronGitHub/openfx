# Example plug-ins using the Support library. Also built by Support/Plugins/Makefile.
set(OFX_SUPPORT_PLUGINS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include") # ofxsProcessing.H, ofxsImageBlender.H

set(PLUGINS
	Basic
	ChoiceParams
	Field
	Generator
	GPUGain
	Invert
	MultiBundle
	Retimer
	Tester
	Transition)
# GPUGain's Metal render (macOS only; Makefile: HAVE_METAL=0 to turn it off)
if(APPLE)
	option(OFX_EXAMPLE_GPUGAIN_METAL "Build the GPUGain example with Metal render" ON)
else()
	set(OFX_EXAMPLE_GPUGAIN_METAL OFF)
endif()

foreach(PLUGIN IN LISTS PLUGINS)
	file(GLOB PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cpp")
	# GPUGain (ported from upstream OpenFX): CPU render, plus Metal render on macOS (MetalKernel.mm,
	# compiled as Objective-C++ by the C++ compiler), CUDA and OpenCL render when
	# OFX_SUPPORTS_CUDARENDER / OFX_SUPPORTS_OPENCLRENDER are on. Only the paths built in are
	# declared to the host.
	if(PLUGIN STREQUAL "GPUGain")
		if(NOT OFX_SUPPORTS_OPENCLRENDER)
			list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "OpenCLKernel")
		endif()
		if(OFX_EXAMPLE_GPUGAIN_METAL)
			list(APPEND PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/MetalKernel.mm")
		endif()
		if(OFX_SUPPORTS_CUDARENDER)
			list(APPEND PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/CudaKernel.cu")
		endif()
	endif()

	set(TGT example-${PLUGIN}-support)
	add_ofx_plugin(${TGT} ${PLUGIN})
	target_sources(${TGT} PRIVATE ${PLUGIN_SOURCES})
	target_link_libraries(${TGT} PRIVATE OfxSupport)
	# only the plug-ins whose overlays call OpenGL themselves link it: the Support library loads it
	# at run time (DrawArgs::getDrawContext(), used by Basic), so the others load without OpenGL
	if(PLUGIN STREQUAL "ChoiceParams" OR PLUGIN STREQUAL "MultiBundle" OR PLUGIN STREQUAL "Tester"
	   OR (PLUGIN STREQUAL "GPUGain" AND OFX_SUPPORTS_OPENGLRENDER))
		target_link_libraries(${TGT} PRIVATE OpenGL::GL)
	endif()
	target_include_directories(${TGT} PRIVATE ${OFX_SUPPORT_PLUGINS_INCLUDE_DIR})

	if(PLUGIN STREQUAL "GPUGain")
		if(OFX_EXAMPLE_GPUGAIN_METAL)
			target_compile_definitions(${TGT} PRIVATE HAVE_METAL)
			target_link_libraries(${TGT} PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore")
		endif()
		if(OFX_SUPPORTS_OPENCLRENDER)
			target_compile_definitions(${TGT} PRIVATE HAVE_OPENCL)
			if(APPLE)
				target_link_libraries(${TGT} PRIVATE "-framework OpenCL")
			else()
				target_link_libraries(${TGT} PRIVATE $<TARGET_NAME_IF_EXISTS:OpenCL::Headers> OpenCL::OpenCL)
			endif()
		endif()
		if(OFX_SUPPORTS_CUDARENDER)
			target_compile_definitions(${TGT} PRIVATE HAVE_CUDA)
		endif()
	endif()
endforeach()
