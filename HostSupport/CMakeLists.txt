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

if(BUILD_EXAMPLE_HOSTS)
	if(OFX_SUPPORTS_MULTITHREAD)
		# hostDemo and cacheDemo don't implement the multi-thread suite functions that
		# OFX_SUPPORTS_MULTITHREAD makes pure virtual in OFX::Host::ImageEffect::Host.
		message(STATUS "BUILD_EXAMPLE_HOSTS: skipped, the example hosts don't support OFX_SUPPORTS_MULTITHREAD")
	else()
		add_subdirectory(examples)
	endif()
endif()
