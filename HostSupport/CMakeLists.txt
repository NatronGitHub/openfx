set(OFX_HOSTSUPPORT_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(OFX_HOSTSUPPORT_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB OFX_HOSTSUPPORT_HEADER_FILES "${OFX_HOSTSUPPORT_HEADER_DIR}/*.h")
file(GLOB OFX_HOSTSUPPORT_LIBRARY_FILES "${OFX_HOSTSUPPORT_LIBRARY_DIR}/*.cpp")

add_library(OfxHost STATIC
	${OFX_HEADER_FILES}
	${OFX_HOSTSUPPORT_HEADER_FILES}
	${OFX_HOSTSUPPORT_LIBRARY_FILES})

set_target_properties(OfxHost PROPERTIES
	LINKER_LANGUAGE CXX
	POSITION_INDEPENDENT_CODE ON
	EXPORT_NAME HostSupport)

# C++17 or later, as upstream, for the library and the hosts that use it.
target_compile_features(OfxHost PUBLIC cxx_std_17)
target_compile_definitions(OfxHost PUBLIC ${OFX_DEFINITIONS})
# expat.h is included by the public header ofxhPluginCache.h
target_link_libraries(OfxHost PUBLIC OfxApi expat::expat)
if(UNIX)
	target_link_libraries(OfxHost PUBLIC ${CMAKE_DL_LIBS})
endif()

target_include_directories(OfxHost PUBLIC
	$<BUILD_INTERFACE:${OFX_HOSTSUPPORT_HEADER_DIR}>
	$<INSTALL_INTERFACE:include/HostSupport>)

add_library(openfx::HostSupport ALIAS OfxHost)

# openfx::HostSupportOpenGL: offscreen OpenGL contexts, textures and renders (ofxhOpenGLContext.h,
# ofxhOpenGLTexture.h, ofxhOpenGLRender.h). Backends: EGL on Linux and BSD (libEGL is loaded at
# run time: no build dependency), CGL on macOS, WGL on Windows (opengl32, gdi32 and user32, system
# libraries).
if(OFX_HOSTSUPPORT_OPENGL)
	if(NOT OFX_SUPPORTS_OPENGLRENDER)
		message(FATAL_ERROR "OFX_HOSTSUPPORT_OPENGL needs OFX_SUPPORTS_OPENGLRENDER")
	endif()
	find_package(Threads REQUIRED)
	file(GLOB OFX_HOSTSUPPORT_OPENGL_FILES "${OFX_HOSTSUPPORT_LIBRARY_DIR}/opengl/*.cpp" "${OFX_HOSTSUPPORT_LIBRARY_DIR}/opengl/*.h")
	add_library(OfxHostOpenGL STATIC ${OFX_HOSTSUPPORT_OPENGL_FILES})
	set_target_properties(OfxHostOpenGL PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		EXPORT_NAME HostSupportOpenGL)
	target_link_libraries(OfxHostOpenGL PUBLIC OfxHost Threads::Threads)
	if(APPLE)
		target_link_libraries(OfxHostOpenGL PRIVATE "-framework OpenGL")
	elseif(WIN32)
		target_link_libraries(OfxHostOpenGL PRIVATE opengl32 gdi32 user32)
	elseif(UNIX)
		target_link_libraries(OfxHostOpenGL PRIVATE ${CMAKE_DL_LIBS})
	endif()
	add_library(openfx::HostSupportOpenGL ALIAS OfxHostOpenGL)
endif()

# openfx::HostSupportOpenCL: OpenCL devices, command queues and renders (ofxhOpenCLDevice.h,
# ofxhOpenCLRender.h). OpenCL (the ICD loader, or the macOS framework) is loaded at run time: no
# build dependency.
if(OFX_HOSTSUPPORT_OPENCL)
	file(GLOB OFX_HOSTSUPPORT_OPENCL_FILES "${OFX_HOSTSUPPORT_LIBRARY_DIR}/opencl/*.cpp" "${OFX_HOSTSUPPORT_LIBRARY_DIR}/opencl/*.h")
	add_library(OfxHostOpenCL STATIC ${OFX_HOSTSUPPORT_OPENCL_FILES})
	set_target_properties(OfxHostOpenCL PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		EXPORT_NAME HostSupportOpenCL)
	target_link_libraries(OfxHostOpenCL PUBLIC OfxHost)
	if(UNIX)
		target_link_libraries(OfxHostOpenCL PRIVATE ${CMAKE_DL_LIBS})
	endif()
	add_library(openfx::HostSupportOpenCL ALIAS OfxHostOpenCL)
endif()

# openfx::HostSupportMetal: Metal devices, command queues and renders (ofxhMetalDevice.h,
# ofxhMetalRender.h), macOS only. Objective-C++ with ARC (the .mm files, compiled by the C++
# compiler); its headers are plain C++. Links the Metal and Foundation frameworks of the SDK.
if(OFX_HOSTSUPPORT_METAL)
	if(NOT APPLE)
		message(FATAL_ERROR "OFX_HOSTSUPPORT_METAL is for macOS only")
	endif()
	file(GLOB OFX_HOSTSUPPORT_METAL_FILES "${OFX_HOSTSUPPORT_LIBRARY_DIR}/metal/*.mm")
	add_library(OfxHostMetal STATIC ${OFX_HOSTSUPPORT_METAL_FILES})
	set_target_properties(OfxHostMetal PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		EXPORT_NAME HostSupportMetal)
	target_compile_options(OfxHostMetal PRIVATE -fobjc-arc)
	target_link_libraries(OfxHostMetal PUBLIC OfxHost "-framework Metal" "-framework Foundation")
	add_library(openfx::HostSupportMetal ALIAS OfxHostMetal)
endif()

if(BUILD_EXAMPLE_HOSTS)
	if(OFX_SUPPORTS_MULTITHREAD)
		# hostDemo and cacheDemo don't implement the multi-thread suite functions that
		# OFX_SUPPORTS_MULTITHREAD makes pure virtual in OFX::Host::ImageEffect::Host.
		message(STATUS "BUILD_EXAMPLE_HOSTS: skipped, the example hosts don't support OFX_SUPPORTS_MULTITHREAD")
	else()
		add_subdirectory(examples)
	endif()
endif()
