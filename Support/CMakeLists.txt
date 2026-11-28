set(OFX_SUPPORT_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
file(GLOB OFX_SUPPORT_HEADER_FILES "${OFX_SUPPORT_HEADER_DIR}/*.h")

add_subdirectory(Library)
if(BUILD_EXAMPLE_PLUGINS)
  add_subdirectory(Plugins)
  add_subdirectory(PropTester)
  # self-test of the OpenGL DrawSuite core (include/ofxDrawSuiteOpenGL.h) and of HostSupport's
  # helper (HostSupport/include/ofxhDrawSuiteOpenGL.h, header-only) with a recording fake OpenGL
  # table: no OpenGL header or library, built and run on every platform
  # (.github/scripts/cmake-build-test.sh runs it)
  add_executable(drawsuite-opengl-test tests/DrawSuiteOpenGLTest.cpp)
  target_include_directories(drawsuite-opengl-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../HostSupport/include")
  # self-test of Support code that needs no host (the list is at the top of
  # tests/SupportLibraryTest.cpp). Exits with 0 if all passed (.github/scripts/cmake-build-test.sh
  # runs it).
  # OfxSupport is built without OFX_USE_MULTITHREAD_MUTEX: the test compiles its own
  # Library/ofxsMultiThread.cpp with it, which the linker takes instead of OfxSupport's (a static
  # library), to test OFX::MultiThread::Mutex without the host's mutex functions.
  add_executable(support-library-test tests/SupportLibraryTest.cpp Library/ofxsMultiThread.cpp)
  target_compile_definitions(support-library-test PRIVATE OFX_USE_MULTITHREAD_MUTEX)
  target_include_directories(support-library-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Library")
  find_package(Threads REQUIRED) # (the Mutex test runs a second thread)
  target_link_libraries(support-library-test PRIVATE OfxSupport Threads::Threads)
  # plugin-unload-test BINARY...: each plug-in binary is really unloaded by dlclose(), and on
  # macOS has no thread-local variables (Support/tests/PluginUnloadTest.cpp, #385).
  # .github/scripts/cmake-build-test.sh runs it on the example plug-ins.
  if(NOT WIN32)
    add_executable(plugin-unload-test tests/PluginUnloadTest.cpp)
    target_include_directories(plugin-unload-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    target_link_libraries(plugin-unload-test PRIVATE ${CMAKE_DL_LIBS})
    # thread-scope-test.ofx (tests/ThreadScopeBundle.cpp): a binary that uses ThreadScope, built
    # and exported as add_ofx_plugin does (hidden visibility, the OFX entry points only), but
    # not a bundle and not installed: cmake-build-test.sh gives it to plugin-unload-test twice
    add_library(thread-scope-test MODULE tests/ThreadScopeBundle.cpp)
    set_target_properties(thread-scope-test PROPERTIES PREFIX "" SUFFIX ".ofx"
                          CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(thread-scope-test PRIVATE "${OFX_SUPPORT_HEADER_DIR}")
    target_link_libraries(thread-scope-test PRIVATE openfx::Api)
    target_compile_features(thread-scope-test PRIVATE cxx_std_17) # (as OfxSupport)
    if(APPLE)
      target_link_options(thread-scope-test PRIVATE "LINKER:-exported_symbols_list,${OFX_SUPPORT_HEADER_DIR}/osxSymbols")
    else()
      target_link_options(thread-scope-test PRIVATE "LINKER:--version-script=${OFX_SUPPORT_HEADER_DIR}/linuxSymbols")
    endif()
    # the same with default visibility and no export list: ThreadScope's statics must still stay
    # in the binary (not STB_GNU_UNIQUE with GCC on Linux, which would pin it in memory)
    add_library(thread-scope-test-default MODULE tests/ThreadScopeBundle.cpp)
    set_target_properties(thread-scope-test-default PROPERTIES PREFIX "" SUFFIX ".ofx")
    target_include_directories(thread-scope-test-default PRIVATE "${OFX_SUPPORT_HEADER_DIR}")
    target_link_libraries(thread-scope-test-default PRIVATE openfx::Api)
    target_compile_features(thread-scope-test-default PRIVATE cxx_std_17)
  endif()
  # self-test of DrawArgs::getDrawContext() and its OpenGL backend: on macOS in an offscreen CGL
  # context (it links OpenGL for CGL); elsewhere without a context, and without linking OpenGL,
  # to test the run-time loading (not on Windows). It prints the failed checks and exits with 0
  # if all passed (.github/scripts/cmake-build-test.sh runs it).
  if(NOT WIN32)
    add_executable(drawcontext-opengl-test tests/DrawContextOpenGLTest.cpp)
    target_include_directories(drawcontext-opengl-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Library")
    target_link_libraries(drawcontext-opengl-test PRIVATE OfxSupport)
    if(APPLE)
      target_link_libraries(drawcontext-opengl-test PRIVATE OpenGL::GL)
    endif()
    if(TARGET OpenGL::GL AND OFX_SUPPORT_DRAWCONTEXT_OPENGL)
      # OpenGL is installed: the backend must load it
      target_compile_definitions(drawcontext-opengl-test PRIVATE OFXS_DRAWCONTEXT_TEST_EXPECT_OPENGL)
    endif()
  endif()
endif()
