#ifndef _ofxsCore_H_
#define _ofxsCore_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause


/** @mainpage OFX Support Library

@section mainpageIntro Introduction

This support library skins the raw OFX C API with a set of C++ classes and functions that makes it easier to understand and write plug-ins to the API. Look at the examples to see how it is done.

<HR>

@section fifteenLineGuide Plugin Writing Guide

- work from the examples (Support/Plugins, e.g. Invert/invert.cpp)
- derive a class from OFX::ImageEffect: its constructor fetches the clips and the parameters
  (fetchClip(), fetch*Param()), and it overrides the actions it handles (render(),
  isIdentity(), getRegionOfDefinition(), changedParam(), ...)
- declare the plug-in factory with mDeclarePluginFactory(MyPluginFactory, {}, {}) (the last two
  arguments are the bodies of load() and unload()), and define its member functions:
  - void MyPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
    describes the plug-in to the host (labels, contexts, pixel depths, ...)
  - void MyPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
    defines the clips and the parameters for a context reported in describe()
  - OFX::ImageEffect* MyPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context)
    called when a new instance of the plug-in is needed: new your OFX::ImageEffect subclass and
    return it
- register the factory, which gives the plug-in identifier and version:
  static MyPluginFactory p("com.example.myPlugin", 1, 0); mRegisterPluginFactoryInstance(p)
  (plug-ins only known at run time instead declare a static OFX::PluginFactories object with a
  function that appends their factories, called at the first OfxGetNumberOfPlugins: see
  OFX::PluginFactories)

The OFX::ImageEffect class has a set of members you can override to do various things, like rendering an effect. Again, look at the examples.

Parameter changes that belong together, e.g. several setValue() calls in changedParam() or in an
interact, go in one undo step with an edit block, which ends when it goes out of scope:
@code
OFX::ParamSet::EditBlock eb(*this, "Reset center");
@endcode
To begin it only if a parameter actually changes, pass OFX::ParamSet::EditBlock::eBeginOnChange
and call eb.open() before each change; eHostErrorsIgnore makes the changes without an edit block
if the host refuses one. The destructor never throws (see OFX::ParamSet::EditBlock).

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

#if defined(OFX_EXTENSIONS_NATRON) && !defined(OFX_EXTENSIONS_NUKE)
#error "OFX_EXTENSIONS_NATRON needs OFX_EXTENSIONS_NUKE: the Natron extensions use the Nuke ones (views, planes, transforms)"
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
#include <float.h> // for the plug-in code that relies on it
#endif
#include <cmath> // for the plug-in code that relies on it
#include <cstring> // memcpy (IsNaN, IsInfinite)

#ifdef OFX_CLIENT_EXCEPTION_HEADER
#include OFX_CLIENT_EXCEPTION_HEADER
#endif

/** @brief Exception specification of the Support functions that never throw (what() and the
    destructors of the Support exceptions, OFX::Memory::free); plug-ins may use it too. */
#define OFX_NOTHROW noexcept

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

/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {
  /** @brief std::unique_ptr, under the name the plug-ins use (std::auto_ptr was removed in C++17) */
  template <typename T>
  using auto_ptr = std::unique_ptr<T>;
  
  // IsNaN and IsInfinite test the bits of the IEEE 754 binary64 representation instead of calling
  // std::isnan/std::isinf (or _isnan/_finite): with -ffast-math or -Ofast (-ffinite-math-only), or
  // /fp:fast, the compiler assumes that no floating-point value is NaN or infinite, and folds
  // std::isnan(x), std::isinf(x) and x != x to false, so that the checks disappear. These functions
  // check values that cross the API, i.e. that come from another binary (a host or a plug-in),
  // which may have been built with other options, so they must work whatever the options this
  // file is compiled with. The bits are integers, but clang also deduces facts about them from the
  // double they come from (e.g. a fast-math function argument is marked as never infinite), so an
  // empty asm statement (gcc, clang, clang-cl), or volatile copies (MSVC), hide their origin from
  // the optimizer. The bits are read as two 32-bit words, which needs no 64-bit registers (32-bit
  // x86). The results are those of std::isnan and std::isinf (a float argument is converted to
  // double, which keeps NaN and infinity).
  inline void DoubleWords(double x, unsigned int& hi, unsigned int& lo)
  {
    unsigned int w[2];
    std::memcpy(w, &x, sizeof(w));
    const unsigned int one = 1;
    unsigned char firstByte;
    std::memcpy(&firstByte, &one, 1);
    const int h = (firstByte == 1) ? 1 : 0; // the index of the high word (1 if little-endian)
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(w[0]), "+r"(w[1])); // an optimization barrier: the compiler can't know the words
#else
    volatile unsigned int v0 = w[0];
    volatile unsigned int v1 = w[1];
    w[0] = v0;
    w[1] = v1;
#endif
    hi = w[h] & 0x7fffffffU; // without the sign bit
    lo = w[1 - h];
  }
  inline bool IsNaN(double x)
  {
    unsigned int hi, lo;
    DoubleWords(x, hi, lo);
    return hi > 0x7ff00000U || (hi == 0x7ff00000U && lo != 0); // exponent all ones, mantissa != 0
  }
  inline bool IsInfinite(double x)
  {
    unsigned int hi, lo;
    DoubleWords(x, hi, lo);
    return hi == 0x7ff00000U && lo == 0; // exponent all ones, mantissa == 0
  }

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
    eChangePluginEdit,  /**< @brief The plugin's own code changed something in the instance, eg: a callback on on param settting the value of another */
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

  /** @brief Throws an @ref OFX::Exception::Suite depending on the status flag passed in
      (std::bad_alloc for kOfxStatErrMemory), does nothing on success */
  void 
    throwSuiteStatusException(OfxStatus stat);

  /** @brief Throws an @ref OFX::Exception::Suite (kOfxStatErrUnsupported): the host lacks the named suite function */
  void 
    throwHostMissingSuiteException(const std::string& name);

  /** @brief This struct is used to return an identifier for the plugin by the function @ref OFX:Plugin::getPlugin. 
  The members correspond to those in the OfxPlugin struct defined in ofxCore.h.
  */

  class ImageEffectDescriptor;
  class ImageEffect;

  /** @brief This class wraps up an OFX property set

  The prop* functions throw std::bad_alloc, OFX::Exception::PropertyUnknownToHost,
  OFX::Exception::PropertyValueIllegalToHost or OFX::Exception::Suite when the property suite
  fails. With throwOnFailure false, a failure of the host is not thrown, and the
  getters return 0, NULL or an empty value; the getters that fill the caller's array
  (propGetDoubleN(property, values, count) and propGetIntN(property, values, count)) leave it
  unchanged, or as the host wrote it. setThrowOnUnsupportedProperties(false) also suppresses
  PropertyUnknownToHost (a property the host doesn't know) when throwOnFailure is true. */
  class PropertySet {
  protected :
    /** @brief The raw property handle */
    OfxPropertySetHandle _propHandle;

    /** @brief Class static, whether we are logging each property action. Per thread:
        the property validation turns logging off on its thread while render threads use
        properties. */
    static thread_local int _gPropLogging;

    /** @brief Do not throw an exception if a host returns 'unsupported' when setting a property.
        One flag per plug-in binary, not synchronized: set it in the load action or in describe,
        before the host may call the plug-ins of the binary from several threads (CP.2). */
    static bool _gThrowOnUnsupported;

  public :
    /** @brief turns on logging of property access functions (on the calling thread) */
    static void propEnableLogging(void)  {++_gPropLogging;}

    /** @brief turns off logging of property access functions (on the calling thread) */
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

    bool  propExists(const char* property, bool throwOnFailure = true) const;
    int  propGetDimension(const char* property, bool throwOnFailure = true) const;
    void propReset(const char* property);

    // set single values
    void propSetPointer(const char* property, void *value, int idx, bool throwOnFailure = true);
    void propSetString(const char* property, const std::string &value, int idx, bool throwOnFailure = true);
    void propSetDouble(const char* property, double value, int idx, bool throwOnFailure = true);
    void propSetInt(const char* property, int value, int idx, bool throwOnFailure = true);

    // set multiple values
    void propSetStringN(const char* property, const std::vector<std::string> &values, bool throwOnFailure = true);
    
    void propSetDoubleN(const char* property, const std::vector<double> &values, bool throwOnFailure = true);

    void propSetDoubleN(const char* property, const double *values, int count, bool throwOnFailure = true);

    void propSetIntN(const char* property, const std::vector<int> &values, bool throwOnFailure = true);

    // values is before count to avoid an easy confusion with propSetInt, whet the pointer to values would be cast to bool
    void propSetIntN(const char* property, const int *values, int count, bool throwOnFailure = true);


    void propSetPointer(const char* property, void *value, bool throwOnFailure = true)
    {propSetPointer(property, value, 0, throwOnFailure);}

    void propSetString(const char* property, const std::string &value, bool throwOnFailure = true)
    {propSetString(property, value, 0, throwOnFailure);}

    void propSetDouble(const char* property, double value, bool throwOnFailure = true)
    {propSetDouble(property, value, 0, throwOnFailure);}

    void propSetInt(const char* property, int value, bool throwOnFailure = true)
    {propSetInt(property, value, 0, throwOnFailure);}


    /// get a pointer property
    void       *propGetPointer(const char* property, int idx, bool throwOnFailure = true) const;

    /// get a string property
    std::string propGetString(const char* property, int idx, bool throwOnFailure = true) const;

    /// get a double property
    double      propGetDouble(const char* property, int idx, bool throwOnFailure = true) const;

    /// get an int property
    int propGetInt(const char* property, int idx, bool throwOnFailure = true) const;

    /// get a pointer property with index 0
    void* propGetPointer(const char* property, bool throwOnFailure = true) const
    {
      return propGetPointer(property, 0, throwOnFailure); 
    }

    /// get a string property with index 0
    std::string propGetString(const char* property, bool throwOnFailure = true) const
    {
      return propGetString(property, 0, throwOnFailure); 
    }

    /// get a double property with index 0
    double propGetDouble(const char* property, bool throwOnFailure = true) const
    {
      return propGetDouble(property, 0, throwOnFailure); 
    }

    /// get an int property with index 0
    int propGetInt(const char* property, bool throwOnFailure = true) const
    {
      return propGetInt(property, 0, throwOnFailure); 
    }
      
    void propGetStringN(const char* property, std::vector<std::string>* values, bool throwOnFailure = true) const;

    /// get all the values of a string property, as a list (upstream OpenFX's API: same as propGetStringN)
    std::list<std::string> propGetNString(const char* property, bool throwOnFailure = true) const
    {
      std::vector<std::string> values;
      propGetStringN(property, &values, throwOnFailure);
      return std::list<std::string>(values.begin(), values.end());
    }

    void propGetDoubleN(const char* property, std::vector<double>* values, bool throwOnFailure = true) const;

    // values is before count to avoid an easy confusion with propGetDouble, whet the pointer to values would be cast to bool
    void propGetDoubleN(const char* property, double* values, int count, bool throwOnFailure = true) const;

    void propGetIntN(const char* property, std::vector<int>* values, bool throwOnFailure = true) const;

    void propGetIntN(const char* property, int* values, int count, bool throwOnFailure = true) const;

  };

  // forward decl of the image effect
  class ImageEffect;
};


#endif
