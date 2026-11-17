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

# The library compiles as C++98; consumers (Natron, plug-ins) use C++11 or later.
target_compile_features(OfxHost PUBLIC cxx_std_11)
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

if(OFX_INSTALL AND NOT OFX_USE_SYSTEM_EXPAT)
	# headers of the bundled expat, for ofxhPluginCache.h (not installed by a project that uses
	# add_subdirectory(openfx): OFX_INSTALL is OFF there)
	install(FILES expat-2.10.0/lib/expat.h expat-2.10.0/lib/expat_external.h
	        DESTINATION include)
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
