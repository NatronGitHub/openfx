# Example hosts built on HostSupport (also built by HostSupport/examples/Makefile)
add_executable(hostDemo
	hostDemo.cpp
	hostDemoClipInstance.cpp
	hostDemoEffectInstance.cpp
	hostDemoHostDescriptor.cpp
	hostDemoParamInstance.cpp)
target_link_libraries(hostDemo PRIVATE OfxHost)
if(OFX_HOSTSUPPORT_OPENGL)
	# --gpu=opengl (openfx::HostSupportOpenGL)
	target_link_libraries(hostDemo PRIVATE OfxHostOpenGL)
	target_compile_definitions(hostDemo PRIVATE HOSTDEMO_OPENGL)
endif()
if(OFX_HOSTSUPPORT_OPENCL)
	# --gpu=opencl (openfx::HostSupportOpenCL)
	target_link_libraries(hostDemo PRIVATE OfxHostOpenCL)
	target_compile_definitions(hostDemo PRIVATE HOSTDEMO_OPENCL)
endif()
if(OFX_HOSTSUPPORT_METAL)
	# --gpu=metal (openfx::HostSupportMetal)
	target_link_libraries(hostDemo PRIVATE OfxHostMetal)
	target_compile_definitions(hostDemo PRIVATE HOSTDEMO_METAL)
endif()

add_executable(cacheDemo cacheDemo.cpp)
target_link_libraries(cacheDemo PRIVATE OfxHost)

# dumps the property sets HostSupport creates, for scripts/audit-propsets.py
add_executable(propsetDump
	propsetDump.cpp
	hostDemoClipInstance.cpp
	hostDemoEffectInstance.cpp
	hostDemoHostDescriptor.cpp
	hostDemoParamInstance.cpp)
target_link_libraries(propsetDump PRIVATE OfxHost)

# unit checks of HostSupport on hostDemo's host classes (run by .github/scripts/hostdemo-checks.sh)
add_executable(suiteHandleChecks
	suiteHandleChecks.cpp
	hostDemoClipInstance.cpp
	hostDemoEffectInstance.cpp
	hostDemoHostDescriptor.cpp
	hostDemoParamInstance.cpp)
target_link_libraries(suiteHandleChecks PRIVATE OfxHost)

# checks of openfx::HostSupportOpenGL, with Examples/OpenGL (skipped without an OpenGL context,
# unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_OPENGL)
	add_executable(openglRenderChecks
		openglRenderChecks.cpp
		hostDemoClipInstance.cpp
		hostDemoEffectInstance.cpp
		hostDemoHostDescriptor.cpp
		hostDemoParamInstance.cpp)
	target_link_libraries(openglRenderChecks PRIVATE OfxHostOpenGL)
endif()

# checks of openfx::HostSupportOpenCL, with Support/Plugins/GPUGain built with
# OFX_SUPPORTS_OPENCLRENDER (skipped without an OpenCL device or the plug-in, unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_OPENCL)
	add_executable(openclRenderChecks
		openclRenderChecks.cpp
		hostDemoClipInstance.cpp
		hostDemoEffectInstance.cpp
		hostDemoHostDescriptor.cpp
		hostDemoParamInstance.cpp)
	target_link_libraries(openclRenderChecks PRIVATE OfxHostOpenCL)
endif()

# checks of openfx::HostSupportMetal, with Support/Plugins/GPUGain built with Metal
# (OFX_EXAMPLE_GPUGAIN_METAL; skipped without a Metal device or the plug-in, unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_METAL)
	add_executable(metalRenderChecks
		metalRenderChecks.cpp
		metalRenderChecksKernel.mm
		hostDemoClipInstance.cpp
		hostDemoEffectInstance.cpp
		hostDemoHostDescriptor.cpp
		hostDemoParamInstance.cpp)
	target_link_libraries(metalRenderChecks PRIVATE OfxHostMetal)
	set_source_files_properties(metalRenderChecksKernel.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()

if(OFX_INSTALL) # (OFF under add_subdirectory(openfx))
	install(TARGETS hostDemo cacheDemo propsetDump suiteHandleChecks RUNTIME DESTINATION bin)
endif()
