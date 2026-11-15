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

install(TARGETS hostDemo cacheDemo RUNTIME DESTINATION bin)
