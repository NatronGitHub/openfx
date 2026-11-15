# Example plug-ins using the Support library. Also built by Support/Plugins/Makefile.
set(OFX_SUPPORT_PLUGINS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include") # ofxsProcessing.H, ofxsImageBlender.H

set(PLUGINS
	Basic
	Field
	Generator
	Invert
	MultiBundle
	Retimer
	Tester
	Transition)
# MultiplyGPU needs the Resolve GPU extensions, and so a Support library built with them
# (the extension defines change Support's class layouts, so they must match).
option(BUILD_EXAMPLE_MULTIPLYGPU "Build the MultiplyGPU example (needs OFX_EXTENSIONS_RESOLVE)" ON)
if(OFX_EXTENSIONS_RESOLVE AND BUILD_EXAMPLE_MULTIPLYGPU)
	list(APPEND PLUGINS MultiplyGPU)
elseif(BUILD_EXAMPLE_MULTIPLYGPU)
	message(STATUS "Not building the MultiplyGPU example: it needs OFX_EXTENSIONS_RESOLVE=ON")
endif()

foreach(PLUGIN IN LISTS PLUGINS)
	file(GLOB PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cpp")
	if(PLUGIN STREQUAL "MultiplyGPU")
		if(NOT OFX_SUPPORTS_OPENCLRENDER)
			list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "MultiplyOpenCL")
		endif()
		if(OFX_SUPPORTS_CUDARENDER)
			file(GLOB CUDA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cu")
			list(APPEND PLUGIN_SOURCES ${CUDA_SOURCES})
		endif()
	endif()

	set(TGT example-${PLUGIN}-support)
	add_ofx_plugin(${TGT} ${PLUGIN})
	target_sources(${TGT} PRIVATE ${PLUGIN_SOURCES})
	target_link_libraries(${TGT} PRIVATE OfxSupport OpenGL::GL)
	target_include_directories(${TGT} PRIVATE ${OFX_SUPPORT_PLUGINS_INCLUDE_DIR})

	if(PLUGIN STREQUAL "MultiplyGPU")
		if(OFX_SUPPORTS_OPENCLRENDER)
			target_compile_definitions(${TGT} PRIVATE HAVE_OPENCL)
			if(APPLE)
				target_link_libraries(${TGT} PRIVATE "-framework OpenCL")
			else()
				target_link_libraries(${TGT} PRIVATE OpenCL::Headers OpenCL::OpenCL)
			endif()
		endif()
		if(OFX_SUPPORTS_CUDARENDER)
			target_compile_definitions(${TGT} PRIVATE HAVE_CUDA)
		endif()
	endif()
endforeach()
