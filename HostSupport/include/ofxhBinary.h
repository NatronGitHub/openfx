#ifndef OFX_BINARY_H
#define OFX_BINARY_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause
#include <string>
#include <iostream>

#include "ofxhPlatformDefines.h"

#if defined(UNIX)
#include <dlfcn.h>
#elif defined (WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX // no min/max macros: they break std::min, std::max and std::numeric_limits<T>::max()
#endif
#include "windows.h"
#include <assert.h>
#endif

#include <sys/stat.h>

namespace OFX 
{

  /// class representing a DLL/Shared Object/etc
  class Binary {
    /// destruction will close the library and invalidate
    /// any function pointers returned by lookupSymbol()
  protected :
    std::string _binaryPath;
    bool _invalid;
#if defined(UNIX)
    void *_dlHandle;
#elif defined (WINDOWS)
    HINSTANCE _dlHandle;
#endif
    time_t _time;
    off_t _size;
    int _users;

  private :
    // not copyable: the destructor unloads the library, so that a copy would unload it twice
    // (C.21)
    Binary(const Binary &) = delete;
    Binary &operator=(const Binary &) = delete;

  public :

    /// create object representing the binary.  will stat() it, 
    /// and this fails, will set binary to be invalid.
    explicit Binary(const std::string &binaryPath);

    ~Binary() { unload(); }

    // calls stat, returns true if successfull, false otherwise
    static bool getFileModTimeAndSize(const std::string &binaryPath, time_t& modificationTime, off_t& fileSize);

    bool isLoaded() const { return _dlHandle != 0; }

    /// is this binary invalid? (did the a stat() or load() on the file fail,
    /// or are we missing a some of the symbols?
    bool isInvalid() const { return _invalid; }

    /// set invalid status (e.g. called by user if a mandatory symbol was missing)
    void setInvalid(bool invalid) { _invalid = invalid; }

    /// Last modification time of the file.
    time_t getTime() const { return _time; }

    /// Current size of the file.
    off_t getSize() const { return _size; }

    /// Path to the file.
    const std::string &getBinaryPath() const { return _binaryPath; }

    void ref();
    void unref();

    /// open the binary.
    void load();

    /// close the binary
    void unload();

    /// look up a symbol in the binary file and return it as a pointer.
    /// returns null pointer if not found, or if the library is not loaded.
    void *findSymbol(const std::string &symbol);
  };
}

#endif
