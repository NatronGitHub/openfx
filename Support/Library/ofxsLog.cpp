/*
OFX Support Library, a library that skins the OFX plug-in API with C++ classes.
Copyright (C) 2004-2005 The Open Effects Association Ltd
Author Bruno Nicoletti bruno@thefoundry.co.uk

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
* Neither the name The Open Effects Association Ltd, nor the names of its 
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The Open Effects Association Ltd
1 Wardour St
London W1D 6PA
England


*/

/** @file This file contains the body of functions used for logging ofx problems etc...

The log file is written to using printf style functions, rather than via c++ iostreams.

*/

#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <string>

#ifdef DEBUG
#include <iostream>
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <direct.h>
#define getcwd _getcwd // stupid MSFT "deprecation" warning
#else
#include <unistd.h>
#endif
#endif

#include "ofxsLog.h"

#if (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L)) // MSVC: __cplusplus is 199711L without /Zc:__cplusplus
#  define OFXS_LOG_CXX11 1
#  ifdef DEBUG
#    include <mutex>
#  endif
#endif

namespace OFX {
  namespace Log {

    /** @brief log file */
    static FILE *gLogFP = 0;

    /// environment variable for the log file
#define kLogFileEnvVar "OFX_PLUGIN_LOGFILE"

    /** @brief the global logfile name */
    /// (created on first use and never destroyed, so that it is valid before static initialization
    /// and after static destruction)
    static std::string &logFileName()
    {
      static std::string *name = new std::string(getenv(kLogFileEnvVar) ? getenv(kLogFileEnvVar) : "ofxTestLog.txt");
      return *name;
    }

    /** @brief indent level. Per thread with C++11, since plug-ins log from render threads too and
        the indent follows each thread's nesting of actions; a single global before C++11. */
#ifdef OFXS_LOG_CXX11
    static thread_local int gIndent = 0;
#else
    static int gIndent = 0;
#endif

#if defined(OFXS_LOG_CXX11) && defined(DEBUG)
    /** @brief serializes opening the log file (on the first log from any thread), writing each log
        line (so that lines from different threads don't interleave), closing it and setting its
        name. Recursive: the writers call open(). Only in DEBUG builds, the only ones that open a
        log file. Created on first use and never destroyed, so that logging before static
        initialization or after static destruction (e.g. an action during exit) still works. */
    static std::recursive_mutex &logMutex()
    {
      static std::recursive_mutex *m = new std::recursive_mutex;
      return *m;
    }
#  define OFXS_LOG_LOCK() std::lock_guard<std::recursive_mutex> ofxsLogLock(logMutex())
#else
#  define OFXS_LOG_LOCK() ((void)0)
#endif

    /** @brief Sets the name of the log file. */
    void setFileName(const std::string &value)
    {
      OFXS_LOG_LOCK();
      logFileName() = value;
    }

    /** @brief Opens the log file, returns whether this was sucessful or not. */
    bool open(void)
    {
#ifdef DEBUG
      OFXS_LOG_LOCK();
      if(!gLogFP) {
        char buffer[2048];
        char *answer = getcwd(buffer, sizeof(buffer));
        std::cout << "INFO: OFX Log is \"" << logFileName() << "\", working directory is \"" << answer << '\"' << std::endl;
        gLogFP = fopen(logFileName().c_str(), "w");
        if (!gLogFP) {
          std::cout << "INFO: Failed to open OFX Log for writing" << std::endl;
        }
        return gLogFP != 0;
      }
#endif
      return gLogFP != 0;
    }

    /** @brief Closes the log file. */
    void close(void)
    {
      OFXS_LOG_LOCK();
      if(gLogFP) {
        fclose(gLogFP);
      }
      gLogFP = 0;
    }

    /** @brief Indent the log of the calling thread */
    void indent(void)
    {
      ++gIndent;
    }

    /** @brief Outdent the log of the calling thread */
    void outdent(void)
    {
      --gIndent;
    }

    /** @brief do the indenting */
    static void doIndent(void)
    {
      if(open()) {
        for(int i = 0; i < gIndent; i++) {
          fputs("    ", gLogFP);
        }
      }
    }

    /** @brief Prints to the log file. */
    void print(const char *format, ...)
    {
      OFXS_LOG_LOCK();
      if(open()) {
        doIndent();
        va_list args;
        va_start(args, format);
        vfprintf(gLogFP, format, args);
        fputc('\n', gLogFP);
        fflush(gLogFP);
        va_end(args);
      }  
    }

    /** @brief Prints to the log file only if the condition is true and prepends a warning notice. */
    void warning(bool condition, const char *format, ...)
    {
      if (!condition) {
        return;
      }
      OFXS_LOG_LOCK();
      if(open()) {
        doIndent();
        fputs("WARNING : ", gLogFP);

        va_list args;
        va_start(args, format);
        vfprintf(gLogFP, format, args);
        fputc('\n', gLogFP);
        va_end(args);

        fflush(gLogFP);
      }  
    }

    /** @brief Prints to the log file only if the condition is true and prepends an error notice. */
    void error(bool condition, const char *format, ...)
    {
      if (!condition) {
        return;
      }
      OFXS_LOG_LOCK();
      if(open()) {
        doIndent();
        fputs("ERROR : ", gLogFP);

        va_list args;
        va_start(args, format);
        vfprintf(gLogFP, format, args);
        fputc('\n', gLogFP);
        va_end(args);

        fflush(gLogFP);
      }  
    }
  }
}
