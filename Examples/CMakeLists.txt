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
# GPU examples use the Resolve-era GPU extensions (OFX_EXTENSIONS_RESOLVE)
if(OFX_SUPPORTS_OPENCLRENDER)
	list(APPEND PLUGINS OpenCL)
endif()
if(OFX_SUPPORTS_CUDARENDER)
	list(APPEND PLUGINS Cuda)
endif()

foreach(PLUGIN IN LISTS PLUGINS)
	file(GLOB PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cpp")
	if(PLUGIN STREQUAL "Cuda")
		file(GLOB CUDA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN}/*.cu")
		list(APPEND PLUGIN_SOURCES ${CUDA_SOURCES})
	endif()

	set(TGT example-${PLUGIN})
	add_ofx_plugin(${TGT} ${PLUGIN})
	target_sources(${TGT} PRIVATE ${PLUGIN_SOURCES})
	target_link_libraries(${TGT} PRIVATE OfxApi OpenGL::GL)
	target_include_directories(${TGT} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	if(PLUGIN STREQUAL "OpenCL" OR PLUGIN STREQUAL "Cuda")
		target_compile_definitions(${TGT} PRIVATE OFX_EXTENSIONS_RESOLVE)
	endif()
	if(PLUGIN STREQUAL "OpenCL")
		if(APPLE)
			target_link_libraries(${TGT} PRIVATE "-framework OpenCL")
		else()
			target_link_libraries(${TGT} PRIVATE OpenCL::Headers OpenCL::OpenCL)
		endif()
	endif()
endforeach()
