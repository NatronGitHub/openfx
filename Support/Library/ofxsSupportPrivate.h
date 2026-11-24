// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _ofxsSupportPrivate_H_
#define _ofxsSupportPrivate_H_

#include "ofxsInteract.h"
#ifdef OFX_EXTENSIONS_VEGAS
#if defined(_WIN32) || defined(WIN32) || defined(WIN64)
#include "ofxsHWNDInteract.h"
#endif
#endif
#include "ofxsImageEffect.h"
#include "ofxsLog.h"
#include "ofxsMultiThread.h"

/** @brief Namespace private to the ofx support library.
*/
namespace OFX {

  namespace Private {
    // External linkage: the Support files share these globals, and SupportExt declares gThreadSuite
    // and gLoadCount; its ofxsThreadSuiteCheck() may replace gThreadSuite in each plug-in's load.

    /** @brief Pointer to the host */
    extern OfxHost *gHost;

    /** @brief Pointer to the effect suite */
    extern OfxImageEffectSuiteV1 *gEffectSuite;

    /** @brief Pointer to the property suite */
    extern OfxPropertySuiteV1    *gPropSuite;

    /** @brief Pointer to the  interact suite */
    extern OfxInteractSuiteV1    *gInteractSuite;
    extern OfxDrawSuiteV1        *gDrawSuite; // OFX 1.5 DrawSuite, NULL if the host lacks it

    /** @brief Pointer to the parameter suite */
    extern OfxParameterSuiteV1   *gParamSuite;

    /** @brief Pointer to the general memory suite */
    extern OfxMemorySuiteV1      *gMemorySuite;

    /** @brief Pointer to the threading suite */
    extern OfxMultiThreadSuiteV1 *gThreadSuite;

    /** @brief Pointer to the message suite */
    extern OfxMessageSuiteV1     *gMessageSuite;

    /** @brief Pointer to the optional message suite V2 */
    extern OfxMessageSuiteV2     *gMessageSuiteV2;

    /** @brief Pointer to the optional progress suite */
    extern OfxProgressSuiteV1     *gProgressSuiteV1;

    /** @brief Pointer to the optional progress suite */
    extern OfxProgressSuiteV2     *gProgressSuiteV2;

    /** @brief Pointer to the optional timeline suite */
    extern OfxTimeLineSuiteV1     *gTimeLineSuite;

    /** @brief Pointer to the parametric parameter suite */
    extern OfxParametricParameterSuiteV1* gParametricParameterSuite;

#ifdef OFX_EXTENSIONS_NUKE
    /** @brief Pointer to the camera parameter suite (nuke ofx extension) */
    extern NukeOfxCameraSuiteV1* gCameraSuite;
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    /** @brief Pointer to the optional progress suite */
    extern OfxVegasProgressSuiteV1 *gVegasProgressSuite;

    /** @brief Pointer to the optional stereoscopic image suite */
    extern OfxVegasStereoscopicImageSuiteV1 *gVegasStereoscopicImageSuite;

    /** @brief Pointer to the optional keyframe suite */
    extern OfxVegasKeyframeSuiteV1 *gVegasKeyframeSuite;
#if defined(_WIN32) || defined(WIN32) || defined(WIN64)
    /** @brief Pointer to the optional HWND interact suite (defined in ofxsImageEffect.cpp) */
    extern OfxHWNDInteractSuiteV1 *gHWNDInteractSuite;
#endif
#endif

    /** @brief Support lib function called on an ofx load action */
    void loadAction(void);

    /** @brief Support lib function called on an ofx unload action */
    void unloadAction(void);

    /** @brief The plugin function that gets passed the host structure. 
    */
    void setHost(OfxHost *host);

    /** @brief fetches our pointer out of the props on the handle */
    ImageEffect *retrieveImageEffectPointer(OfxImageEffectHandle handle);

    /** @brief fetch the prop set from the effect handle */
    OFX::PropertySet
      fetchEffectProps(OfxImageEffectHandle handle);

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief decodes a kNatronOfxPropNativeOverlays string of the host descriptor (ofxNatron.h);
        false if it isn't one, and then handle is unchanged (ofxsImageEffect.cpp) */
    bool decodeNativeOverlayHandle(const std::string& str, ImageEffectHostDescription::NativeOverlayHandle& handle);
#endif

    /** @brief the set of descriptors, one per context used by kOfxActionDescribeInContext,  'eContextNone' is the one used by the kOfxActionDescribe */
    typedef std::map<ContextEnum, ImageEffectDescriptor*> EffectContextMap;
    
    struct VersionIDKey
    {
        unsigned int majorVersion, minorVersion;
        std::string id;
        
        VersionIDKey(): majorVersion(0), minorVersion(0), id() {}
        
        bool operator< (const VersionIDKey& rhs) const
        {
            if (id == rhs.id) {
                if (majorVersion == rhs.majorVersion) {
                    if (minorVersion == rhs.minorVersion) {
                        return false;
                    } else {
                        return minorVersion < rhs.minorVersion;
                    }
                } else {
                    return majorVersion < rhs.majorVersion;
                }
            } else {
                return id < rhs.id;
            }
        }
    };
      
    typedef std::map<VersionIDKey, EffectContextMap> EffectDescriptorMap;

    /** @brief How the overlay of a draw action was registered (DrawArgs::_overlayRegistration) */
    enum OverlayRegistrationEnum {
      eOverlayRegistrationUnknown = 0, // DrawArgs not made by the interact dispatcher
      eOverlayRegistrationV1,          // kOfxImageEffectPluginPropOverlayInteractV1, or a param interact
      eOverlayRegistrationV2           // kOfxImageEffectPluginPropOverlayInteractV2
    };

    /** @brief The registry of the overlay registrations (ofxsImageEffect.cpp).
        Record that the overlay with that entry point is registered as
        kOfxImageEffectPluginPropOverlayInteractV1 (ImageEffectDescriptor::setOverlayInteractDescriptor:
        on a DrawSuite host, the V1 sibling entry if the descriptor class has one, else the main
        entry; setOverlayInteractDescriptorV2(desc, true): the V1 sibling entry) */
    void setOverlayInteractV1(OfxPluginEntryPoint *mainEntry);

    /** @brief Record that the overlay with that main entry is registered as
        kOfxImageEffectPluginPropOverlayInteractV2 (ImageEffectDescriptor::setOverlayInteractDescriptorV2).
        If the same entry is also registered as V1, V2 wins (isOverlayInteractV2() is true), and a
        warning is logged once (and printed to std::cerr in DEBUG builds). That only happens for a
        descriptor class without a V1 sibling entry: on a DrawSuite host,
        setOverlayInteractDescriptor() registers the sibling, so the main entry is only V2. */
    void setOverlayInteractV2(OfxPluginEntryPoint *mainEntry);

    /** @brief true if the overlay with that main entry was registered as V2 */
    bool isOverlayInteractV2(OfxPluginEntryPoint *mainEntry);

    /** @brief The OpenGL V1 entry point (InteractMainEntry<DESC>::overlayInteractMainEntryV1)
        of the descriptor class whose main entry is mainEntry, or NULL if it has none (its
        main entry is not an InteractMainEntry<DESC> one). Recorded by
        setOverlayInteractV1Sibling() (ofxsInteract.h), never cleared. */
    OfxPluginEntryPoint *getOverlayInteractV1Sibling(OfxPluginEntryPoint *mainEntry);

    /** @brief Forget all the registrations (at the last kOfxActionUnload) */
    void clearOverlayInteractRegistrations();

    /** @brief Access to the private members of DrawArgs and DrawContext */
    struct DrawArgsAccess {
      static void setOverlayRegistration(DrawArgs &args, OverlayRegistrationEnum r) { args._overlayRegistration = (int)r; }
      static OverlayRegistrationEnum getOverlayRegistration(const DrawArgs &args) { return (OverlayRegistrationEnum)args._overlayRegistration; }
      static const DrawContext &drawSuiteContext(const DrawArgs &args) { return args._drawContextStorage; }
      // a V1 draw: forget the host's draw context (drawContext becomes NULL)
      static void clearDrawSuiteContext(DrawArgs &args) { args._drawContextStorage = DrawContext(); args.drawContext = NULL; }
      // the context that getDrawContext() returns for V1 overlays: getDrawContext() sets its
      // backend (the only change made through a const DrawArgs)
      static DrawContext &openGLContext(const DrawArgs &args) { return args._openGLDrawContext; }
      static void setOpenGLBackend(DrawContext &ctx, const DrawContextBackend *backend) { ctx._openGL = backend; }
      static void initOpenGLContext(DrawContext &ctx, const OfxPointD &pixelScale, double screenPixelRatio, bool drawSuiteShadows = false)
      {
        ctx._pixelScale = pixelScale;
        ctx._screenPixelRatio = screenPixelRatio;
        ctx._drawSuiteShadows = drawSuiteShadows;
      }
      // kNatronOfxInteractPropDrawSuiteShadows, for the OpenGL backend (and the self-test)
      static bool drawSuiteShadows(const DrawContext &ctx) { return ctx._drawSuiteShadows; }
    };

    /** @brief The DrawSuite emulation behind a DrawContext that has no DrawSuite handle (the
        OpenGL backend, ofxsDrawContextOpenGL.cpp).

        DrawContext calls it through virtual functions only, so that ofxsInteract.cpp has no
        reference to the OpenGL backend: the plug-ins that never call DrawArgs::getDrawContext()
        don't link ofxsDrawContextOpenGL.o (from a static library) or OpenGL.
        The functions return OfxDrawSuiteV1 statuses. */
    class DrawContextBackend {
    public:
      virtual ~DrawContextBackend() {}
      virtual OfxStatus getColour(const DrawContext &ctx, OfxStandardColour stdColour, OfxRGBAColourF *colour) const = 0;
      virtual OfxStatus setColour(const DrawContext &ctx, const OfxRGBAColourF *colour) const = 0;
      virtual OfxStatus setLineWidth(const DrawContext &ctx, float width) const = 0;
      virtual OfxStatus setLineStipple(const DrawContext &ctx, OfxDrawLineStipplePattern pattern) const = 0;
      virtual OfxStatus draw(const DrawContext &ctx, OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const = 0;
      virtual OfxStatus drawText(const DrawContext &ctx, const char *text, const OfxPointD *pos, int alignment) const = 0;

    protected:
      // the DrawContext state, for the backends
      static const OfxPointD &pixelScale(const DrawContext &ctx) { return ctx._pixelScale; }
      static double screenPixelRatio(const DrawContext &ctx) { return ctx._screenPixelRatio; }
      static bool drawSuiteShadows(const DrawContext &ctx) { return ctx._drawSuiteShadows; }
      static OfxRGBAColourF &colour(const DrawContext &ctx) { return ctx._colour; }
      static float &lineWidth(const DrawContext &ctx) { return ctx._lineWidth; }
      static OfxDrawLineStipplePattern &lineStipple(const DrawContext &ctx) { return ctx._lineStipple; }
    };

    /** @brief The OpenGL backend (ofxsDrawContextOpenGL.cpp), NULL if it was compiled with
        OFXS_DRAWCONTEXT_NO_OPENGL or if the OpenGL functions can't be loaded at run time. The
        first call loads them (no OpenGL context is needed for that): only call it in the draw
        actions that may use OpenGL (V1 entries, parameter interacts, DrawArgs not made by the
        dispatcher without a draw context), so that the other plug-ins never load OpenGL. Its
        functions must only be used if openGLContextIsCurrent(). */
    const DrawContextBackend *openGLDrawContextBackend();

    /** @brief true if an OpenGL context is current on this thread (wglGetCurrentContext,
        CGLGetCurrentContext, glXGetCurrentContext, eglGetCurrentContext, or on ELF systems
        OSMesaGetCurrentContext from the library of the gl* functions, loaded at run time); false if none is, if OpenGL can't be
        loaded, or if no such function can be loaded. Call it after openGLDrawContextBackend(). */
    bool openGLContextIsCurrent();

    /** @brief The number of functions openGLContextIsCurrent() uses (for the self-test): 0 if
        OpenGL can't be loaded, or if no such function was found */
    int openGLCurrentContextFunctionCount();
  }

  /** @brief The validation code has its own namespace */
  namespace Validation {

    /** @brief This is uses to hold a property value, used by the property checking classes.

    Could have been a union, but std::string can't be in one.
    */
    struct ValueHolder {
      std::string  vString;
      int    vInt;
      double vDouble;
      void  *vPointer;

      ValueHolder(void) : vString(), vInt(0), vDouble(0.), vPointer(NULL) {}
      ValueHolder(char  *s) : vString(s), vInt(0), vDouble(0.), vPointer(NULL) {}
      ValueHolder(const std::string &s) : vString(s), vInt(0), vDouble(0.), vPointer(NULL) {}
      ValueHolder(int    i) : vString(), vInt(i), vDouble(0.), vPointer(NULL) {}
      ValueHolder(double d) : vString(), vInt(0), vDouble(d), vPointer(NULL) {}
      ValueHolder(void  *p) : vString(), vInt(0), vDouble(0.), vPointer(p) {}

      ValueHolder &operator = (char *v)  {vString = v; return *this;}
      ValueHolder &operator = (std::string v)  {vString = v; return *this;}
      ValueHolder &operator = (void *v)  {vPointer = v; return *this;}
      ValueHolder &operator = (int v)    {vInt = v; return *this;}
      ValueHolder &operator = (double v) {vDouble = v; return *this;}

      operator const char * () {return vString.c_str();}
      operator std::string &() {return vString;}
      operator int    &() {return vInt;}
      operator double &() {return vDouble;}
      operator void * &() {return vPointer;}
    };

    /** @brief Enum used in the varargs list of the PropertyDescription constructor  */
    enum DescriptionTag {
      eDescDefault,  /** @brief following values are the default to check against */
      eDescFinished, /** @brief we have finished the description */
      eDescIntroduced, /** @brief followed by two ints, the OFX version (major, minor) that introduced the property in this property set (default: 1, 0) */
      eDescHostOptional /** @brief the host may legitimately not have this property in this property set (OFX "host_optional"), e.g. a host extension */
    };

    /** @brief  class to describe properties, check their default and set their values */
    class PropertyDescription
    {
    public :
      /** @brief name of the property */
      const std::string _name;

      /** @brief dimension of the property */
      int      _dimension;

      /** @brief What type of property is it */
      OFX::PropertyTypeEnum _ilk;

      /** @brief The default value that this property should have. Empty implies no default (eg: a host name has no default). */
      std::vector<ValueHolder> _defaultValue; 

      /** @brief OFX version that introduced the property in this property set: hosts of an older OFX version may lack it */
      int _introducedMajor;
      int _introducedMinor;

      /** @brief the host may lack this property, whatever its OFX version */
      bool _hostOptional;

    public :
      /** @brief var args constructor that is use to describe properties */
      PropertyDescription(const char *name, OFX::PropertyTypeEnum ilk, int dimension, ...);

      /** @brief dtor */
      virtual ~PropertyDescription(void) {}

      /** @brief See if the property exists in the containing property set and has the correct dimension */
      void validate(bool checkDefaults, PropertySet &propSet);

      /** @brief Must the host have this property? Not if it is host optional, or newer than the host's OFX version (kOfxPropAPIVersion) */
      bool isRequiredByHost() const;
    };

    /** @brief Describes a set of properties */
    class PropertySetDescription {
    protected :
      /** @brief name of the property set */
      const std::string           _setName;

      /** @brief the descriptions of each property */
      std::vector<PropertyDescription *> _descriptions;

      /** @brief The descriptions of each property */
      std::vector<PropertyDescription *> _deleteThese;

    public :
      /** @brief constructor. 

      The varargs zero terminated are made from pairs of PropertyDescription * and ints indicating the number of properties pointed to.
      These are to come from static arrays and need not be deleted
      */
      PropertySetDescription(const char *setName, ...);// [PropertyDescription *v, int nSetToThese]

      /** @brief destructor */
      virtual ~PropertySetDescription();

      /** @brief add another property in */
      void addProperty(PropertyDescription *desc, bool deleteOnDestruction = true);

      /** @brief See if all properties exist and have the correct dimensions */
      void validate(PropertySet &propSet, bool checkDefaults = true, bool logOrdinaryMessages = false); 
    };


    /** @brief Validates the host structure and property handle */
    void
      validateHostProperties(OfxHost *host);

    /** @brief Validates the effect descriptor properties */
    void
      validatePluginDescriptorProperties(PropertySet props, bool checkDefaults = true);

    /** @brief Validates the effect instance properties */
    void
      validatePluginInstanceProperties(PropertySet props);

    /** @brief validates a clip descriptor */
    void
      validateClipDescriptorProperties(PropertySet props);

    /** @brief validates a clip instance */
    void
      validateClipInstanceProperties(PropertySet props);

#ifdef OFX_EXTENSIONS_NUKE

    /** @brief validates a camera descriptor */
    void
      validateCameraDescriptorProperties(PropertySet props);

    /** @brief validates a camera instance */
    void
      validateCameraInstanceProperties(PropertySet props);
#endif

    /** @brief validates an image or texture instance */
    void
      validateImageBaseProperties(PropertySet props);

    /** @brief validates an image instance */
    void
      validateImageProperties(PropertySet props);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /** @brief validates an OpenGL texture descriptor */
    void
      validateTextureProperties(PropertySet props);
#endif

    /** @brief Validates action in/out arguments */
    void
      validateActionArgumentsProperties(const std::string &action, PropertySet inArgs, PropertySet outArgs);

    /** @brief Validates parameter properties */
    void
      validateParameterProperties(ParamTypeEnum paramType, 
      OFX::PropertySet paramProps,
      bool checkDefaults);

    /** @brief initialises the validation code, call this in on load */
    void initialise(void);
  }

}

#endif
