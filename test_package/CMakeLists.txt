cmake_minimum_required(VERSION 3.21...3.31)
project(PackageTest CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(openfx CONFIG REQUIRED)

# A tiny host using HostSupport
add_executable(test_package src/test_package.cpp)
target_link_libraries(test_package openfx::HostSupport)

# A plug-in using Support: the Invert example of the source tree
set(OFX_TEST_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Support/Plugins")
file(GLOB PLUGIN_SOURCES "${OFX_TEST_PLUGIN_DIR}/Invert/*.cpp")
add_ofx_plugin(invert_plugin "${OFX_TEST_PLUGIN_DIR}/Invert")
target_sources(invert_plugin PRIVATE ${PLUGIN_SOURCES})
target_include_directories(invert_plugin PRIVATE "${OFX_TEST_PLUGIN_DIR}/include")
target_link_libraries(invert_plugin openfx::Support)

# Without conan: ctest runs the host on the bundle built above
enable_testing()
add_test(NAME test_package COMMAND test_package)
set_tests_properties(test_package PROPERTIES ENVIRONMENT "OFX_PLUGIN_PATH=${OFX_PLUGIN_BUILD_DIR}")
