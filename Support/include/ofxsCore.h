#ifndef _ofxsCore_H_
#define _ofxsCore_H_
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


/** @mainpage OFX Support Library

@section mainpageIntro Introduction

This support library skins the raw OFX C API with a set of C++ classes and functions that makes it easier to understand and write plug-ins to the API. Look at the examples to see how it is done.

<HR>

@section fifteenLineGuide Fifteen Line Plugin Writing Guide

- work from the examples
- you need to write the following functions....
- void OFX::Plugin::getPluginID(OFX::PluginID &id)
- gives the unique name and version numbers of the plug-in
- void OFX::Plugin::loadAction(void)
- called after the plug-in is first loaded, and before any instance has been made,
- void OFX::Plugin::unloadAction(void)
- called before the plug-in is unloaded, and all instances have been destroyed,
- void OFX::Plugin::describe(OFX::ImageEffectDescriptor &desc) 
- called to describe the plugin to the host
- void OFX::Plugin::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context) 
- called to describe the plugin to the host for a context reported in OFX::Plugin::describe
-  OFX::ImageEffect * OFX::Plugin::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context)
- called when a new instance of a plug-in needs to be created. You need to derive a class from ImageEffect, new it and return it.

The OFX::ImageEffect class has a set of members you can override to do various things, like rendering an effect. Again, look at the examples.

<HR>

@section license Copyright and License

The library is copyright 2004-2005, The Open Effects Association Ltd, and was
written by Bruno Nicoletti (bruno@thefoundry.co.uk).

It has been released under the GNU Lesser General Public License, see the 
top of any source file for details.

*/

/** @file This file contains core code that wraps OFX 'objects' with C++ classes.

This file only holds code that is visible to a plugin implementation, and so hides much
of the direct OFX objects and any library side only functions.
*/

#ifdef _MSC_VER
#pragma warning( disable : 4290 )
#endif

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxPixels.h"
#ifdef OFX_SUPPORTS_DIALOG
#include "ofxDialog.h"
#endif

#include <assert.h>
#include <vector>
#include <list>
#include <string>
#include <map>
#include <exception>
#include <stdexcept>
#include <sstream>
#include <memory>
#if defined(_MSC_VER)
#include <float.h> // _isnan
#endif
#include <cmath> // isnan, std::isnan

#ifdef OFX_CLIENT_EXCEPTION_HEADER
#include OFX_CLIENT_EXCEPTION_HEADER
#endif

#if __cplusplus >= 201103L
#  define OFX_THROW(x) noexcept(false)
#  define OFX_THROW2(x,y) noexcept(false)
#  define OFX_THROW3(x,y,z) noexcept(false)
#  define OFX_THROW4(x,y,z,w) noexcept(false)
#else
#  define OFX_THROW(x) throw(x)
#  define OFX_THROW2(x,y) throw(x,y)
#  define OFX_THROW3(x,y,z) throw(x,y,z)
#  define OFX_THROW4(x,y,z,w) throw(x,y,z,w)
#endif

// Is noexcept supported?
// "noexcept" is only supported since the Visual Studio 2015, as stated here: https://msdn.microsoft.com/en-us/library/wfa0edys.aspx
#if defined(__clang__)
#if __has_feature(cxx_noexcept)
#define OFX_NOTHROW noexcept(true)
#else
#define OFX_NOTHROW throw()
#endif
#else
#if defined(__GXX_EXPERIMENTAL_CXX0X__) && __GNUC__ * 10 + __GNUC_MINOR__ >= 46 || \
    defined(_MSC_FULL_VER) && _MSC_FULL_VER >= 190023026
#define OFX_NOTHROW noexcept(true)
#else
#if defined(_NOEXCEPT)
#define OFX_NOTHROW _NOEXCEPT
#else
#define OFX_NOTHROW throw()
#endif
#endif
#endif

/** @brief Defines an integer 3D point

Should migrate this to the ofxCore.h in a v1.1
*/
struct Ofx3DPointI {
  int x, y, z;
};

/** @brief Defines a double precision 3D point

Should migrate this to the ofxCore.h in a v1.1
*/
struct Ofx3DPointD {
  double x, y, z;
};

/** @brief Nasty macro used to define empty protected copy ctors and assign ops */
#define mDeclareProtectedAssignAndCC(CLASS) \
  CLASS &operator=(const CLASS &) {assert(false); return *this;}	\
  CLASS(const CLASS &) {assert(false); } 

/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {
#if __cplusplus >= 201103L
  template <typename T>
  using auto_ptr = std::unique_ptr<T>;
#else
  using std::auto_ptr;
#endif
  
#if defined(_MSC_VER)
  inline bool IsInfinite(double x) { return _finite(x) == 0 && _isnan(x) == 0; }
  inline bool IsNaN     (double x) { return _isnan(x) != 0;                    }
#else
#  if __cplusplus >= 201103L || _GLIBCXX_USE_C99_MATH
  // These definitions are for the normal Unix suspects.
  inline bool IsInfinite(double x) { return (std::isinf)(x);    }
  inline bool IsNaN     (double x) { return (std::isnan)(x);    }
#  else
#    ifdef isnan // isnan is defined as a macro
  inline bool IsInfinite(double x) { return isinf(x);    }
  inline bool IsNaN     (double x) { return isnan(x);    }
#    else
  inline bool IsInfinite(double x) { return ::isinf(x);    }
  inline bool IsNaN     (double x) { return ::isnan(x);    }
#    endif
#  endif
#endif

  /** forward class declarations */
  class PropertySet;

  /** @brief Enumerates the different types a property can be */
  enum PropertyTypeEnum {
    ePointer,
    eInt,
    eString,
    eDouble
  };

  /** @brief Enumerates the reasons a plug-in instance may have had one of its values changed */
  enum InstanceChangeReason {
    eChangeUserEdit,    /**< @brief A user actively editted something in the plugin, eg: changed the value of an integer param on an interface */
    eChangePluginEdit,  /**< @brief The plugin's own code changed something in the instance, eg: a callback on on param setting the value of another */
    eChangeTime         /**< @brief The current value of a parameter has changed because the param animates and the current time has changed */
  };

  /** @brief maps a status to a string for debugging purposes, note a c-str for printf */
  const char * mapStatusToString(OfxStatus stat);

  /** @brief namespace for OFX support lib exceptions, all derive from std::exception, calling it */
  namespace Exception {

    /** @brief thrown when a suite returns a dud status code 
    */
    class Suite : public std::exception {
    protected :
      OfxStatus _status;
    public :
      Suite(OfxStatus s) : _status(s) {}
      OfxStatus status(void) const {return _status;}
      operator OfxStatus() const {return _status;}

      /** @brief reimplemented from std::exception */
      virtual const char * what () const OFX_NOTHROW {return mapStatusToString(_status);}

    };

    /** @brief Exception indicating that a host doesn't know about a property that is should do */
    class PropertyUnknownToHost : public std::exception {
    protected :
      std::string _what;
    public :
      PropertyUnknownToHost(const char *what) : _what(what) {}
      virtual ~PropertyUnknownToHost() OFX_NOTHROW {}

      /** @brief reimplemented from std::exception */
      virtual const char * what () const OFX_NOTHROW
      {
        return _what.c_str();
      }
    };

    /** @brief exception indicating that the host thinks a property has an illegal value */
    class PropertyValueIllegalToHost : public std::exception {
    protected :
      std::string _what;
    public :
      PropertyValueIllegalToHost(const char *what) : _what(what) {}
      virtual ~PropertyValueIllegalToHost() OFX_NOTHROW {}

      /** @brief reimplemented from std::exception */
      virtual const char * what () const OFX_NOTHROW
      {
        return _what.c_str();
      }
    };

    /** @brief exception indicating a request for a named thing exists (eg: a param), but is of the wrong type, should never make it back to the main entry
    indicates a logical error in the code. Asserts are raised in debug code in these situations.
    */
    class TypeRequest : public std::exception {
    protected :
      std::string _what;
    public :
      TypeRequest(const char *what) : _what(what) {}
      virtual ~TypeRequest() OFX_NOTHROW {}

      /** @brief reimplemented from std::exception */
      virtual const char * what () const OFX_NOTHROW
      {
        return _what.c_str();
      }
    };

    ////////////////////////////////////////////////////////////////////////////////
    // These exceptions are to be thrown by the plugin if it hits a problem, the
    // code managing the main entry will trap the exception and return a suitable 
    // status code to the host.

    /** @brief exception indicating a required host feature is missing */
    class HostInadequate : public std::exception {
    protected :
      std::string _what;
    public :
      HostInadequate(const char *what) : _what(what) {}
      virtual ~HostInadequate() OFX_NOTHROW {}

      /** @brief reimplemented from std::exception */
      virtual const char * what () const OFX_NOTHROW
      {
        return _what.c_str();
      }
    };

  }; // end of Exception namespace

  /** @brief Throws an @ref OFX::Exception::Suite depending on the status flag passed in */
  void 
    throwSuiteStatusException(OfxStatus stat) 
    OFX_THROW2(OFX::Exception::Suite, std::bad_alloc);

  void 
    throwHostMissingSuiteException(const std::string& name) 
    OFX_THROW(OFX::Exception::Suite);

  /** @brief This struct is used to return an identifier for the plugin by the function @ref OFX:Plugin::getPlugin. 
  The members correspond to those in the OfxPlugin struct defined in ofxCore.h.
  */

  class ImageEffectDescriptor;
  class ImageEffect;

  /** @brief This class wraps up an OFX property set */
  class PropertySet {
  protected :
    /** @brief The raw property handle */
    OfxPropertySetHandle _propHandle;

    /** @brief Class static, whether we are logging each property action */
    static int _gPropLogging;

    /** @brief Do not throw an exception if a host returns 'unsupported' when setting a property */
    static bool _gThrowOnUnsupported;

  public :
    /** @brief turns on logging of property access functions */
    static void propEnableLogging(void)  {++_gPropLogging;}

    /** @brief turns off logging of property access functions */
    static void propDisableLogging(void) {--_gPropLogging;}

    /** @brief Do we throw an exception if a host returns 'unsupported' when setting a property. Default is true */
    static void setThrowOnUnsupportedProperties(bool v) {_gThrowOnUnsupported = v;}

    /** @brief Do we throw an exception if a host returns 'unsupported' when setting a property. Default is true */
    static bool getThrowOnUnsupportedProperties(void) {return _gThrowOnUnsupported;}

    /** @brief construct a property set */
    PropertySet(OfxPropertySetHandle h = NULL) : _propHandle(h) {}
    virtual ~PropertySet();

    /** @brief copy constructor */
    PropertySet(const PropertySet& p) { _propHandle = p.propSetHandle(); }
    PropertySet& operator=(const PropertySet& p) { _propHandle = p.propSetHandle(); return *this; }

  public:
    /** @brief set the handle to use for this set */
    void propSetHandle(OfxPropertySetHandle h) { _propHandle = h;}

    /** @brief return the handle for this property set */
    OfxPropertySetHandle propSetHandle(void) const {return _propHandle;}

    bool  propExists(const char* property, bool throwOnFailure = true) const OFX_THROW3(std::bad_alloc,
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);
    int  propGetDimension(const char* property, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);
    void propReset(const char* property) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    // set single values
    void propSetPointer(const char* property, void *value, int idx, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);
    void propSetString(const char* property, const std::string &value, int idx, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);
    void propSetDouble(const char* property, double value, int idx, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);
    void propSetInt(const char* property, int value, int idx, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    // set multiple values
    void propSetStringN(const char* property, const std::vector<std::string> &values, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);
    
    void propSetDoubleN(const char* property, const std::vector<double> &values, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    void propSetDoubleN(const char* property, const double *values, int count, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    void propSetIntN(const char* property, const std::vector<int> &values, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    // values is before count to avoid an easy confusion with propSetInt, when the pointer to values would be cast to bool
    void propSetIntN(const char* property, const int *values, int count, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);


    void propSetPointer(const char* property, void *value, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {propSetPointer(property, value, 0, throwOnFailure);}

    void propSetString(const char* property, const std::string &value, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {propSetString(property, value, 0, throwOnFailure);}

    void propSetDouble(const char* property, double value, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {propSetDouble(property, value, 0, throwOnFailure);}

    void propSetInt(const char* property, int value, bool throwOnFailure = true) OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {propSetInt(property, value, 0, throwOnFailure);}


    /// get a pointer property
    void       *propGetPointer(const char* property, int idx, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    /// get a string property
    std::string propGetString(const char* property, int idx, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    /// get a double property
    double      propGetDouble(const char* property, int idx, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    /// get an int property
    int propGetInt(const char* property, int idx, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite);

    /// get a pointer property with index 0
    void* propGetPointer(const char* property, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {
      return propGetPointer(property, 0, throwOnFailure); 
    }

    /// get a string property with index 0
    std::string propGetString(const char* property, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {
      return propGetString(property, 0, throwOnFailure); 
    }

    /// get a double property with index 0
    double propGetDouble(const char* property, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {
      return propGetDouble(property, 0, throwOnFailure); 
    }

    /// get an int property with index 0
    int propGetInt(const char* property, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost, 
      OFX::Exception::PropertyValueIllegalToHost, 
      OFX::Exception::Suite)
    {
      return propGetInt(property, 0, throwOnFailure); 
    }
      
    void propGetStringN(const char* property, std::vector<std::string>* values, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    void propGetDoubleN(const char* property, std::vector<double>* values, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    // values is before count to avoid an easy confusion with propGetDouble, when the pointer to values would be cast to bool
    void propGetDoubleN(const char* property, double* values, int count, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    void propGetIntN(const char* property, std::vector<int>* values, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

    void propGetIntN(const char* property, int* values, int count, bool throwOnFailure = true) const OFX_THROW4(std::bad_alloc,
      OFX::Exception::PropertyUnknownToHost,
      OFX::Exception::PropertyValueIllegalToHost,
      OFX::Exception::Suite);

  };

  // forward decl of the image effect
  class ImageEffect;
};

// undeclare the protected assign and CC macro
#undef mDeclareProtectedAssignAndCC

#endif
