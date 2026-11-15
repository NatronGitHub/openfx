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

install(TARGETS hostDemo cacheDemo propsetDump RUNTIME DESTINATION bin)
