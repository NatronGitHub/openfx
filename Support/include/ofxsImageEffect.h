#ifndef _ofxsImageEffect_H_
#define _ofxsImageEffect_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file This file contains core code that wraps OFX 'objects' with C++ classes.

This file only holds code that is visible to a plugin implementation, and so hides much
of the direct OFX objects and any library side only functions.
*/
#include <cstddef> // std::ptrdiff_t
#include <map>
#include <string>
#include <sstream> // stringstream
#include <locale> // std::locale::classic
#include <memory>
#ifndef NDEBUG
#include <stdio.h> // printf in debug
#endif
#include "ofxsParam.h"
#include "ofxsInteract.h"
#ifdef OFX_EXTENSIONS_VEGAS
#if defined(_WIN32) || defined(WIN32) || defined(WIN64)
#include "ofxsHWNDInteract.h"
#endif
#endif
#include "ofxsMessage.h"
#include "ofxsLog.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"
#ifdef OFX_EXTENSIONS_VEGAS
#include "ofxSonyVegas.h"
#endif
#include "ofxParametricParam.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/camera.h"
#include "nuke/fnOfxExtensions.h"
#include "nuke/fnPublicOfxExtensions.h"
#endif
#ifdef OFX_EXTENSIONS_TUTTLE
#include "tuttle/ofxReadWrite.h"
#endif
#ifdef OFX_EXTENSIONS_NATRON
#include "ofxNatron.h"
#endif

/** @brief Declares the copy constructor and the copy assignment of CLASS without defining them
    (C.81, in the C++98 form: the public headers stay C++98). Used in a protected section: a copy
    fails to compile outside of CLASS, its derived classes and their friends, and to link inside
    them. */
#define mDeclareProtectedAssignAndCC(CLASS) \
  CLASS &operator=(const CLASS &); \
  CLASS(const CLASS &);
#define mDeclareProtectedAssignAndCCBase(CLASS,BASE) \
  CLASS &operator=(const CLASS &); \
  CLASS(const CLASS &);

namespace OFX
{
  namespace Private
  {
    OfxStatus mainEntryStr(const char    *actionRaw,
      const void    *handleRaw,
      OfxPropertySetHandle   inArgsRaw,
      OfxPropertySetHandle   outArgsRaw,
      const char* plugname);

    OfxStatus customParamInterpolationV1Entry(
      const void*            handleRaw,
      OfxPropertySetHandle   inArgsRaw,
      OfxPropertySetHandle   outArgsRaw);
  }
}

#ifdef OFX_EXTENSIONS_NATRON
namespace OFX
{
  class ImageEffect;
  class PropertySet;
  namespace Private
  {
    /** @brief Library side (extension 13 of ofxNatron.h): update the keyboard modifiers of
        \p effect before an action of one of its overlay interacts.
        See ImageEffect::getKeyModifiers(). Returns true if \p props has the host's modifiers. */
    bool updateKeyModifiers(ImageEffect &effect, const std::string &action, const PropertySet &props);
    /** @brief Library side (extension 13 of ofxNatron.h): get the host's keyboard modifiers from
        the inArgs \p props of an interact action, without logging an error if they are missing:
        kOfxInteractPropKeyModifiers if ofxInteract.h defines it (OFX 1.6) and the host sets it,
        else kNatronOfxInteractPropKeyModifiers. Returns false if the host set neither. The value
        is masked to the four kNatronOfxKeyModifier* bits (the other bits are reserved). */
    bool fetchKeyModifiers(const PropertySet &props, int *modifiers);
  }
}
#endif


/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {
  /** @brief utility function to ignore an argument
    */
  template<typename T>
  static inline void
  unused(const T&) {}

  /** forward class declarations */
  class ClipDescriptor;
  class ImageEffectDescriptor;

  class Image;
  class Clip;
  class ImageEffect;
  class ImageMemory;

  /** @brief Enumerates the contexts a plugin can be used in
      (when adding one, also add it to OFX::Private::reserveEffectDescriptors() in ofxsImageEffect.cpp) */
  enum ContextEnum {eContextNone,
    eContextGenerator,
    eContextFilter,
    eContextTransition,
    eContextPaint,
    eContextGeneral,
    eContextRetimer,
#ifdef OFX_EXTENSIONS_TUTTLE
    eContextReader,
    eContextWriter,
#endif
#ifdef OFX_EXTENSIONS_NATRON
    eContextTracker
#endif
  };

  /** @brief Enumerates the pixel depths supported */
  enum BitDepthEnum {eBitDepthNone, /**< @brief bit depth that indicates no data is present */
    eBitDepthUByte,
    eBitDepthUShort,
    eBitDepthHalf,
    eBitDepthFloat,
    eBitDepthCustom, ///< some non standard bit depth
#ifdef OFX_EXTENSIONS_VEGAS
    eBitDepthUByteBGRA,   /// added support for sony vegas bgra ordered pixels
    eBitDepthUShortBGRA,
    eBitDepthFloatBGRA,
#endif
  };

  /** @brief Enumerates the component types supported */
  enum PixelComponentEnum {ePixelComponentNone,
    ePixelComponentRGBA,
    ePixelComponentRGB,
    ePixelComponentAlpha,
#ifdef OFX_EXTENSIONS_NUKE
    ePixelComponentMotionVectors,
    ePixelComponentStereoDisparity,
#endif
#ifdef OFX_EXTENSIONS_NATRON
    ePixelComponentXY,
#endif
    ePixelComponentCustom ///< some non standard pixel type
  };
    
#ifdef OFX_EXTENSIONS_NUKE
  enum PassThroughLevelEnum {
    //all planes not specified in output by the getClipComponents action will not be visible by effects down-stream
    ePassThroughLevelBlockAllNonRenderedPlanes = 0,
      
    //all planes not specified in output by the getClipComponents action will be pass-through from the input pass-through clip
    ePassThroughLevelPassThroughNonRenderedPlanes = 1,
      
    //all planes requested by the host are rendered, regardless of the getClipComponents action.
    //In this mode, the render action will be called once
    //for every plane (instead of a single time with all planes in parameter) and the plug-in is expected to use the regular
    //image effect suite, i.e: clipGetImage. The pixel components property of the image returned by clipGetImage is expected to
    //match what is returned by the pixel components property of the clip. This value is useful for instance for Transform effects:
    //all planes will be transformed with minimalist changes to the plug-in code.
    ePassThroughLevelRenderAllRequestedPlanes = 2
  };
    
  enum ViewInvarianceLevelEnum {
      
    // All views produced are different from each other
    eViewInvarianceAllViewsVariant = 0,
      
    // All views are similar except for pass-through planes
    eViewInvarianceOnlyPassThroughPlanesVariant,
      
    // Result is similar across all views
    eViewInvarianceAllViewsInvariant,
  };
#endif

  /** @brief Enumerates the ways a fielded image can be extracted from a clip */
  enum FieldExtractionEnum {eFieldExtractBoth,   /**< @brief extract both fields */
    eFieldExtractSingle, /**< @brief extracts a single field, so you have a half height image */
    eFieldExtractDoubled /**< @brief extracts a single field, but doubles up the field, so you have a full height image */
  };

  /** @brief Enumerates the kind of render thread safety a plugin has */
  enum RenderSafetyEnum {eRenderUnsafe,       /**< @brief can only render a single instance at any one time */
    eRenderInstanceSafe, /**< @brief can call a single render on an instance, but can render multiple instances simultaneously */
    eRenderFullySafe     /**< @brief can call render any number of times on an instance, and render multiple instances simultaneously */
  };

  /** @brief Enumerates the fields present in an image */
  enum FieldEnum {eFieldNone,   /**< @brief unfielded image */
    eFieldBoth,   /**< @brief fielded image with both fields present */
    eFieldLower,  /**< @brief only the spatially lower field is present */
    eFieldUpper,  /**< @brief only the spatially upper field is present  */
    eFieldSingle, /**< @brief image that consists of a single field, and so is half height  */
    eFieldDoubled /**< @brief image that consists of a single field, but each scan line is double, and so is full height  */
  };

#ifdef OFX_EXTENSIONS_VEGAS
  /** @brief Enumerates the pixel order in an image */
  enum PixelOrderEnum {
    ePixelOrderRGBA,   /**< @brief pixel order is RGBA (ofx typical) */
    ePixelOrderBGRA    /**< @brief pixel order is BGRA (added for Sony Vegas) */
  };
#endif

  enum PreMultiplicationEnum { eImageOpaque,          /**< @brief the image is opaque and so has no premultiplication state */
    eImagePreMultiplied,   /**< @brief the image is premultiplied by it's alpha */
    eImageUnPreMultiplied, /**< @brief the image is unpremultiplied */
  };

#ifdef OFX_EXTENSIONS_VEGAS
  /** @brief Enumerates the vegas contexts a plugin is being used in */
  enum VegasRenderQualityEnum {
    eVegasRenderQualityUnknown,
    eVegasRenderQualityDraft,
    eVegasRenderQualityPreview,
    eVegasRenderQualityGood,
    eVegasRenderQualityBest
    };

  /** @brief Enumerates the vegas contexts a plugin is being used in */
  enum VegasContextEnum {
    eVegasContextUnknown,
    eVegasContextMedia,
    eVegasContextTrack,
    eVegasContextEvent,
    eVegasContextEventFadeIn,
    eVegasContextEventFadeOut,
    eVegasContextProject,
    eVegasContextGenerator
    };

  /** @brief Enumerates the hint for vegas to display thumbnails */
  enum VegasPresetThumbnailEnum {
    eVegasPresetThumbnailDefault,
    eVegasPresetThumbnailSolidImage,
    eVegasPresetThumbnailImageWithAlpha
    };
#endif

  enum NativeOriginEnum {
    eNativeOriginBottomLeft,
    eNativeOriginTopLeft,
    eNativeOriginCenter
  };

#ifdef OFX_EXTENSIONS_NATRON

  enum ShortcutModifierEnum
  {
    eShortcutModifierNone = 0x0,
    eShortcutModifierShift = 0x1,
    eShortcutModifierCtrl = 0x2,
    eShortcutModifierAlt = 0x4,
    eShortcutModifierMeta = 0x8,
  };

#endif

  // The map* functions below throw std::invalid_argument on a string or a value they don't know,
  // except mapStrToBitDepthEnum (returns eBitDepthCustom), mapStrToPixelComponentEnum
  // (ePixelComponentCustom), mapMessageTypeEnumToStr (NULL) and mapToMessageReplyEnum
  // (eMessageReplyFailed).

  /** @brief turns a field string into and enum */
  FieldEnum mapStrToFieldEnum(const std::string &str);

#ifdef OFX_EXTENSIONS_VEGAS
  /** @brief map a std::string to a RenderQuality */
  VegasRenderQualityEnum mapToVegasRenderQualityEnum(const std::string &s);

  /** @brief map a std::string to a context */
  VegasContextEnum mapToVegasContextEnum(const std::string &s);
#endif

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief map a std::string to a context enum */
  ContextEnum mapToContextEnum(const std::string &s);

  const char* mapContextEnumToStr(ContextEnum context);

  const char* mapMessageTypeEnumToStr(OFX::Message::MessageTypeEnum type);

  OFX::Message::MessageReplyEnum mapToMessageReplyEnum(OfxStatus stat);

  InstanceChangeReason mapToInstanceChangedReason(const std::string &s);

  BitDepthEnum mapStrToBitDepthEnum(const std::string &str);

  const char* mapBitDepthEnumToStr(BitDepthEnum bitDepth);

  PixelComponentEnum mapStrToPixelComponentEnum(const std::string &str);

  const char* mapPixelComponentEnumToStr(PixelComponentEnum pixelComponent);

#if defined(OFX_EXTENSIONS_NATRON)
  /** @brief extract a custom Natron plane defined in the multi-plane extension from the kOfxImageEffectPropComponents property value, @see getPixelComponentsProperty() */
 bool extractCustomPlane(const std::string& comp, std::string* layerName, std::string* layerLabel, std::string* channelsLabel, std::vector<std::string>* channels);
#endif

  class PluginFactory
  {
  public:
    /** @brief virtual destructor */
    virtual ~PluginFactory() {};
    virtual void load() {}
    virtual void unload() {}
    virtual void describe(OFX::ImageEffectDescriptor &desc) = 0;
    virtual void describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum context) = 0;
    virtual ImageEffect* createInstance(OfxImageEffectHandle handle, ContextEnum context) = 0;
    virtual const std::string& getID() const = 0;
    virtual const std::string& getUID() const = 0;
    virtual unsigned int getMajorVersion() const = 0;
    virtual unsigned int getMinorVersion() const = 0;
    virtual OfxPluginEntryPoint* getMainEntry() = 0;
  };

  template<class FACTORY>
  class FactoryMainEntryHelper
  {
  protected:
    const std::string& getHelperID() const { return _id; }
    unsigned int getHelperMajorVersion() const  { return  _maj; }
    unsigned int getHelperMinorVersion() const  { return  _min; }
    std::string toString(unsigned int val)
    {
      std::ostringstream ss;
      ss.imbue(std::locale::classic()); // no digit grouping, whatever the global locale
      ss << val;
      return ss.str();
    }
    FactoryMainEntryHelper(const std::string& id, unsigned int maj, unsigned int min): _id(id), _maj(maj), _min(min)
    {
      assert(uid().empty()); // constructor should only be called once
      // "<id>@<major>.<minor>": with separators, so that ("foo", 1, 23) and ("foo", 12, 3) differ
      uid() = id + "@" + toString(maj) + "." + toString(min);
    }
    const std::string& getHelperUID() const { return uid(); }
    static OfxStatus mainEntry(const char *action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
    { 
      return OFX::Private::mainEntryStr(action, handle, in, out, uid().c_str());
    }

  private:
    static std::string& uid() {
      // _uid can not be a static class member, because in that case it may be
      // initialized after the constructor is called (plugin factories are declared as global variables).

      // since only one instance of FactoryMainEntryHelp<FACTORY> is constructed
      // (see the assert() above), there is no deinitialization order issue.
      // see https://isocpp.org/wiki/faq/ctors#construct-on-first-use-v2
      static std::string _uid;
      return _uid;
    }
    std::string _id;
    unsigned int _maj;
    unsigned int _min;
  };

  template<class FACTORY>
  class PluginFactoryHelper : public FactoryMainEntryHelper<FACTORY>, public PluginFactory
  {
  public:
    PluginFactoryHelper(const std::string& id, unsigned int maj, unsigned int min): FactoryMainEntryHelper<FACTORY>(id, maj, min)
    {}
    OfxPluginEntryPoint* getMainEntry() { return FactoryMainEntryHelper<FACTORY>::mainEntry; }
    const std::string& getID() const { return FactoryMainEntryHelper<FACTORY>::getHelperID(); }
    const std::string& getUID() const { return FactoryMainEntryHelper<FACTORY>::getHelperUID(); }
    unsigned int getMajorVersion() const { return FactoryMainEntryHelper<FACTORY>::getHelperMajorVersion(); }
    unsigned int getMinorVersion() const { return FactoryMainEntryHelper<FACTORY>::getHelperMinorVersion(); }
  };

#define mDeclarePluginFactory(CLASS, LOADFUNCDEF, UNLOADFUNCDEF) \
  class CLASS : public OFX::PluginFactoryHelper<CLASS> \
  { \
  public: \
  CLASS(const std::string& id, unsigned int verMaj, unsigned int verMin):OFX::PluginFactoryHelper<CLASS>(id, verMaj, verMin){} \
  virtual void load() LOADFUNCDEF ;\
  virtual void unload() UNLOADFUNCDEF ;\
  virtual void describe(OFX::ImageEffectDescriptor &desc); \
  virtual void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context); \
  virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context); \
  };

  /** @brief Declare a plugin factory with a template majorVersion parameters.
   *
   * Allows an easy definition of multiple versions of the same plugin, for backward compatibility.
   * Keep in mind that the members of PluginFactoryHelper must be accessed through this->, 
   * because when a class template derives from a base class template, the base members are
   * not visible in the derived class template definition. (This makes sense; until you specialize,
   * there is no class, and so there are no members. Explicit specializations can always change 
   * the meaning of any given template class.)
   */
#define mDeclarePluginFactoryVersioned(CLASS, LOADFUNCDEF, UNLOADFUNCDEF) \
  template<unsigned int majorVersion> \
  class CLASS : public OFX::PluginFactoryHelper<CLASS<majorVersion> > \
  { \
  public: \
  CLASS(const std::string& id, unsigned int verMin):OFX::PluginFactoryHelper<CLASS<majorVersion> >(id, majorVersion, verMin){} \
  virtual void load() LOADFUNCDEF ;\
  virtual void unload() UNLOADFUNCDEF ;\
  virtual void describe(OFX::ImageEffectDescriptor &desc); \
  virtual void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context); \
  virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context); \
  };

  typedef std::vector<PluginFactory*> PluginFactoryArray;
  struct PluginFactories {
    static OFX::PluginFactoryArray& plugIDs() {
      // Use a static variable in a member function to make sure plugIDs is initialized before PluginFactories.
      // This is OK, since s_plugIDs is not used in the PluginFactories destructor.
      // see https://isocpp.org/wiki/faq/ctors#construct-on-first-use-v2
      static OFX::PluginFactoryArray _plugIDs;
      return _plugIDs;
    }
    PluginFactories(PluginFactory* pf) { plugIDs().push_back(pf); }

    /** @brief a function that appends a binary's plug-in factories to the list, for lazy registration */
    typedef void (*RegistrationFunction)(OFX::PluginFactoryArray& factories);

    /** @brief Lazy registration: for plug-ins that are only known at run time (e.g. one per filter of
        a library that must be loaded first), which can't be static factory objects registered with
        mRegisterPluginFactoryInstance at dlopen.

        Declare one static object with the function that appends the factories, e.g.
        @code
        static void registerMyPlugins(OFX::PluginFactoryArray& factories); // push_back()s the factories
        static OFX::PluginFactories lazyRegistration(&registerMyPlugins);
        @endcode
        The Support library calls the function once, at the first OfxGetNumberOfPlugins or
        OfxGetPlugin, after any static registrations (and after OFX::Plugin::getPluginIDs with
        OFXS_MANUAL_REGISTRATION), with the list to append to. The factories must live until the
        binary is unloaded. If the function throws, the exception is logged and the host gets no
        plug-ins: the registration is not retried. One function per binary: a second, different
        one is an error (in debug builds: an assertion and a log warning; release builds replace
        the first one silently). */
    explicit PluginFactories(RegistrationFunction f)
    {
      RegistrationFunction& slot = lazyRegistrationSlot();
      OFX::Log::warning(slot != NULL && slot != f, "OFX::PluginFactories: a second, different lazy registration function replaces the first one.");
      assert(slot == NULL || slot == f);
      slot = f;
    }

    /** @brief the lazy registration function, or NULL (for the Support library's init()) */
    static RegistrationFunction lazyRegistration() { return lazyRegistrationSlot(); }

  private:
    static RegistrationFunction& lazyRegistrationSlot() {
      // a static in a member function, like plugIDs(): one per binary, and constant-initialized
      // (NULL) before any static initializer constructs a lazy registration object
      static RegistrationFunction _f = NULL;
      return _f;
    }
  };
#define mRegisterPluginFactoryInstance(pf) static OFX::PluginFactories mypluginfactory_##pf(&pf);

  /** @brief Fetch's a suite from the host and logs errors 

  All the standard suites are fetched by the support code, you should use this
  to fetch any extra non-standard suites.
  */
  const void * fetchSuite(const char *suiteName, int suiteVersion, bool optional = false);

  /** @brief OFX 1.5 colour management style (kOfxImageEffectPropColourManagementStyle, ofxColour.h).
   "The assumption is that OCIO > Full > Core > Basic". */
  enum ColourManagementStyleEnum {
    eColourManagementStyleNone,  ///< kOfxImageEffectColourManagementNone - no colour management
    eColourManagementStyleBasic, ///< kOfxImageEffectColourManagementBasic - only basic colourspaces from the config header
    eColourManagementStyleCore,  ///< kOfxImageEffectColourManagementCore - only core colourspaces from the config header
    eColourManagementStyleFull,  ///< kOfxImageEffectColourManagementFull - any colourspace from the config header
    eColourManagementStyleOCIO   ///< kOfxImageEffectColourManagementOCIO - any OCIO config (implies the OCIO library)
  };

  /** @brief OFX 1.5 OpenCL program suite (OfxOpenCLProgramSuiteV1, ofxGPURender.h): an optional
   suite "the host can provide for building OpenCL programs for the plug-in, as an alternative
   to calling clCreateProgramWithSource / clBuildProgram" (the host may add build flags and
   cache program binaries). */
  namespace OpenCL {
    /** @brief Does the host provide the OpenCL program suite? */
    bool isProgramSuiteAvailable();

    /** @brief Ask the host to compile an OpenCL program.

     \arg programSource the OpenCL C source
     \arg optional      if true, the host may skip compiling on this call
     \arg program       points to a cl_program, where the compiled program is stored
     \return the host's status, or kOfxStatErrUnsupported if the host doesn't provide the suite
     (the plug-in should then call clCreateProgramWithSource / clBuildProgram itself). Never throws.
     */
    OfxStatus compileProgram(const char *programSource, bool optional, void *program);
  }

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief A class that lists all the properties of a host */
  struct ImageEffectHostDescription {
  public :
    int APIVersionMajor;
    int APIVersionMinor;
    std::string hostName;
    std::string hostLabel;
    int versionMajor;
    int versionMinor;
    int versionMicro;
    std::string versionLabel;
    bool hostIsBackground;
    bool supportsOverlays;
    bool supportsDrawSuite; ///< OFX 1.5: the host provides OfxDrawSuiteV1, so overlays may draw without OpenGL
    bool supportsMultiResolution;
    bool supportsTiles;
    bool temporalClipAccess;
    bool supportsMultipleClipDepths;
    bool supportsMultipleClipPARs;
    bool supportsSetableFrameRate;
    bool supportsSetableFielding;
    int sequentialRender;
    bool supportsStringAnimation;
    bool supportsCustomInteract;
    bool supportsChoiceAnimation;
    bool supportsStrChoiceAnimation;
    bool supportsStrChoice; ///< kOfxParamHostPropSupportsStrChoice: check before defining a StrChoice param
    bool supportsBooleanAnimation;
    bool supportsCustomAnimation;
    void* osHandle;
    bool supportsParametricParameter;
    bool supportsParametricAnimation;
    bool supportsRenderQualityDraft;
    NativeOriginEnum nativeOrigin;
    bool supportsOpenCLRender;
    bool supportsCudaRender;
    bool supportsCudaStream;
    bool supportsMetalRender;
#ifdef OFX_SUPPORTS_OPENGLRENDER
    bool supportsOpenGLRender;
#endif
#ifdef OFX_EXTENSIONS_NUKE
    bool supportsCamera;
    bool canTransform;
    bool isMultiPlanar;
#endif
    int maxParameters;
    int maxPages;
    int pageRowCount;
    int pageColumnCount;
    typedef std::vector<PixelComponentEnum> PixelComponentArray;
    PixelComponentArray _supportedComponents;
    typedef std::vector<ContextEnum> ContextArray;
    ContextArray _supportedContexts;
    typedef std::vector<BitDepthEnum> PixelDepthArray;
    PixelDepthArray _supportedPixelDepths;
    bool supportsProgressSuite;
    bool supportsTimeLineSuite;
    bool supportsMessageSuiteV2;
    bool supportsCPURender; ///< kOfxImageEffectPropCPURenderSupported (OFX 1.5.1, host optional); true if absent
    ColourManagementStyleEnum colourManagementStyle; ///< kOfxImageEffectPropColourManagementStyle on the host (OFX 1.5); None if absent
    std::vector<std::string> colourManagementAvailableConfigs; ///< kOfxImageEffectPropColourManagementAvailableConfigs on the host (OFX 1.5)

    bool supportsMetalTexture; ///< kOfxImageEffectPropMetalTextureSupported on the host (next OFX release, after 1.5.1); false if absent
#ifdef OFX_EXTENSIONS_NATRON
    bool isNatron;
    bool supportsDynamicChoices;
    bool supportsCascadingChoices;
    bool supportsChannelSelector;
    bool canDistort;

    struct NativeOverlayHandle
    {
      // Uniquely identifies the overlay handle in the host
      std::string identifier;

      // A vector of parameters, where each parameter contains a pair <parameterHint, parameterType>
      // The parameterHint is a hint of the host as to what kind of parameter should be used for that role
      // The parameterType allows the host to contrain which parameter type should be passed to the native overlay handle
      std::vector<std::pair<std::string,std::string> > parameters;
    };
    std::vector<NativeOverlayHandle> nativeInteracts;

    /// cursors the host can display for interacts (kOfxInteractHostPropSupportedCursors,
    /// proposed extension NatronGitHub/openfx#4); empty if the host doesn't support it
    std::vector<std::string> supportedCursors;

    /// does the host pass the keyboard modifiers (kNatronOfxInteractHostPropSupportsKeyModifiers, extension 13
    /// of ofxNatron.h)? Informative only: ImageEffect::getKeyModifiers() works on all hosts.
    bool supportsInteractKeyModifiers;
#endif

  public:
    bool supportsPixelComponent(const PixelComponentEnum component) const;
    bool supportsBitDepth( const BitDepthEnum bitDepth) const;
    bool supportsContext(const ContextEnum context) const;
	
    /** @return default pixel depth supported by host application. */
    BitDepthEnum getDefaultPixelDepth() const;
	
    /** @return default pixel component supported by host application. */
    PixelComponentEnum getDefaultPixelComponent() const;
  };


  /// retrieve the host description
  ImageEffectHostDescription* getImageEffectHostDescription();

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up a clip */
  class ClipDescriptor {
  protected :
    mDeclareProtectedAssignAndCC(ClipDescriptor);

  protected :
    /** @brief name of the clip */
    std::string _clipName;

    /** @brief properties for this clip */
    PropertySet _clipProps;

  protected :
    /** @brief hidden constructor */
    ClipDescriptor(const std::string &name, OfxPropertySetHandle props);

    friend class ImageEffectDescriptor;

  public :
    const PropertySet &getPropertySet() const {return _clipProps;}

    PropertySet &getPropertySet() {return _clipProps;}

    /** @brief returns the clip name */
    const std::string &getName(void) const {return _clipName;}

    /** @brief set the label properties */
    void setLabel(const std::string &label);

    /** @brief set the label properties */
    void setLabels(const std::string &label, const std::string &shortLabel, const std::string &longLabel);

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief set the secretness of the clip, defaults to false */
    void setIsSecret(bool v);

    /** @brief set the clip hint */
    void setHint(const std::string &hint);

    /** @brief set the clip label and hint */
    void setLabelAndHint(const std::string &label, const std::string &hint);

    /** @brief say whether this clip may contain images with a distortion function attached */
    void setCanDistort(bool v);
#endif

    /** @brief set how fielded images are extracted from the clip defaults to eFieldExtractDoubled */
    void setFieldExtraction(FieldExtractionEnum v);

    /** @brief set which components are supported, defaults to none set, this must be called at least once! */
    void addSupportedComponent(PixelComponentEnum v);

    /** @brief set which components are supported. This version adds by the raw C-string label, allowing you to add
    custom component types */
    void addSupportedComponent(const std::string &comp);

    /** @brief say whether we are going to do random temporal access on this clip, defaults to false */
    void setTemporalClipAccess(bool v);

    /** @brief say whether if the clip is optional, defaults to false */
    void setOptional(bool v);

    /** @brief say whether this clip supports tiling, defaults to true */
    void setSupportsTiles(bool v);

    /** @brief say whether this clip is a 'mask', so the host can know to replace with a roto or similar, defaults to false */
    void setIsMask(bool v);

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief say whether this clip may contain images with a transform attached */
    void setCanTransform(bool v);
#endif
  };

#ifdef OFX_EXTENSIONS_NUKE
  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up a camera */
  class CameraDescriptor {
  protected :
    mDeclareProtectedAssignAndCC(CameraDescriptor);

  protected :
    /** @brief name of the camera */
    std::string _cameraName;

    /** @brief properties for this camera */
    PropertySet _cameraProps;

  protected :
    /** @brief hidden constructor */
    CameraDescriptor(const std::string &name, OfxPropertySetHandle props);

    friend class ImageEffectDescriptor;

  public :
    const PropertySet &getPropertySet() const {return _cameraProps;}

    PropertySet &getPropertySet() {return _cameraProps;}

    /** @brief returns the camera name */
    const std::string &getName(void) const {return _cameraName;}

    /** @brief set the label properties */
    void setLabel(const std::string &label);

    /** @brief set the label properties */
    void setLabels(const std::string &label, const std::string &shortLabel, const std::string &longLabel);

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief set the secretness of the camera, defaults to false */
    void setIsSecret(bool v);

    /** @brief set the camera hint */
    void setHint(const std::string &hint);

    /** @brief set the camera label and hint */
    void setLabelAndHint(const std::string &label, const std::string &hint);
#endif

    /** @brief say whether if the camera is optional, defaults to false */
    void setOptional(bool v);
  };
#endif

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up an effect descriptor, used in the describe actions */
  class ImageEffectDescriptor : public ParamSetDescriptor
  {
  protected :
    mDeclareProtectedAssignAndCCBase(ImageEffectDescriptor,ParamSetDescriptor);

  protected :
    /** @brief The effect handle */
    OfxImageEffectHandle _effectHandle;

    /** @brief properties for this effect */
    PropertySet _effectProps;

    /** @brief Set of all previously defined clips, defined on demand */
    std::map<std::string, ClipDescriptor *> _definedClips;

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief Set of all previously defined clips, defined on demand */
    std::map<std::string, CameraDescriptor *> _definedCameras;
#endif

    /** @brief Set of strings for clip preferences action (stored in here so the array persists and can be used in a property name)*/
    std::map<std::string, std::string> _clipComponentsPropNames;
    std::map<std::string, std::string> _clipDepthPropNames;
    std::map<std::string, std::string> _clipPARPropNames;
    std::map<std::string, std::string> _clipROIPropNames;
    std::map<std::string, std::string> _clipFrameRangePropNames;
#ifdef OFX_EXTENSIONS_NUKE
    std::map<std::string, std::string> _clipPlanesPropNames;
    std::map<std::string, std::string> _clipFrameViewsPropNames;
#endif

    auto_ptr<EffectOverlayDescriptor> _overlayDescriptor;
#ifdef OFX_EXTENSIONS_VEGAS
#if defined(_WIN32) || defined(WIN32) || defined(WIN64)
    auto_ptr<HWNDInteractDescriptor>  _hwndInteractDescriptor;
#endif // #if defined(_WIN32) || defined(WIN32) || defined(WIN64)
#endif
  public :
    /** @brief ctor */
    ImageEffectDescriptor(OfxImageEffectHandle handle);

    /** @brief dtor */
    ~ImageEffectDescriptor();

    const PropertySet &getPropertySet() const {return _effectProps;}

    PropertySet &getPropertySet() {return _effectProps;}

    /** @brief, set the label properties in a plugin */
    void setLabel(const std::string &label);

    /** @brief, set the label properties in a plugin */
    void setLabels(const std::string &label, const std::string &shortLabel, const std::string &longLabel);

    /** @brief, set the version properties in a plugin */
    void setVersion(int major, int minor, int micro, int build, const std::string &versionLabel);

    /** @brief Set the plugin grouping, defaults to "" */
    void setPluginGrouping(const std::string &group);

    /** @brief Set the plugin description, defaults to "" */
    void setPluginDescription(const std::string &description, bool validate = true);

    /** @brief Add a context to those supported, defaults to none, must be called at least once */
    void addSupportedContext(ContextEnum v);

    /** @brief Add a pixel depth to those supported, defaults to none, must be called at least once */
    void addSupportedBitDepth(BitDepthEnum v);

    /** @brief Add a pixel depth to those supported for OpenGL rendering, defaults to all */
    void addSupportedOpenGLBitDepth(BitDepthEnum v);

#ifdef OFX_EXTENSIONS_TUTTLE
    /** @brief Add a file extension to those supported, defaults to none */
    void addSupportedExtension(const std::string& extension);
    void addSupportedExtensions(const std::vector<std::string>& extensions);
    void addSupportedExtensions(const char* extensions[]); // NULL-terminated array of char*
    void setPluginEvaluation(double evaluation);
#endif

    /** @brief Is the plugin single instance only ? defaults to false */
    void setSingleInstance(bool v);

    /** @brief Does the plugin expect the host to perform per frame SMP threading defaults to true */
    void setHostFrameThreading(bool v);

    /** @brief Does the plugin support multi resolution images, defaults to true */
    void setSupportsMultiResolution(bool v);

    /** @brief set the instance to be sequentially renderred, this should have been part of clip preferences! */
    void setSequentialRender(bool v);

    /** @brief Have we informed the host we want to be seqentially renderred ? */
    bool getSequentialRender(void) const;

    /** @brief Does the plugin support image tiling, defaults to true */
    void setSupportsTiles(bool v);

    /** @brief Does the plugin handle render quality, defaults to false */
    void setSupportsRenderQuality(bool v);

    /** @brief Does the plugin perform temporal clip access, defaults to false */
    void setTemporalClipAccess(bool v);

    /** @brief Does the plugin want to have render called twice per frame in all circumanstances for fielded images ? defaults to true */
    void setRenderTwiceAlways(bool v);

    /** @brief Does the plugin support inputs and output clips of differing depths, defaults to false */
    void setSupportsMultipleClipDepths(bool v);

    /** @brief Does the plugin support inputs and output clips of pixel aspect ratios, defaults to false */
    void setSupportsMultipleClipPARs(bool v);

    /** @brief How thread safe is the plugin, defaults to eRenderInstanceSafe */
    void setRenderThreadSafety(RenderSafetyEnum v);

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief Indicates if the plug-in can make use of multiple CPU threads within a single call
     of the kOfxImageEffectActionRender action, for instance by using the multi-thread suite.*/
    void setUsesMultiThreading(bool isMultiThreaded);
#endif

    /** @brief If the slave  param changes the clip preferences need to be re-evaluated */
    void addClipPreferencesSlaveParam(ParamDescriptor &p);

    /** @brief Does the plugin support OpenCL Buffers render (kOfxImageEffectPropOpenCLRenderSupported, OFX 1.5), defaults to false */
    void setSupportsOpenCLBuffersRender(bool v);

    /** @brief Does the plugin support OpenCL Images render (kOfxImageEffectPropOpenCLSupported, OFX 1.5), defaults to false */
    void setSupportsOpenCLImagesRender(bool v);

    /** @brief Does the plugin support Metal texture render (kOfxImageEffectPropMetalTextureSupported, next OFX release, after 1.5.1), defaults to false.
     When the host also supports it, it MAY pass images as id<MTLTexture> (see RenderArguments::isEnabledMetalTextureRender).
     Ignored by hosts that don't know it. */
    void setSupportsMetalTextureRender(bool v);

    /** @brief Does the plugin support OpenCL Render (kOfxImageEffectPropOpenCLRenderSupported, OFX 1.5), defaults to false (same as setSupportsOpenCLBuffersRender) */
    void setSupportsOpenCLRender(bool v);

    /** @brief Does the plugin support CUDA Render (kOfxImageEffectPropCudaRenderSupported, OFX 1.5), defaults to false */
    void setSupportsCudaRender(bool v);

    /** @brief Does the plugin support CUDA streams (kOfxImageEffectPropCudaStreamSupported, OFX 1.5), defaults to false */
    void setSupportsCudaStream(bool v);

    /** @brief Does the plugin support Metal Render (kOfxImageEffectPropMetalRenderSupported, OFX 1.5), defaults to false */
    void setSupportsMetalRender(bool v);


    /** @brief kOfxImageEffectPropNoSpatialAwareness (standard since OFX 1.5.1): the plug-in can render
        without spatial awareness, i.e. disable its spatial effects when the host sets
        RenderArguments::isNoSpatialAwareness / BeginSequenceRenderArguments::isNoSpatialAwareness.
        Defaults to false. Does nothing (and doesn't throw) on hosts that don't know the property. */
    void setNoSpatialAwareness(bool v);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /** @brief Does the plugin support OpenGL accelerated rendering (but is also capable of CPU rendering) ? */
    void setSupportsOpenGLRender(bool v);

    /** @brief Does the plugin require OpenGL accelerated rendering ? */
    void setNeedsOpenGLRender(bool v);
    void addOpenGLBitDepth(BitDepthEnum bitDepth);
#endif

#ifdef OFX_EXTENSIONS_VEGAS
    /** @brief Add a guid upgrade path, defaults to none, must be called at least once */
    void addVegasUpgradePath(const std::string &guidString);

    /** @brief sets the path to a help file, defaults to none, must be called at least once */
    void setHelpPath(const std::string &helpPathString);

    /** @brief sets the context ID to a help file if it's a .chm file, defaults to none, must be called at least once */
    void setHelpContextID(int helpContextID);

    void setPresetThumbnailHint(VegasPresetThumbnailEnum thumbnailHint);
#endif

#ifdef OFX_EXTENSIONS_NUKE
      /** @brief indicate that a plugin or host can handle transform effects */
      void setCanTransform(bool v);
      
      /** @brief Indicates that a host or plugin can fetch more than a type of image from a clip*/
      void setIsMultiPlanar(bool v);
      
      /** @brief Plugin indicates to the host that it should pass through any planes not modified by the plugin*/
      void setPassThroughForNotProcessedPlanes(PassThroughLevelEnum v);
      
      /** @brief Indicates to the host that the plugin is view aware, in which case it will have to use the view calls*/
      void setIsViewAware(bool v);
      
      /** @brief Indicates to the host that a view aware plugin produces the same image independent of the view being rendered*/
      void setIsViewInvariant(ViewInvarianceLevelEnum v);
#endif
      
#ifdef OFX_EXTENSIONS_NATRON
      /** @brief Indicates if the host may add a mask that will be handled automatically. */
      void setHostMaskingEnabled(bool enabled);
      
      /** @brief Indicates if the host may add a "Mix" double parameter that will dissolve
        between the source image at 0 and the full effect at 1. */
      void setHostMixingEnabled(bool enabled);

      /** @brief Indicates if the plug-in description is written in markdown or plain-text otherwise. */
      void setDescriptionIsMarkdown(bool markdown);

      /** @brief Add a parameter that will be visible in a toolbar in the viewport of the host application. */
      void addInViewportParam(const std::string& paramName);

      /** @brief Add a shortcut for the given parameter and set its default value. The host could then display this shortcut in an editor. */
      void setDefaultParamInViewportShortcut(const std::string& paramName, int symbolKey, ShortcutModifierEnum modifiers);

      /** @brief Add a native overlay interact for the given parameters.
       
       This should be called in describeInContext().
       It sets the properties kOfxParamPropUseHostOverlayHandle on each parameter,
       and kNatronOfxPropNativeOverlays on the effect descriptor. */
      void addNativeOverlayInteractForParameters(const std::string& interactType, const std::list<ParamDescriptor*>& parameters);
#endif

    /** @brief Create a clip, only callable from describe in context

    The returned clip \em must not be deleted by the client code. This is all managed by the ImageEffectDescriptor itself.
    */
    ClipDescriptor *defineClip(const std::string &name);

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief Create a camera, only callable from describe in context

    The returned camera \em must not be deleted by the client code. This is all managed by the ImageEffectDescriptor itself.
    */
    CameraDescriptor *defineCamera(const std::string &name);
#endif

    /** @brief Access to the string maps needed for runtime properties. Because the char array must persist after the call,
    we need these to be stored in the descriptor, which is only deleted on unload.*/

    const std::map<std::string, std::string>& getClipComponentPropNames() const { return _clipComponentsPropNames; }
    const std::map<std::string, std::string>& getClipDepthPropNames() const { return _clipDepthPropNames; }
    const std::map<std::string, std::string>& getClipPARPropNames() const { return _clipPARPropNames; }
    const std::map<std::string, std::string>& getClipROIPropNames() const { return _clipROIPropNames; }
    const std::map<std::string, std::string>& getClipFrameRangePropNames() const { return _clipFrameRangePropNames; }
#ifdef OFX_EXTENSIONS_NUKE
    const std::map<std::string, std::string>& getClipPlanesPropNames() const { return _clipPlanesPropNames; }
    const std::map<std::string, std::string>& getClipFrameViewsPropNames() const { return _clipFrameViewsPropNames; }
#endif
      
    /** @brief register an overlay that draws with OpenGL (kOfxImageEffectPluginPropOverlayInteractV1)

     Its draws are V1 draws: DrawArgs::drawContext is NULL, and getDrawContext() uses OpenGL (see
     OFX::DrawContext). On a DrawSuite host, the registered entry is the V1 sibling of the
     descriptor class (InteractMainEntry<DESC>::overlayInteractMainEntryV1) if it has one, so
     that its main entry stays free for V2; else, and without the DrawSuite, the main entry. */
    virtual void setOverlayInteractDescriptor(EffectOverlayDescriptor* desc);

    /** @brief register an overlay that draws with the OFX 1.5 DrawSuite (kOfxImageEffectPluginPropOverlayInteractV2)

     The Support library applies the spec strictly (ofxInteract.h, kOfxInteractActionDraw: "If
     this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite.
     If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use
     OpenGL."): a V2 draw uses only the DrawSuite, a V1 draw uses only OpenGL. See
     OFX::DrawContext and DrawArgs::getDrawContext().

     - Host with the DrawSuite (getImageEffectHostDescription()->supportsDrawSuite): the overlay
       is registered as kOfxImageEffectPluginPropOverlayInteractV2. Its draw() gets
       DrawArgs::drawContext (non-NULL if the host passed kOfxInteractPropDrawContext), and
       getDrawContext() draws with the DrawSuite.
       If hasOpenGLDraw is true, the same descriptor class is also registered as
       kOfxImageEffectPluginPropOverlayInteractV1, with a distinct entry point
       (InteractMainEntry<DESC>::overlayInteractMainEntryV1), for the hosts that call V1. Set it
       only if draw() has its own OpenGL code for the V1 draws (calledViaOverlayInteractV2()
       false): in those, drawContext is NULL and getDrawContext() draws with OpenGL. This needs a
       descriptor whose getMainEntry() is InteractMainEntry<DESC>::overlayInteractMainEntry (as
       DefaultEffectOverlayDescriptor); with another main entry, only V2 is registered.
       If hasOpenGLDraw is false, only V2 is registered.
       The spec doesn't say which entry a host calls when both are set: HostSupport hosts may
       prefer the V1 entry (Host::preferOverlayInteractV1(), e.g. Natron), so a plug-in
       registered with hasOpenGLDraw = true may get V1 calls on DrawSuite hosts.
     - Host without the DrawSuite (e.g. an OFX 1.4 host): the compatibility layer, the only one.
       The overlay is registered as kOfxImageEffectPluginPropOverlayInteractV1 (as
       setOverlayInteractDescriptor()), drawContext is NULL, and getDrawContext() converts the
       DrawSuite calls to OpenGL (Support/Library/ofxsDrawContextOpenGL.cpp). So a plug-in that
       draws only through getDrawContext() works on every host.
     - If the host has the DrawSuite but refuses the V2 property, the overlay is registered as
       V1 only (as setOverlayInteractDescriptor()), with a warning: it then draws with OpenGL.

     The registration is recorded per entry point. On a DrawSuite host,
     setOverlayInteractDescriptor() registers the V1 sibling entry, so a descriptor class
     registered both ways keeps V1 draws OpenGL and V2 draws DrawSuite. (Only a descriptor
     without a V1 sibling, registered both ways, has its main entry recorded as V1 and V2: V2
     wins, and a warning is logged, also printed to std::cerr in DEBUG builds.)

     Setting kOfxImageEffectPluginPropOverlayInteractV2 directly (not through this function) is
     not supported: that main entry is not recorded as V2, so its draws are treated as V1
     draws (drawContext is NULL, getDrawContext() uses OpenGL), even when the host calls it as
     V2. Use setOverlayInteractDescriptorV2().

     hasOpenGLDraw: OFXS_SUPPORT_HAS_OVERLAY_HAS_OPENGL_DRAW.

     Returns true if registered as V2; false if registered as V1, or if nothing was registered
     (the host doesn't support overlays, or desc has no main entry).
     */
    bool setOverlayInteractDescriptorV2(EffectOverlayDescriptor* desc, bool hasOpenGLDraw = false);

#ifdef OFX_EXTENSIONS_VEGAS
#if defined(_WIN32) || defined(WIN32) || defined(WIN64)
    /** @brief override this to create an hwnd interact for the effect */
    virtual void setHWNDInteractDescriptor(HWNDInteractDescriptor* desc);
#endif // #if defined(_WIN32) || defined(WIN32) || defined(WIN64)
#endif
#ifdef OFX_EXTENSIONS_NATRON
  /** @brief indicate that a plugin or host can handle distortion function effects */
  void setCanDistort(bool v);

  /** @brief indicate if the host may add a channel selector */
  void setChannelSelector(PixelComponentEnum v);

  /** @brief say whether all the planes listed on the output clip in the getClipComponents action should preferably be rendered
    at once or not (e.g: optical flow plug-in that could produce bw/fw planes at once)*/
  void setRenderAllPlanes(bool enabled);
#endif

  /** @brief indicate that the plugin is obsolete, so that the host doesn't show it in its plugin browser, but still
   loads it in existing projects (kOfxImageEffectPluginPropObsolete, next OFX release, after 1.5.1).
   Upstream: the plugin is made obsolete by another version of the same plugin in the same bundle, and at least one
   version of the plugin in the bundle must not be obsolete. This fork relaxes that rule: a plugin superseded by
   another plugin or by a host-internal node may set it too, even if it is the only version of that plugin (e.g.
   openfx-io's ReadEXR, superseded by ReadOIIO; upstream openfx#221). Ignored by hosts that don't know it. */
  void setIsObsolete(bool v);

  /** @brief Can the plugin render on the CPU (kOfxImageEffectPropCPURenderSupported, OFX 1.5.1), defaults to true.
   Set to false for GPU-only plugins. Ignored by hosts that don't know it. */
  void setSupportsCPURender(bool v);

  /** @brief the highest colour management style the plugin can use (kOfxImageEffectPropColourManagementStyle,
   OFX 1.5); the style actually used by an instance is ImageEffect::getColourManagementStyle().
   Returns false if the host doesn't know the property (then there is no colour management). */
  bool setColourManagementStyle(ColourManagementStyleEnum v);

  /** @brief the native mode configs the plugin supports (kOfxImageEffectPropColourManagementAvailableConfigs,
   OFX 1.5: "This property must be set by both plug-ins and hosts"), e.g. kOfxConfigIdentifier.
   Returns false if the host doesn't know the property. */
  bool setColourManagementAvailableConfigs(const std::vector<std::string> &configs);
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up an image */
  class ImageBase {
  protected :
    /** @brief the handle that holds this image */
    PropertySet _imageProps;

    /** @brief friend so we get access to ctor */
    //friend class Clip;

    PixelComponentEnum _pixelComponents;     /**< @brief get the components in the image */
    int       _pixelComponentCount;
    int       _rowBytes;                     /**< @brief the number of bytes per scanline */
    int       _pixelBytes;                   /**< @brief the number of bytes per pixel */
    BitDepthEnum _pixelDepth;                 /**< @brief get the pixel depth */
    PreMultiplicationEnum _preMultiplication; /**< @brief premultiplication on the image */
    OfxRectI  _regionOfDefinition;           /**< @brief the RoD in pixel coordinates, this may be more or less than the bounds! */
    OfxRectI  _bounds;                       /**< @brief the bounds on the pixel data */
    double    _pixelAspectRatio;             /**< @brief the pixel aspect ratio */
    FieldEnum _field;                        /**< @brief which field this represents */
    std::string _uniqueID;                   /**< @brief the unique ID of this image */
    OfxPointD _renderScale;                  /**< @brief any scaling factor applied to the image */
#ifdef OFX_EXTENSIONS_NUKE
    double _transform[9];                    /**< @brief a 2D transform to apply to the image */
    bool _transformIsIdentity;
#endif
#ifdef OFX_EXTENSIONS_NATRON
    OfxInverseDistortionFunctionV1 _inverseDistortionFunction;
    const void* _inverseDistortionFunctionData;
#endif

  private :
    // not copyable: the destructors of Image and Texture release the handle (C.21, R.1)
    ImageBase(const ImageBase &); // not defined: not copyable
    ImageBase &operator=(const ImageBase &); // not defined: not copyable

  public :
    /** @brief ctor */
    ImageBase(OfxPropertySetHandle props);

    /** @brief dtor */
    virtual ~ImageBase();
      
    const PropertySet &getPropertySet() const {return _imageProps;}

    PropertySet &getPropertySet() {return _imageProps;}

    /** @brief get the pixel depth */
    BitDepthEnum getPixelDepth(void) const {return _pixelDepth;}

    /** @brief get the components in the image */
    PixelComponentEnum getPixelComponents(void) const { return _pixelComponents;}

    /** @brief get the number of components in the image */
    int getPixelComponentCount(void) const { return _pixelComponentCount; }

    /** @brief get the string representing the pixel components */
    std::string getPixelComponentsProperty(void) const { return _imageProps.propGetString(kOfxImageEffectPropComponents);}

    /** @brief premultiplication on the image */
    PreMultiplicationEnum getPreMultiplication(void) const { return _preMultiplication;}

  protected:
    friend class ImageEffect;
    // protected for safety, because Fusion gives a bad RS:
    // Effects should use the RS from inArgs instead,
    // or they will not run on Fusion.
    /** @brief get the scale factor that has been applied to this image */
    const OfxPointD& getRenderScale(void) const { return _renderScale;}

  public:

    /** @brief get the scale factor that has been applied to this image */
    double getPixelAspectRatio(void) const { return _pixelAspectRatio;}

    /** @brief get the region of definition (in pixel coordinates) of this image */
    const OfxRectI& getRegionOfDefinition(void) const { return _regionOfDefinition;}

    /** @brief get the bounds on the image data (in pixel coordinates) of this image */
    const OfxRectI& getBounds(void) const { return _bounds;}

    /** @brief get the row bytes, may be negative */
    int getRowBytes(void) const { return _rowBytes;}

    /** @brief get the number of bytes per pixel */
    int getPixelBytes(void) const { return _pixelBytes;}

    /** @brief get the fielding of this image */
    FieldEnum getField(void) const { return _field;}

    /** @brief the unique ID of this image */
    const std::string& getUniqueIdentifier(void) const { return _uniqueID;}

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief the 2D transform attached to this image. */
    void getTransform(double t[9]) const { for (int i = 0; i < 9; ++i) { t[i] = _transform[i]; } }

    /** @brief is the transform identity? */
    bool getTransformIsIdentity() const { return _transformIsIdentity; }
#endif

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief the 2D distortion function attached to this image. */
    OfxInverseDistortionFunctionV1 getInverseDistortionFunction(const void** distortionFunctionData) const {
      *distortionFunctionData = _inverseDistortionFunctionData;
      return _inverseDistortionFunction;
    }
#endif
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up an image */
  class Image : public ImageBase {
  protected :
    void     *_pixelData;                    /**< @brief the base address of the image */
    void     *_OpenCLImage;                  /**< @brief the OpenCL Image handle (cl_mem), or NULL (OFX 1.5) */

  private :
    /** @brief the offset in bytes of the pixel (x,y) from the base address of an image of the given
        bounds, row bytes and pixel bytes. Signed: kOfxImagePropRowBytes may be negative (an image
        stored top to bottom), and an unsigned offset would only work by wrapping around (ES.100,
        ES.102, ES.106). (x,y) must be in the bounds. */
    static std::ptrdiff_t pixelOffset(int x, int y, const OfxRectI &bounds, int rowBytes, int pixelBytes)
    {
      return static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes
        + static_cast<std::ptrdiff_t>(x - bounds.x1) * pixelBytes;
    }

  public :
    /** @brief ctor */
    Image(OfxPropertySetHandle props);

    /** @brief dtor */
    virtual ~Image();

    /** @brief get the pixel data for this image */
    void *getPixelData(void) { return _pixelData;}

    /** @brief get the pixel data for this image */
    const void *getPixelData(void) const { return _pixelData;}

    /** @brief get the OpenCL Image handle (cl_mem) if the host passes the image as an OpenCL Image
     (kOfxImageEffectPropOpenCLImage, OFX 1.5), else NULL. In that case getPixelData() is not used. */
    void *getOpenCLImage(void) { return _OpenCLImage;}

    /** @brief get the OpenCL Image handle (cl_mem), or NULL */
    const void *getOpenCLImage(void) const { return _OpenCLImage;}

    /** @brief return a pixel pointer, returns NULL if (x,y) is outside the image bounds

    x and y are in pixel coordinates

    If the components are custom, then this will return NULL as the support code
    can't know the pixel size to do the work.
    */
    void *getPixelAddress(int x, int y)
    {
      // inline: it is called per pixel
      if (x < _bounds.x1 || x >= _bounds.x2 || y < _bounds.y1 || y >= _bounds.y2 || _pixelBytes == 0) {
        return 0;
      }
      return static_cast<char *>(_pixelData) + pixelOffset(x, y, _bounds, _rowBytes, _pixelBytes);
    }

    /** @brief return a pixel pointer, returns NULL if (x,y) is outside the image bounds

    x and y are in pixel coordinates

    If the components are custom, then this will return NULL as the support code
    can't know the pixel size to do the work.
    */
    const void *getPixelAddress(int x, int y) const
    {
      if (x < _bounds.x1 || x >= _bounds.x2 || y < _bounds.y1 || y >= _bounds.y2 || _pixelBytes == 0) {
        return 0;
      }
      return static_cast<const char *>(_pixelData) + pixelOffset(x, y, _bounds, _rowBytes, _pixelBytes);
    }

    /** @brief return a pixel pointer, returns the nearest pixel if (x,y) is outside the image bounds

    x and y are in pixel coordinates

    If the components are custom, then this will return NULL as the support code
    can't know the pixel size to do the work.
    */
    void *getPixelAddressNearest(int x, int y)
    {
      // clamp to the bounds (no std::min/max: <algorithm> isn't needed here)
      x = (x < _bounds.x2 - 1) ? x : (_bounds.x2 - 1);
      x = (x > _bounds.x1) ? x : _bounds.x1;
      y = (y < _bounds.y2 - 1) ? y : (_bounds.y2 - 1);
      y = (y > _bounds.y1) ? y : _bounds.y1;
      return static_cast<char *>(_pixelData) + pixelOffset(x, y, _bounds, _rowBytes, _pixelBytes);
    }

    /** @brief return a pixel pointer, returns the nearest pixel if (x,y) is outside the image bounds

    x and y are in pixel coordinates

    If the components are custom, then this will return NULL as the support code
    can't know the pixel size to do the work.
    */
    const void *getPixelAddressNearest(int x, int y) const
    {
      x = (x < _bounds.x2 - 1) ? x : (_bounds.x2 - 1);
      x = (x > _bounds.x1) ? x : _bounds.x1;
      y = (y < _bounds.y2 - 1) ? y : (_bounds.y2 - 1);
      y = (y > _bounds.y1) ? y : _bounds.y1;
      return static_cast<const char *>(_pixelData) + pixelOffset(x, y, _bounds, _rowBytes, _pixelBytes);
    }
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up an OpenGL texture */
  class Texture : public ImageBase {
  protected :
    int _index;
    int _target;

  public :
    /** @brief ctor */
    Texture(OfxPropertySetHandle props);

    /** @brief dtor */
    virtual ~Texture();

    /** @brief get OpenGL texture id (cast to GLuint) */
    inline int getIndex() const {return _index;}
      
    /** @brief get OpenGL texture target (cast to GLenum) */
    inline int getTarget() const {return _target;}
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up a clip instance */
  class Clip {
  protected :
    mDeclareProtectedAssignAndCC(Clip);

    /** @brief name of the clip */
    std::string _clipName;

    /** @brief properties for this clip */
    PropertySet _clipProps;

    /** @brief handle for this clip */
    OfxImageClipHandle _clipHandle;

    /** @brief effect instance that owns this clip */
    ImageEffect *_effect;

    /** @brief hidden constructor */
    Clip(ImageEffect *effect, const std::string &name, OfxImageClipHandle handle, OfxPropertySetHandle props);

    /** @brief so one can be made */
    friend class ImageEffect;

#ifdef OFX_EXTENSIONS_VEGAS
    PixelOrderEnum _pixelOrder;              /**< @brief the pixel order */
#endif

  public :
    /// get the underlying property set on this clip
    const PropertySet &getPropertySet() const {return _clipProps;}

    /// get the underlying property set on this clip
    PropertySet &getPropertySet() {return _clipProps;}

    /** @brief returns the clip name */
    const std::string &getName(void) const {return _clipName;}

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief set the label property in a clip */
    void setLabel(const std::string &label);

    /** @brief set the label properties in a clip */
    void setLabels(const std::string &label, const std::string &shortLabel, const std::string &longLabel);

    /** @brief set the secretness of the clip, defaults to false */
    void setIsSecret(bool v);

    /** @brief set the clip hint */
    void setHint(const std::string &hint);

    /** @brief set the clip hint */
    void setLabelAndHint(const std::string &label, const std::string &hint);

    /* @brief Get the clip format in pixel coordinates */
    void getFormat(OfxRectI &format) const;
#endif

    /// get the OFX clip handle
    OfxImageClipHandle getHandle() {return _clipHandle;}

    /** @brief get the name */
    const std::string &name(void) const {return _clipName;}

    /** @brief fetch the label */
    void getLabel(std::string &label) const;

    /** @brief fetch the labels */
    void getLabels(std::string &label, std::string &shortLabel, std::string &longLabel) const;

    /** @brief what is the pixel depth images will be given to us as */
    BitDepthEnum getPixelDepth(void) const;

    /** @brief what is the components images will be given to us as */
    PixelComponentEnum getPixelComponents(void) const;

    /** @brief get the number of components in the image */
    int getPixelComponentCount(void) const;

    /** @brief get the string representing the pixel components */
    std::string getPixelComponentsProperty(void) const { return _clipProps.propGetString(kOfxImageEffectPropComponents);}

    /** @brief what is the actual pixel depth of the clip */
    BitDepthEnum getUnmappedPixelDepth(void) const;

    /** @brief what is the component type of the clip */
    PixelComponentEnum getUnmappedPixelComponents(void) const;

    /** @brief get the string representing the pixel components */
    std::string getUnmappedPixelComponentsProperty(void) const { return _clipProps.propGetString(kOfxImageClipPropUnmappedComponents);}

    /** @brief the colourspace of the images of this clip, as set by the host (kOfxImageClipPropColourspace, OFX 1.5);
     empty if there is no colour management. It may be a cross-reference "OfxColourspace_<clip>".
     "Plug-ins must check kOfxImageClipPropColourspace to see if their request was satisfied." */
    std::string getColourspace(void) const;

    /** @brief getColourspace(), with "OfxColourspace_<clip>" cross-references followed to an actual colourspace
     (see ImageEffect::resolveColourspace()). */
    std::string getResolvedColourspace(void) const;

    /** @brief get the components in the image */
    PreMultiplicationEnum getPreMultiplication(void) const;

    /** @brief which spatial field comes first temporally */
    FieldEnum getFieldOrder(void) const;

    /** @brief is the clip connected */
    bool isConnected(void) const;

    /** @brief can the clip be continuously sampled */
    bool hasContinuousSamples(void) const;

    /** @brief get the scale factor that has been applied to this clip */
    double getPixelAspectRatio(void) const;

    /** @brief get the frame rate, in frames per second on this clip, after any clip preferences have been applied */
    double getFrameRate(void) const;

    /** @brief the frame number the host shows the user for time 0 (kOfxImageClipPropDisplayTimeOffset, next OFX release, after 1.5.1), 0 if the host doesn't set it.
     Add it to OFX times shown to the user, and subtract it from frame numbers entered by the user. */
    double getDisplayTimeOffset(void) const;

    /** @brief return the range of frames over which this clip has images, after any clip preferences have been applied */
    OfxRangeD getFrameRange(void) const;

    /** @brief get the frame rate, in frames per second on this clip, before any clip preferences have been applied */
    double getUnmappedFrameRate(void) const;

    /** @brief return the range of frames over which this clip has images, before any clip preferences have been applied */
    OfxRangeD getUnmappedFrameRange(void) const;

    /** @brief get the RoD for this clip in the cannonical coordinate system */
    OfxRectD getRegionOfDefinition(double t) const;

#ifdef OFX_EXTENSIONS_RESOLVE // kOfxImageClipPropThumbnail is Resolve-specific, not in OFX 1.5
    /** @brief is the clip for thumbnail */
    bool isForThumbnail(void) const;
#endif

#ifdef OFX_EXTENSIONS_NUKE
    
    /** @brief get the RoD for this clip in the cannonical coordinate system for the given view */
    OfxRectD getRegionOfDefinition(double t, int view) const;
      
    /** @brief fetch an image for the given plane and view

    For the output clip, view must be the view being rendered (RenderArguments::renderView).
       
    When finished with, the client code must delete the image.
    
    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image* fetchImagePlane(double t, int view, const char* plane);
      
    /** @brief fetch an image plane, with a specific region in cannonical coordinates
       
    When finished with, the client code must delete the image.
    
    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image* fetchImagePlane(double t, int view, const char* plane, const OfxRectD& bounds);
      
    /** @brief fetch an image for the given plane and view
       
    When finished with, the client code must delete the image.
    
    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image* fetchImagePlane(double t, const char* plane);
      
    /** @brief fetch an image plane, with a specific region in cannonical coordinates
       
    When finished with, the client code must delete the image.
    
    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image* fetchImagePlane(double t, const char* plane, const OfxRectD& bounds);

    /** @brief Indicates the planes present on a clip.*/
    void getPlanesPresent(std::vector<std::string>* components) const;
      
#endif
      
#ifdef OFX_EXTENSIONS_VEGAS
    /** @brief get the pixel order of this image */
    PixelOrderEnum getPixelOrder(void) const;
#endif

    /** @brief fetch an image

    When finished with, the client code must delete the image.

    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image *fetchImage(double t);

#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    /** @brief fetch an image

    When finished with, the client code must delete the image.

    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image *fetchStereoscopicImage(double t, int view);
#endif

    /** @brief fetch an image, with a specific region in cannonical coordinates

    When finished with, the client code must delete the image.

    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image *fetchImage(double t, const OfxRectD &bounds);

    /** @brief fetch an image, with a specific region in cannonical coordinates

    When finished with, the client code must delete the image.

    If the same image is fetched twice, it must be deleted in each case, they will not be the same pointer.
    */
    Image *fetchImage(double t, const OfxRectD *bounds)
    {
      if(bounds) 
        return fetchImage(t, *bounds);
      else
        return fetchImage(t);
    }

#ifdef OFX_SUPPORTS_OPENGLRENDER
    Texture *loadTexture(double t, BitDepthEnum format = eBitDepthNone, const OfxRectD *region = NULL);
#endif
  };

#ifdef OFX_EXTENSIONS_NUKE
  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up a camera instance */
  class Camera {
  protected :
    mDeclareProtectedAssignAndCC(Camera);

    /** @brief name of the camera */
    std::string _cameraName;

    /** @brief properties for this camera */
    PropertySet _cameraProps;

    /** @brief handle for this camera */
    NukeOfxCameraHandle _cameraHandle;

    /** @brief effect instance that owns this camera */
    ImageEffect *_effect;

    /** @brief hidden constructor */
    Camera(ImageEffect *effect, const std::string &name, NukeOfxCameraHandle handle, OfxPropertySetHandle props);

    /** @brief so one can be made */
    friend class ImageEffect;

  public :
    /// get the underlying property set on this camera
    const PropertySet &getPropertySet() const {return _cameraProps;}

    /// get the underlying property set on this camera
    PropertySet &getPropertySet() {return _cameraProps;}

    /** @brief returns the camera name */
    const std::string &getName(void) const {return _cameraName;}

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief set the label property in a camera */
    void setLabel(const std::string &label);

    /** @brief set the label properties in a camera */
    void setLabels(const std::string &label, const std::string &shortLabel, const std::string &longLabel);

    /** @brief set the secretness of the camera, defaults to false */
    void setIsSecret(bool v);

    /** @brief set the camera hint */
    void setHint(const std::string &hint);

    /** @brief set the camera hint */
    void setLabelAndHint(const std::string &label, const std::string &hint);
#endif

    /// get the OFX camera handle
    NukeOfxCameraHandle getHandle() {return _cameraHandle;}

    /** @brief get the name */
    const std::string &name(void) const {return _cameraName;}

    /** @brief fetch the label */
    void getLabel(std::string &label) const;

    /** @brief fetch the labels */
    void getLabels(std::string &label, std::string &shortLabel, std::string &longLabel) const;

    /** @brief is the camera connected */
    bool isConnected(void) const;

    /** @brief Get an arbitrary camera parameter for a given time and view

     \arg camera       - the handle of the camera, as obtained from cameraGetHandle
     \arg time         - the time to evaluate the parameter for
     \arg view         - the view to evaluate the parameter for
     \arg paramName    - parameter name to look up (matches name of knob in Nuke, see defines at top of \ref ofxCamera.h)
     \arg baseReturnAddress - base address to store the evaluated result
     \arg returnSize   - the number of doubles at the baseReturnAddress
     */
    void getParameter(const char* paramName, double time, int view, double* baseReturnAddress, int returnSize) const;
  };
#endif

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Class that skins image memory allocation */
  class ImageMemory {
  protected :
    OfxImageMemoryHandle _handle;

  private :
    // not copyable: the destructor frees the memory (C.21, R.1)
    ImageMemory(const ImageMemory &); // not defined: not copyable
    ImageMemory &operator=(const ImageMemory &); // not defined: not copyable

  public :
    /** @brief ctor */
    ImageMemory(size_t nBytes, ImageEffect *associatedEffect = 0);

    /** @brief dtor */
    ~ImageMemory();

    /** @brief lock the memory and return a pointer to it */
    void *lock(void);

    /** @brief unlock the memory */
    void unlock(void);
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Host behaviour when the plug-in is unlicensed (kOfxImageEffectPropBehaviourWhenUnlicensed, next OFX release, after 1.5.1) */
  enum UnlicensedBehaviourEnum {
    eUnlicensedBehaviourNotSet,   /**< property absent: render (e.g. watermarked) and return kOfxStatOK */
    eUnlicensedBehaviourContinue, /**< kOfxUnlicensedContinue: render and return kOfxStatUnlicensed */
    eUnlicensedBehaviourFail      /**< kOfxUnlicensedFail: may return kOfxStatUnlicensed without rendering */
  };

  /** @brief POD struct to pass rendering arguments into @ref ImageEffect::render */
  struct RenderArguments {
    double    time;
    OfxPointD renderScale;
    OfxRectI  renderWindow;
    FieldEnum fieldToRender;
    bool      isEnabledOpenCLRender;
    bool      isEnabledCudaRender;
    bool      isEnabledMetalRender;
    void*     pOpenCLCmdQ;
    void*     pCudaStream;
    void*     pMetalCmdQ;            /**< kOfxImageEffectPropMetalCommandQueue (OFX 1.5): the id<MTLCommandQueue> to encode onto, NULL if absent */
#ifdef OFX_SUPPORTS_OPENGLRENDER
    bool      openGLEnabled;
#ifdef OFX_EXTENSIONS_NATRON
    void*     openGLContextData;
#endif
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    int         viewsToRender;      /// default is 1, for stereoscopic 3d: 2
#endif
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    int         renderView;         /// default is 0, for s3d left eye: 0, right eye: 1
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    VegasRenderQualityEnum renderQuality;
#endif
    bool      sequentialRenderStatus;
    bool      interactiveRenderStatus;
    bool      renderQualityDraft;
#ifdef OFX_EXTENSIONS_NUKE
    std::list<std::string> planes;
#endif
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
    UnlicensedBehaviourEnum behaviourWhenUnlicensed; /**< kOfxImageEffectPropBehaviourWhenUnlicensed (next OFX release, after 1.5.1): eUnlicensedBehaviourNotSet if absent */
    bool      isEnabledMetalTextureRender; /**< kOfxImageEffectPropMetalTextureEnabled (next OFX release, after 1.5.1): images are id<MTLTexture>. false if absent */
    bool      isNoSpatialAwareness;  /**< kOfxImageEffectPropNoSpatialAwareness (OFX 1.5.1): the host asks for a render without spatial effects (only if the plug-in declared setNoSpatialAwareness(true)). false if absent */
  };

  /** @brief POD struct to pass rendering arguments into @ref OFX::ImageEffect::isIdentity */
  struct IsIdentityArguments {
    double    time;
    OfxPointD renderScale;
    OfxRectI  renderWindow;
    FieldEnum fieldToRender;
#ifdef OFX_EXTENSIONS_NUKE
    int view;
    std::string plane;
#endif
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
  };

  /** @brief POD struct to pass arguments into  @ref OFX::ImageEffect::beginSequenceRender */
  struct BeginSequenceRenderArguments {
    OfxRangeD frameRange;
    double    frameStep;
    bool      isInteractive;
    OfxPointD renderScale;
    bool      isEnabledOpenCLRender;
    bool      isEnabledCudaRender;
    bool      isEnabledMetalRender;
    void*     pOpenCLCmdQ;
    void*     pCudaStream;
    void*     pMetalCmdQ;            /**< kOfxImageEffectPropMetalCommandQueue (OFX 1.5): the id<MTLCommandQueue> to encode onto, NULL if absent */
#ifdef OFX_SUPPORTS_OPENGLRENDER
    bool      openGLEnabled;
#ifdef OFX_EXTENSIONS_NATRON
    void*     openGLContextData;
#endif
#endif
    bool      sequentialRenderStatus;
    bool      interactiveRenderStatus;
    bool      renderQualityDraft;
#ifdef OFX_EXTENSIONS_NUKE
    int view;
#endif
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
    UnlicensedBehaviourEnum behaviourWhenUnlicensed; /**< kOfxImageEffectPropBehaviourWhenUnlicensed (next OFX release, after 1.5.1): eUnlicensedBehaviourNotSet if absent */
    bool      isEnabledMetalTextureRender; /**< kOfxImageEffectPropMetalTextureEnabled (next OFX release, after 1.5.1): images are id<MTLTexture>. false if absent */
    bool      isNoSpatialAwareness;  /**< kOfxImageEffectPropNoSpatialAwareness (OFX 1.5.1): the host asks for a render without spatial effects (only if the plug-in declared setNoSpatialAwareness(true)). false if absent */
  };

  /** @brief POD struct to pass arguments into  @ref OFX::ImageEffect::endSequenceRender */
  struct EndSequenceRenderArguments {
    bool      isInteractive;
    OfxPointD renderScale;
    bool      isEnabledOpenCLRender;
    bool      isEnabledCudaRender;
    bool      isEnabledMetalRender;
    void*     pOpenCLCmdQ;
    void*     pCudaStream;
    void*     pMetalCmdQ;            /**< kOfxImageEffectPropMetalCommandQueue (OFX 1.5): the id<MTLCommandQueue> to encode onto, NULL if absent */
#ifdef OFX_SUPPORTS_OPENGLRENDER
    bool      openGLEnabled;
#ifdef OFX_EXTENSIONS_NATRON
    void*     openGLContextData;
#endif
#endif
    bool      sequentialRenderStatus;
    bool      interactiveRenderStatus;
    bool      renderQualityDraft;
#ifdef OFX_EXTENSIONS_NUKE
    int view;
#endif
    bool      isEnabledMetalTextureRender; /**< kOfxImageEffectPropMetalTextureEnabled (next OFX release, after 1.5.1): images are id<MTLTexture>. false if absent */
  };

  /** @brief POD struct to pass arguments into  @ref OFX::ImageEffect::getRegionOfDefinition */
  struct RegionOfDefinitionArguments {
    double    time;
    OfxPointD renderScale;
#ifdef OFX_EXTENSIONS_NUKE
    int view;
#endif
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
  };

  /** @brief POD struct to pass arguments into @ref OFX::ImageEffect::getRegionsOfInterest */
  struct RegionsOfInterestArguments {
    double    time;
    OfxPointD renderScale;
    OfxRectD  regionOfInterest;
#ifdef OFX_EXTENSIONS_NUKE
    int view;
#endif
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
  };

#ifdef OFX_EXTENSIONS_VEGAS
  /** @brief POD struct to pass arguments into @ref OFX::ImageEffect::upliftVegasKeyframes */
  class SonyVegasUpliftArguments {
  protected:
      /** @brief properties for this clip */
      PropertySet _argProps;

  public:

      SonyVegasUpliftArguments(PropertySet args);

      std::string  guidUplift;
      int          keyframeCount;
      void*        commonData;
      int          commonDataSize;

      void*  getKeyframeData     (int keyframeIndex) const;
      int    getKeyframeDataSize (int keyframeIndex) const;
      double getKeyframeTime     (int keyframeIndex) const;
      VegasInterpolationEnum getKeyframeInterpolation (int keyframeIndex) const;
  };
#endif

#ifdef OFX_EXTENSIONS_NUKE
  /** @brief POD struct to pass arguments into @ref OFX::ImageEffect::getTransform */
  struct TransformArguments {
    double    time;
    OfxPointD renderScale;
    FieldEnum fieldToRender;
    bool      renderQualityDraft;
    int       renderView;
  };
#endif

#ifdef OFX_EXTENSIONS_NATRON
  /** @brief POD struct to pass arguments into @ref OFX::ImageEffect::getInverseDistortion */
  struct DistortionArguments {
    double    time;
    OfxPointD renderScale;
    FieldEnum fieldToRender;
    bool      renderQualityDraft;
    int       renderView;
  };
#endif

  /** @brief Class used to set regions of interest on a clip in @ref OFX::ImageEffect::getRegionsOfInterest

  This is a base class, the actual class is private and you don't need to see the glue involved.
  */ 
  class RegionOfInterestSetter {
  public :
    /** @brief virtual destructor */
    virtual ~RegionOfInterestSetter() {}

    /** @brief function to set the RoI of a clip, pass in the clip to set the RoI of, and the RoI itself */
    virtual void setRegionOfInterest(const Clip &clip, const OfxRectD &RoI) = 0;
  };

  /** @brief POD struct to pass arguments into @ref OFX::ImageEffect::getFramesNeeded */
  struct FramesNeededArguments {
    double    time;
    bool      isThumbnailRender;     /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1): the host considers this a "thumbnail" render. false if absent */
  };

  /** @brief Class used to set the frames needed to render a single frame of a clip in @ref OFX::ImageEffect::getFramesNeeded

  This is a base class, the actual class is private and you don't need to see the glue involved.
  */ 
  class FramesNeededSetter {
  public :
    /** @brief virtual destructor */
    virtual ~FramesNeededSetter() {}

    /** @brief function to set the frames needed on a clip, the range is min <= time <= max */
    virtual void setFramesNeeded(const Clip &clip, const OfxRangeD &range) = 0;
  };
    
#ifdef OFX_EXTENSIONS_NUKE
  struct ClipComponentsArguments {
    double time;
    int view;
  };
    
  class ClipComponentsSetter {
      
      OFX::PropertySet _outArgs;
      bool _doneSomething;
      typedef std::map<std::string, std::string> StringStringMap;
      const StringStringMap& _clipPlanesPropNames;
      std::map<std::string,std::vector<std::string> > _clipPlanes;
      
      const std::string& extractValueForName(const StringStringMap& m, const std::string& name);
      
  public:
      
      ClipComponentsSetter(OFX::PropertySet props,
                           const StringStringMap& clipPlanesPropNames)
      : _outArgs(props)
      , _doneSomething(false)
      , _clipPlanesPropNames(clipPlanesPropNames)
      , _clipPlanes()
      {
          
      }
      
      bool setOutProperties();

      /**
       * @brief Pass a plane name defined in SupportExt/ofxsMultiPlane.h
       **/
      void addClipPlane(Clip& clip, const std::string& comps);
      
      //Pass NULL into clip for non pass-through
      void setPassThroughClip(const Clip* clip,double time,int view);

  };
    
  struct FrameViewsNeededArguments {
      double time;
      int view;
  };
    
  class FrameViewsNeededSetter {
      OFX::PropertySet _outArgs;
      bool _doneSomething;
      typedef std::map<std::string, std::string> StringStringMap;
      const StringStringMap& _clipFrameViewsPropnames;
      
      // For each clip and for each view a vector of ranges
      std::map<std::string, std::map<int, std::vector<OfxRangeD> > > _frameViews;
      
      const std::string& extractValueForName(const StringStringMap& m, const std::string& name);
  public:
      
      FrameViewsNeededSetter(OFX::PropertySet props,
                             const StringStringMap& clipFrameViewsPropNames)
      : _outArgs(props)
      , _doneSomething(false)
      , _clipFrameViewsPropnames(clipFrameViewsPropNames)
      , _frameViews()
      {}
            
      bool setOutProperties();
      
      void addFrameViewsNeeded(const Clip& clip,const OfxRangeD &range, int view);
      
  };
#endif

  /** @brief Class used to set the clip preferences of the effect.
  */ 
  class ClipPreferencesSetter {
    OFX::PropertySet outArgs_;
    bool doneSomething_;
    typedef std::map<std::string, std::string> StringStringMap;
    const StringStringMap& clipDepthPropNames_;
    const StringStringMap& clipComponentPropNames_;
    const StringStringMap& clipPARPropNames_;
    const std::string& extractValueForName(const StringStringMap& m, const std::string& name);
  public :
    ClipPreferencesSetter( OFX::PropertySet props, 
      const StringStringMap& depthPropNames,
      const StringStringMap& componentPropNames,
      const StringStringMap& PARPropNames) 
      : outArgs_(props)
      , doneSomething_(false)
      , clipDepthPropNames_(depthPropNames)
      , clipComponentPropNames_(componentPropNames)
      , clipPARPropNames_(PARPropNames)
    {}

    bool didSomething(void) const {return doneSomething_;}

    /** @brief, force the host to set a clip's mapped component type to be \em comps. 

    Only callable on non optional clips in all contexts. Must set comps to be one of the types the effect says it supports on the given clip.

    See the OFX API documentation for the default values of this.
    */
    void setClipComponents(Clip &clip, PixelComponentEnum comps);

    /** @brief, force the host to set a clip's mapped bit depth be \em bitDepth

    Only callable if the OFX::ImageEffectHostDescription::supportsMultipleClipDepths is true.

    See the OFX API documentation for the default values of this.
    */
    void setClipBitDepth(Clip &clip, BitDepthEnum bitDepth);

    /** @brief request images of an input clip in one of these colourspaces, most preferred first
     (OfxImageClipPropPreferredColourspaces_<clip>, OFX 1.5 colour management).

     Only meaningful if ImageEffect::getColourManagementStyle() is not eColourManagementStyleNone; names must be
     allowed by that style (e.g. kOfxColourspaceOfxSceneLinear, or "OfxColourspace_<clip>" cross-references).
     Check Clip::getColourspace() later: "In the event that the host cannot supply images in a requested
     colourspace, it may supply images in any valid colourspace".
     Returns false (and sets nothing) if the host doesn't support it.
     */
    bool setPreferredColourspaces(Clip &clip, const std::vector<std::string> &colourspaces);

    /** @brief, force the host to set a clip's mapped Pixel Aspect Ratio to be \em PAR

    Only callable if the OFX::ImageEffectHostDescription::supportsMultipleClipPARs is true.

    Default is up to the host, generally based on the input clips. 

    Not supported by most host applications.
    */
    void setPixelAspectRatio(Clip &clip, double PAR);

    /** @brief Allows an effect to change the output frame rate 

    Only callable if OFX::ImageEffectHostDescription::supportsSetableFrameRate is true.

    Default is controlled by the host, typically the framerate of the input clips.
    */
    void setOutputFrameRate(double v);

    /** @brief Set the premultiplication state of the output clip.

    Defaults to the premultiplication state of ???
    */
    void setOutputPremultiplication(PreMultiplicationEnum v);

    /** @brief Set whether the effect can be continuously sampled.

    Defaults to false. 
    */
    void setOutputHasContinuousSamples(bool v);

    /** @brief upstream (AcademySoftwareFoundation/openfx) spelling of setOutputHasContinuousSamples() */
    void setOutputHasContinousSamples(bool v) { setOutputHasContinuousSamples(v); }

    /** @brief Sets whether the effect will produce different images in all frames, even if the no params or input images are varying (eg: a noise generator).

    Defaults to false.
    */
    void setOutputFrameVarying(bool v);

    /** @brief Sets the output fielding

    Default is host dependent, must be one of 
    - eFieldNone,  
    - eFieldLower, 
    - eFieldUpper  
    */
    void setOutputFielding(FieldEnum v);

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief Sets the output format in pixel coordinates.
     Default to first non optional input clip format
     */
    void setOutputFormat(const OfxRectI& format);
#endif
  };

  /** @brief POD data structure passing in the instance changed args */
  struct InstanceChangedArgs {
    InstanceChangeReason reason;      /**< @brief why did it change */
    double               time;        /**< time of the change */
    OfxPointD            renderScale; /**< the renderscale on the instance */
    bool                 isThumbnailRender; /**< kOfxImageEffectPropThumbnailRender (OFX 1.5.1), false if absent */
  };

  /** @brief struct to pass arguments into @ref OFX::ImageEffect::interpolateCustomParam.
  It is non-POD (it contains std::string), but it is passed as const ref, so that does
  not matter */
  struct InterpolateCustomArgs {
    double      time;
    std::string value1;
    std::string value2;
    double      keytime1;
    double      keytime2;
    double      amount;
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief arguments of kOfxImageEffectActionGetOutputColourspace (OFX 1.5) */
  struct GetOutputColourspaceArguments {
    std::vector<std::string> preferredColourspaces; ///< the host's preferred output colourspaces, most preferred first
  };

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief Wraps up an effect instance, plugin implementations need to inherit from this */
  class ImageEffect : public ParamSet
  {
  protected :
    mDeclareProtectedAssignAndCCBase(ImageEffect,ParamSet);

  private :
    /** @brief to get access to the effect handle without exposing it generally via a function */
    friend class ImageMemory;

    /** @brief The effect handle */
    OfxImageEffectHandle _effectHandle;

    /** @brief properties for this clip */
    PropertySet _effectProps;

    /** @brief the context of the effect */
    ContextEnum _context;

    /** @brief Set of all previously fetched clips, defined on demand */
    std::map<std::string, Clip *> _fetchedClips;

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief Set of all previously fetched cameras, defined on demand */
    std::map<std::string, Camera *> _fetchedCameras;
#endif

    /** @brief the overlay interacts that are open on this image effect */
    std::list<OverlayInteract *> _overlayInteracts;

    /** @brief cached result of whether progress start succeeded. */
    bool _progressStartSuccess;

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief the modifier keys that are down (see getKeyModifiers()): bit 2*i is the left key
        and bit 2*i+1 the right key of the i-th kNatronOfxKeyModifier* (Shift, Control, Alt, Meta) */
    int _modifierKeysDown;
#endif

    /** @brief used to disable checks on some hosts.

    in Resolve, kOfxImagePropField property is always kOfxImageFieldNone on OFX images, regardless of the clip properties
    */
    bool _hostIsResolve;

    /** @brief used to disable checks on some hosts.

    in Fusion, kOfxImageEffectPropRenderScale is 1/3. in the inArgs for render, but 3. on the images
    */
    bool _hostIsFusion;

    /** @brief used to disable checks on some hosts.

    in Sony Vegas, kOfxImageEffectPropRenderScale is <1 in the inArgs for downscaled render, but =1. on the images
    */
    bool _hostIsVegas;

    /** @brief ignore render scale checks */
    bool _ignoreBadRenderScale;

  public :
    /** @brief ctor */
    ImageEffect(OfxImageEffectHandle handle);

    /** @brief dtor */
    virtual ~ImageEffect();

    const PropertySet &getPropertySet() const {return _effectProps;}

    PropertySet &getPropertySet() {return _effectProps;}


    OfxImageEffectHandle getHandle(void) const {return _effectHandle;}

    /** @brief the context this effect was instantiate in */
    ContextEnum getContext(void) const;

#ifdef OFX_EXTENSIONS_VEGAS
    /** @brief the Vegas context this effect exists in */
    VegasContextEnum getVegasContext(void) const;
#endif

    /** @brief size of the project */
    OfxPointD getProjectSize(void) const;

    /** @brief origin of the project */
    OfxPointD getProjectOffset(void) const;

    /** @brief extent of the project */
    OfxPointD getProjectExtent(void) const;

    /** @brief pixel aspect ratio of the project */
    double getProjectPixelAspectRatio(void) const;

    /** @brief how long does the effect last */
    double getEffectDuration(void) const;

    /** @brief the frame rate of the project */
    double getFrameRate(void) const;

    /** @brief is the instance currently being interacted with */
    bool isInteractive(void) const;

    /** @brief set the instance to be sequentially renderred, this should have been part of clip preferences! */
    void setSequentialRender(bool v);

    /** @brief Have we informed the host we want to be seqentially renderred ? */
    bool getSequentialRender(void) const;

    /** @brief Does the plugin support image tiling ? Can only be called from changedParam or changedClip. */
    void setSupportsTiles(bool v);

    /** @brief Have we informed the host we support image tiling ? */
    bool getSupportsTiles(void) const;

    /** @brief get plugin file path */
    std::string getPluginFilePath(void) const { return _effectProps.propGetString(kOfxPluginPropFilePath); }
    
#ifdef OFX_EXTENSIONS_NUKE
    /** @brief indicate that a plugin or host can handle transform effects */
    void setCanTransform(bool v);
    
    bool getCanTransform() const;
#endif

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief indicate that a plugin or host can handle distortion function effects */
    void setCanDistort(bool v);

    bool getCanDistort() const;

    /** @brief Returns a list of planes that were created by the user on the host side*/
    void getExtraneousPlanesCreated(std::vector<std::string>* planes) const;

    /** @brief The current selection rectangle drawn by the user on the host viewport, in canonical coordinates.

     The host updates this property before it calls the kOfxActionInstanceChanged action for the
     parameter kNatronOfxParamSelectionRectangleState; it is (0,0,0,0) after a clear.
     */
    OfxRectD getSelectionRectangle() const;

    /** @brief The state of the keyboard modifiers for this effect instance, a bitmask of
        kNatronOfxKeyModifierShift, kNatronOfxKeyModifierControl, kNatronOfxKeyModifierAlt and
        kNatronOfxKeyModifierMeta (extension 13 of ofxNatron.h).

        Plug-ins should call this instead of counting the modifier key events in their overlay
        interact: it works the same on hosts with and without the extension. Call it from the
        actions of the overlay interacts of this instance (also available as
        InteractArgs::keyModifiers) and from changedParam() called because of an overlay action,
        e.g. for the host selection rectangle (kNatronOfxParamSelectionRectangleState): the
        instanceChanged action gives no modifiers, so this is the state of the last overlay
        action, e.g. the pen down that started the rectangle (see extension 7 of ofxNatron.h).
        It returns the last state seen by this instance, not a live query of the keyboard:
        - if the host passes kNatronOfxInteractPropKeyModifiers (or kOfxInteractPropKeyModifiers)
          in the inArgs of an action of an overlay interact of this instance, the state is that value;
        - otherwise, it is tracked from the kOfxInteractActionKeyDown, kOfxInteractActionKeyRepeat
          and kOfxInteractActionKeyUp actions of the modifier keys (kOfxKey_Shift_L/R,
          kOfxKey_Control_L/R, kOfxKey_Alt_L/R, kOfxKey_Meta_L/R) received by the overlay interacts of
          this instance, and cleared by their kOfxInteractActionLoseFocus (parameter interacts
          don't update it). A modifier pressed or released while no overlay interact of this
          instance had the keyboard focus is missed.
        It is 0 until the instance has seen an event.

        Only valid on the main (UI) thread, where the host calls the interact actions and
        kOfxActionInstanceChanged: never call it from a render thread.

        kNatronOfxKeyModifierControl is set while the key that the host reports as
        kOfxKey_Control_L/R is down, which is Cmd on macOS hosts built with Qt (e.g. Natron), and
        kNatronOfxKeyModifierMeta is then the Control key.
     */
    int getKeyModifiers() const;

    /** @brief true if kNatronOfxKeyModifierShift is set in getKeyModifiers() */
    bool isShiftDown() const { return (getKeyModifiers() & kNatronOfxKeyModifierShift) != 0; }
    /** @brief true if kNatronOfxKeyModifierControl is set in getKeyModifiers() (Cmd on macOS hosts built with Qt) */
    bool isControlDown() const { return (getKeyModifiers() & kNatronOfxKeyModifierControl) != 0; }
    /** @brief true if kNatronOfxKeyModifierAlt is set in getKeyModifiers() */
    bool isAltDown() const { return (getKeyModifiers() & kNatronOfxKeyModifierAlt) != 0; }
    /** @brief true if kNatronOfxKeyModifierMeta is set in getKeyModifiers() (the Control key on macOS hosts built with Qt) */
    bool isMetaDown() const { return (getKeyModifiers() & kNatronOfxKeyModifierMeta) != 0; }

  private:
    friend bool Private::updateKeyModifiers(ImageEffect &effect, const std::string &action, const PropertySet &props);

    /** @brief update the keyboard modifiers from the host's kNatronOfxInteractPropKeyModifiers
        if \p props has it, else (interact actions only) from the modifier key events. Returns
        true if \p props has the property. Called through Private::updateKeyModifiers(). */
    bool updateKeyModifiers(const std::string &action, const PropertySet &props);

  public:
#endif

    /** @brief Can this instance render on the CPU (kOfxImageEffectPropCPURenderSupported, OFX 1.5.1). Ignored by hosts that don't know it. */
    void setSupportsCPURender(bool v);

    /** @name OFX 1.5 colour management (ofxColour.h). All empty / None on hosts without colour management.

     These are read from the instance properties on every call, and the host may change them during the
     lifetime of the instance: the spec requires the host to call kOfxImageEffectActionGetOutputColourspace
     "again if the host changes any of the other colour properties", but defines no notification for such
     a change. So read them where they are used (getClipPreferences(), getOutputColourspace(),
     beginSequenceRender(), render()...), and don't cache them in the constructor.
     */
    //@{
    /** @brief the colour management style chosen by the host for this instance */
    ColourManagementStyleEnum getColourManagementStyle() const;
    /** @brief the native config used to look up colourspace strings (kOfxImageEffectPropColourManagementConfig) */
    std::string getColourManagementConfig() const;
    /** @brief the OCIO config, in the OCIO style (kOfxImageEffectPropOCIOConfig): "Filesystem path to the config
     or URI starting ocio://" ("Use of URIs for built-in configs, such as ocio://default is permitted").
     Built-in config URIs need the OCIO library >= 2.2 (OCIO_NAMESPACE::Config::CreateFromFile() accepts them
     since 2.2); with an older OCIO, a plugin should report an error for an ocio:// value. */
    std::string getOCIOConfig() const;
    /** @brief the display colourspace for a plugin-owned viewport (kOfxImageEffectPropDisplayColourspace) */
    std::string getDisplayColourspace() const;
    /** @brief the OCIO display and view for a plugin-owned viewport (empty: use the default rules) */
    std::string getOCIODisplay() const;
    std::string getOCIOView() const;

    /** @brief follow "OfxColourspace_<clip>" cross-references ("Cross-referencing between clips is possible by
     setting this property to OfxColourspace_<clip>") to an actual colourspace, using the colourspace of the
     referenced clip of this instance. Returns colourspace itself if it is not a cross-reference, and "" if a
     reference is to an unknown clip or the references form a cycle. */
    std::string resolveColourspace(const std::string &colourspace) const;
    //@}

    /** @brief Does this instance support Metal texture render (kOfxImageEffectPropMetalTextureSupported, next OFX release, after 1.5.1). Ignored by hosts that don't know it. */
    void setSupportsMetalTextureRender(bool v);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /** @brief Does the plugin support OpenGL accelerated rendering (but is also capable of CPU rendering) ? Can only be called from changedParam or changedClip (OFX 1.4). */
    void setSupportsOpenGLRender(bool v);

    /** @brief Does the plugin require OpenGL accelerated rendering ? Can only be called from changedParam or changedClip. */
    void setNeedsOpenGLRender(bool v);
#endif

    /** @brief notify host that the internal data structures need syncing back to parameters for persistence and so on.  This is reset by the host after calling SyncPrivateData. */
    void setParamSetNeedsSyncing();

    OFX::Message::MessageReplyEnum sendMessage(OFX::Message::MessageTypeEnum type, const std::string& id, const std::string& msg, bool throwIfMissing = false);

    OFX::Message::MessageReplyEnum setPersistentMessage(OFX::Message::MessageTypeEnum type, const std::string& id, const std::string& msg, bool throwIfMissing = false);
    OFX::Message::MessageReplyEnum clearPersistentMessage(bool throwIfMissing = false);

#ifdef OFX_SUPPORTS_DIALOG
    /** @brief Request the host to send a kOfxActionDialog to the plugin from its UI thread. */
    void requestDialog(OfxPropertySetHandle inArgs, void *instanceData);

    /** @brief Inform the host of redraw event so it can redraw itself */
    void notifyRedrawPending(OfxPropertySetHandle inArgs);
#endif

    /** @brief Fetch the named clip from this instance

    The returned clip \em must not be deleted by the client code. This is all managed by the ImageEffect itself.

    The first fetch of a clip inserts it into an unsynchronized map: fetch the clips in the
    constructor of the effect, as for the parameters (see ParamSet).
    */
    Clip *fetchClip(const std::string &name);

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief Fetch the named camera from this instance

    The returned camera param \em must not be deleted by the client code. This is all managed by the ImageEffect itself.
    The first fetch of a camera inserts it into an unsynchronized map, as fetchClip() does.
    */
    Camera* fetchCamera(const std::string& name);
#endif

    /** @brief Check that the render scale and the field of a fetched image corresponds to the render args
    */
    template <typename T1>
    void
    checkBadRenderScaleOrField(const T1& img, const RenderArguments& args)
    {
#    ifdef NDEBUG
      unused(img);
      unused(args);
#    else
      if (_ignoreBadRenderScale) {
        return;
      }
      if ( (img->getRenderScale().x != args.renderScale.x) ||
           (img->getRenderScale().y != args.renderScale.y) ||
           (img->getField() != args.fieldToRender) ) {
        // stderr (debug builds only): the OFX log only writes a file when DEBUG is defined
        fprintf(stderr, "OFX Host gave image with wrong scale or field properties: "
                "renderScale (%g, %g) instead of (%g, %g), field %d instead of %d\n",
                img->getRenderScale().x, img->getRenderScale().y,
                args.renderScale.x, args.renderScale.y,
                (int)img->getField(), (int)args.fieldToRender);
        OFX::Log::error(true, "OFX Host gave image with wrong scale or field properties: "
                        "renderScale (%g, %g) instead of (%g, %g), field %d instead of %d",
                        img->getRenderScale().x, img->getRenderScale().y,
                        args.renderScale.x, args.renderScale.y,
                        (int)img->getField(), (int)args.fieldToRender);
        setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        throwSuiteStatusException(kOfxStatFailed);
      }
#    endif
    }

    /** @brief Check that the render scale and the field of a fetched image corresponds to the render args
    */
    template <typename T1, typename T2>
    void
    checkBadRenderScale(const T1& img, const T2& args)
    {
#    ifdef NDEBUG
      unused(img);
      unused(args);
#    else
      if (_ignoreBadRenderScale) {
        return;
      }
      if ( (img->getRenderScale().x != args.renderScale.x) ||
           (img->getRenderScale().y != args.renderScale.y) ) {
        setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        throwSuiteStatusException(kOfxStatFailed);
      }
#    endif
    }

    /** @brief Check that the render scale of a fetched image is one
    */
    template <typename T>
    void
    checkRenderScaleOne(const T& img)
    {
#    ifdef NDEBUG
      unused(img);
#    else
      if (_ignoreBadRenderScale) {
        return;
      }
      if ( (img->getRenderScale().x != 1.) ||
           (img->getRenderScale().y != 1.) ) {
        setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        throwSuiteStatusException(kOfxStatFailed);
      }
#    endif
    }

    /** @brief does the host want us to abort rendering? */
    bool abort(void) const;

    /** @brief adds a new interact to the set of interacts open on this effect */
    void addOverlayInteract(OverlayInteract *interact);

    /** @brief removes an interact to the set of interacts open on this effect */
    void removeOverlayInteract(OverlayInteract *interact);

    /** @brief force all overlays on this interact to be redrawn */
    void redrawOverlays(void);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    bool flushOpenGLResources(void);
#endif

    ////////////////////////////////////////////////////////////////////////////////
    // these are actions that need to be overridden by a plugin that implements an effect host

    /** @brief The purge caches action, a request for an instance to free up as much memory as possible in low memory situations */
    virtual void purgeCaches(void);

    /** @brief The sync private data action, called when the effect needs to sync any private data to persistent parameters */
    virtual void syncPrivateData(void);

    /** @brief client render function, this is one of the few that must be overridden */
    virtual void render(const RenderArguments &args) = 0;

    /** @brief client begin sequence render function */
    virtual void beginSequenceRender(const BeginSequenceRenderArguments &args); 

    /** @brief client end sequence render function */
    virtual void endSequenceRender(const EndSequenceRenderArguments &args); 

    /** @brief client is identity function, returns the clip and time for the identity function 

    If the effect would do no processing for the given param set and render arguments, then this
    function should return true and set the \em identityClip pointer to point to the clip that is the identity
    and \em identityTime to be the time at which to access the clip for the identity operation.
    */
    virtual bool isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &identityTime
#ifdef OFX_EXTENSIONS_NUKE
    , int& view
    , std::string& plane
#endif
    );

    /** @brief The get RoD action.

    If the effect wants change the rod from the default value (which is the union of RoD's of all input clips)
    it should set the \em rod argument and return true.

    This is all in cannonical coordinates.
    */
    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments &args, OfxRectD &rod);

    /** @brief the get region of interest action

    If the effect wants change its region of interest on any input clip from the default values (which is the same as the RoI in the arguments)
    it should do so by calling the OFX::RegionOfInterestSetter::setRegionOfInterest function on the \em rois argument.

    Note, everything is in \em cannonical \em coordinates.
    */
    virtual void getRegionsOfInterest(const RegionsOfInterestArguments &args, RegionOfInterestSetter &rois);

    /** @brief the get frames needed action

    If the effect wants change the frames needed on an input clip from the default values (which is the same as the frame to be renderred)
    it should do so by calling the OFX::FramesNeededSetter::setFramesNeeded function on the \em frames argument.
    */
    virtual void getFramesNeeded(const FramesNeededArguments &args, FramesNeededSetter &frames);

    /** @brief get the clip preferences */
    virtual void getClipPreferences(ClipPreferencesSetter &clipPreferences);

  protected:

    /** @brief To be used in the implementatino of getClipPreferences to retrieve the default values of the properties on the output clip.
     On some hosts, properties results may be cached. getClipPreferences is the action used to compute properties on the source clip and the output clip,
     however if in the implementation of getClipPreferences the code attempts to read one of those properties on the output clip this resolves in undefined
     behavior in the spec. 
     To ensure that a plug-in may get the default properties before getClipPreferences is called, you may use the functions below
     */
    PixelComponentEnum getDefaultOutputClipComponents();
    BitDepthEnum getDefaultBitdepth();

  public:

    /** @brief the effect is about to be actively edited by a user, called when the first user interface is opened on an instance */
    virtual void beginEdit(void);

    /** @brief the effect is no longer being edited by a user, called when the last user interface is closed on an instance */
    virtual void endEdit(void);

    /** @brief the effect is about to have some values changed */
    virtual void beginChanged(InstanceChangeReason reason);

    /** @brief called when a param has just had its value changed */
    virtual void changedParam(const InstanceChangedArgs &args, const std::string &paramName);

    /** @brief called when a clip has just been changed in some way (a rewire maybe) */
    virtual void changedClip(const InstanceChangedArgs &args, const std::string &clipName);

    /** @brief the effect has just had some values changed */
    virtual void endChanged(InstanceChangeReason reason);

#ifdef OFX_SUPPORTS_DIALOG
    /** @brief called in the host's UI thread after a plugin has requested a dialog @see requestDialog() */
    virtual void dialog(void *userData);
#endif

#ifdef OFX_EXTENSIONS_VEGAS
    /** @brief Vegas requires conversion of keyframe data */
    virtual void upliftVegasKeyframes(const SonyVegasUpliftArguments &upliftInfo);

    /** @brief Vegas invokes about dialog */
    virtual bool invokeAbout();

    /** @brief Vegas invokes help dialog */
    virtual bool invokeHelp();
#endif

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief get the needed input components and produced output components*/
    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& clipComponents);
      
    /** @brief get the frame/views needed for input clips*/
    virtual void getFrameViewsNeeded(const FrameViewsNeededArguments& args, FrameViewsNeededSetter& frameViews);

    /** @brief recover a transform matrix from an effect */
    virtual bool getTransform(const TransformArguments &args, Clip * &transformClip, double transformMatrix[9]);
      
    /** @brief Returns the textual representation of a view*/
    std::string getViewName(int viewIndex) const;
    
    /** @brief Returns the number of views*/
    int getViewCount() const;
#endif

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief Implement if you effect can apply a 2D distortion.
     In the generic form, the distortion function pointer must be set and a pointer to the data that should be passed back
     to the function. A free function must also be provided to free the data once the host does not need them any more.
     This let a chance to the host to concatenate distortion effects together filter only once.
     @param distortionFunctionDataSizeHintInBytes This should indicate the size in bytes of the data held by distortionFunctionData.
     Since distortionFunctionData may contain the result of heavy computations (such as a STMap), the host will attempt to cache these data.
     However the void* does not indicate much to the host as to "how heavy" these datas are in its cache, so this parameter should hint
     the host of the size of these datas in bytes.

     If the effect distortion can be represented as a 3x3 matrix, then leave the function pointer to NULL and fill the transform matrix.
     This will enable the host to better concatenate the distortion in such cases where 3x3 matrices can be multiplied together instead
     of multiplying the transformation matrices for each pixel.
     */
    virtual bool getInverseDistortion(const DistortionArguments &args, Clip * &transformClip, double transformMatrix[9],
                               OfxInverseDistortionFunctionV1* distortionFunction,
                               void** distortionFunctionData,
                               int* distortionFunctionDataSizeHintInBytes,
                               OfxInverseDistortionDataFreeFunctionV1* freeDataFunction);
#endif

    /** @brief called when a custom param needs to be interpolated */
    virtual std::string interpolateCustomParam(const InterpolateCustomArgs &args, const std::string &paramName);

    /** @brief what is the time domain of this effect, valid only in the general context

    return true is range was set, otherwise the default (the union of the time domain of all input clips) is used
    */
    virtual bool getTimeDomain(OfxRangeD &range);

    /** @brief kOfxImageEffectActionGetOutputColourspace (OFX 1.5): which colourspace the output clip will be in,
     given the host's preferred colourspaces (args.preferredColourspaces, most preferred first).

     Return true and set colourspace (a colourspace allowed by the style in use, or a cross-reference such as
     "OfxColourspace_Source"); return false to let the host use the colourspace of the first input clip (the default).

     The result must not depend on time or view: the action has no time or view argument (its only inArg is
     kOfxImageClipPropPreferredColourspaces), and it describes the output clip as a whole. It may depend on
     parameter values ("a plug-in may change its output colourspace based on a user changing a parameter"),
     so a parameter that selects the output colourspace should not animate (setAnimates(false)), or the plugin
     should use its value at a fixed time. Clip::getColourspace() of the inputs may be used ("Assume that the
     plug-in will check kOfxImageClipPropColourspace on its input clips during this action").
     */
    virtual bool getOutputColourspace(const GetOutputColourspaceArguments &args, std::string &colourspace);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /** @brief OpenGL context attached (returns context-specific data if createContextData, or NULL if the plugin does not support multiple contexts) */
    virtual void* contextAttached(bool createContextData);

    /** @brief OpenGL context detached */
    virtual void contextDetached(void* contextData);
#endif

    /// Start doing progress.
    void progressStart(const std::string &message, const std::string &messageid = "");

    /// finish yer progress. After it, progressUpdate() and progressEnd() don't call the host
    /// until the next progressStart().
    void progressEnd();

    /// set the progress to some level of completion, returns
    /// false if you should abandon processing, true to continue
    /// (also true if there is no progress: no progressStart(), or after progressEnd())
    bool progressUpdate(double t);   

    /// get the current time on the timeline. This is not necessarily the same
    /// time as being passed to an action (eg render)
    double timeLineGetTime();

    /// set the timeline to a specific time
    void timeLineGotoTime(double t);

    /// get the first and last times available on the effect's timeline
    void timeLineGetBounds(double &t1, double &t2);  
  };  


  ////////////////////////////////////////////////////////////////////////////////
  /** @brief The OFX::Plugin namespace. All the functions in here needs to be defined by each plugin that uses the support libs.
  */  
  namespace Plugin {
    /** @brief Plugin side function used to identify the plugin to the support library.
     
     This was obsoleted by automatic plugin registration, using the macro mRegisterPluginFactoryInstance:
     static BasicExamplePluginFactory p("net.sf.openfx.basicPlugin", 1, 0);
     mRegisterPluginFactoryInstance(p)
     and, for plug-ins only known at run time, by lazy registration: a static
     OFX::PluginFactories object constructed with a registration function (see OFX::PluginFactories).
     */
    void getPluginIDs(OFX::PluginFactoryArray &id);
  };

};

// undeclare the protected assign and CC macro
#undef mDeclareProtectedAssignAndCC
#undef mDeclareProtectedAssignAndCCBase

#endif
