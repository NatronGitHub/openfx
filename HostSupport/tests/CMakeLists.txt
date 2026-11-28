# The check programs of HostSupport (HostSupport/examples/Makefile also builds suiteHandleChecks):
# the functional checks in functional/, the regression checks in regression/, and the
# harness they share here (suiteHandleChecks.h, gpuRenderChecks.h). They run on hostDemo's host
# classes, compiled from HostSupport/examples.
set(OFX_HOST_DEMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../examples")
set(OFX_HOST_DEMO_SOURCES
	"${OFX_HOST_DEMO_DIR}/hostDemoClipInstance.cpp"
	"${OFX_HOST_DEMO_DIR}/hostDemoEffectInstance.cpp"
	"${OFX_HOST_DEMO_DIR}/hostDemoHostDescriptor.cpp"
	"${OFX_HOST_DEMO_DIR}/hostDemoParamInstance.cpp")

# a check program: its sources, with hostDemo's host classes and the shared harness
function(ofx_host_check name)
	add_executable(${name} ${ARGN} ${OFX_HOST_DEMO_SOURCES})
	target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${OFX_HOST_DEMO_DIR}")
endfunction()

# unit checks of HostSupport on hostDemo's host classes (run by .github/scripts/hostdemo-checks.sh):
# the functional checks and the regression checks, in one program
ofx_host_check(suiteHandleChecks functional/suiteHandleChecks.cpp regression/suiteHandleRegressions.cpp)
target_link_libraries(suiteHandleChecks PRIVATE OfxHost)

# checks of openfx::HostSupportOpenGL, with Examples/OpenGL (skipped without an OpenGL context,
# unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_OPENGL)
	ofx_host_check(openglRenderChecks functional/openglRenderChecks.cpp)
	target_link_libraries(openglRenderChecks PRIVATE OfxHostOpenGL)
endif()

# checks of openfx::HostSupportOpenCL, with Support/Plugins/GPUGain built with
# OFX_SUPPORTS_OPENCLRENDER (skipped without an OpenCL device or the plug-in, unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_OPENCL)
	ofx_host_check(openclRenderChecks functional/openclRenderChecks.cpp)
	target_link_libraries(openclRenderChecks PRIVATE OfxHostOpenCL)
endif()

# checks of openfx::HostSupportMetal, with Support/Plugins/GPUGain built with Metal
# (OFX_EXAMPLE_GPUGAIN_METAL; skipped without a Metal device or the plug-in, unless OFXH_REQUIRE=1)
if(OFX_HOSTSUPPORT_METAL)
	ofx_host_check(metalRenderChecks functional/metalRenderChecks.cpp functional/metalRenderChecksKernel.mm)
	target_link_libraries(metalRenderChecks PRIVATE OfxHostMetal)
	set_source_files_properties(functional/metalRenderChecksKernel.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()

# checks of openfx::HostSupportCUDA: on a fake CUDA driver (always), with Support/Plugins/GPUGain
# from OFX_PLUGIN_PATH; and on a real CUDA device, if any (skipped without one, unless
# OFXH_REQUIRE_CUDA=1)
if(OFX_HOSTSUPPORT_CUDA)
	ofx_host_check(cudaRenderChecks functional/cudaRenderChecks.cpp)
	target_link_libraries(cudaRenderChecks PRIVATE OfxHostCUDA)
endif()

if(OFX_INSTALL) # (OFF under add_subdirectory(openfx))
	install(TARGETS suiteHandleChecks RUNTIME DESTINATION bin)
endif()
