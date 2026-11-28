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
if(OFX_HOSTSUPPORT_CUDA)
	# --gpu=cuda (openfx::HostSupportCUDA)
	target_link_libraries(hostDemo PRIVATE OfxHostCUDA)
	target_compile_definitions(hostDemo PRIVATE HOSTDEMO_CUDA)
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

# (the check programs that reuse these host classes, suiteHandleChecks and the GPU components'
# checks, are in HostSupport/tests)

if(OFX_INSTALL) # (OFF under add_subdirectory(openfx))
	install(TARGETS hostDemo cacheDemo propsetDump RUNTIME DESTINATION bin)
endif()
