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
  # self-test of Support code that needs no host (Support/tests/SupportLibraryTest.cpp): the null
  # action of the interact entry point, ~AutoMutexT(), define*Param() on a type mismatch, and (with OFX_EXTENSIONS_NATRON) the decoding
  # of kNatronOfxPropNativeOverlays. Exits with 0 if all passed (.github/scripts/cmake-build-test.sh
  # runs it).
  add_executable(support-library-test tests/SupportLibraryTest.cpp)
  target_include_directories(support-library-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Library")
  target_link_libraries(support-library-test PRIVATE OfxSupport)
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
