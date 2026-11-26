# Example hosts built on HostSupport (also built by HostSupport/examples/Makefile)
add_executable(hostDemo
	hostDemo.cpp
	hostDemoClipInstance.cpp
	hostDemoEffectInstance.cpp
	hostDemoHostDescriptor.cpp
	hostDemoParamInstance.cpp)
target_link_libraries(hostDemo PRIVATE OfxHost)

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

if(OFX_INSTALL) # (OFF under add_subdirectory(openfx))
	install(TARGETS hostDemo cacheDemo propsetDump suiteHandleChecks RUNTIME DESTINATION bin)
endif()
