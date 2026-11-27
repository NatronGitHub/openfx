// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef HOST_DEMO_HOST_DESCRIPTOR_H
#define HOST_DEMO_HOST_DESCRIPTOR_H

namespace MyHost {

  /// the host profiles: the default one, --ofx14 and --ofx11
  enum class Profile { eDefault, eOFX14, eOFX11 };

  /// "default", "ofx14" or "ofx11" (the --report value, and the plug-in cache name suffix)
  const char* profileName(Profile profile);

  /// a host combines several things...
  ///    - a factory to create a new instance of your plugin
  ///      - it also gets to filter some calls during in the
  ///        API to check for validity and perform custom
  ///        operations (eg: add extra properties).
  ///    - it provides a description of the host application
  ///      which is passed back to the plugin.
  class Host : public OFX::Host::ImageEffect::Host
  {
  public:    
    Host();

    /// --ofx14: behave as a host that implements OFX 1.4 only. The host descriptor keeps only
    /// the OFX 1.4 host properties, fetchSuite() returns only the OFX 1.4 suites, and OfxSetHost
    /// is not called. Call it before the plug-in scan.
    void restrictToOFX14();

    /// --ofx11: behave as an OFX 1.1 host modelled on VEGAS Pro 12 (openfx-misc#66,
    /// openfx-misc's README-hosts.txt), with the quirks of other old hosts that the Support
    /// library must survive. Call it before the plug-in scan.
    /// - VEGAS Pro 12: the host descriptor keeps only the OFX 1.1 host properties, and
    ///   kOfxPropAPIVersion 1.1; no tiles, no multi-resolution, no overlays, 8-bit and float depths
    ///   only, a single clip PAR. fetchSuite() returns the OFX 1.1 suites but the interact suite,
    ///   plus the message suite V2. OfxSetHost is not called.
    /// - OFX 1.1: the double params have no kOfxParamPropDefaultCoordinateSystem (OFX 1.2), as
    ///   with disableNormalisedDefaults().
    /// - Resolve and Nuke: the plug-in descriptors have no kOfxImageEffectInstancePropSequentialRender
    ///   (README-hosts.txt); the instances don't have it either.
    /// - DaVinci Resolve and Fusion 2026: the persistent messages are refused
    ///   (https://github.com/creativevideotips/cvt-ltx-alpha-gen, src/ofx/src/plugin.cpp).
    void restrictToOFX11();

    /// --no-normalised-defaults: behave as a host that doesn't support normalised parameter
    /// defaults. The double param descriptors have no kOfxParamPropDefaultCoordinateSystem (a
    /// plug-in's set fails with kOfxStatErrUnknown), so their defaults are canonical. Call it
    /// before the plug-in scan.
    void disableNormalisedDefaults() { _normalisedDefaults = false; }

    /// the base class suites, or only the OFX 1.4 (1.1) ones with restrictToOFX14() (restrictToOFX11())
    virtual const void *fetchSuite(const char *suiteName, int suiteVersion);

    /// false with restrictToOFX14() or restrictToOFX11()
    virtual bool supportsOfxSetHost() const { return _profile == Profile::eDefault; }

    /// true if the environment variable OFX_HOSTDEMO_KEEP_LOADED is 1: the scanned plug-ins stay
    /// loaded (one kOfxActionLoad and kOfxActionDescribe), instead of being unloaded after the
    /// scan and loaded again at their first use (for the checks of both behaviours)
    virtual bool keepPluginsLoadedAfterScan() const { return _keepLoaded; }

    /// kOfxParamPropColourManagement on RGB and RGBA params (a test-host hook: hostDemo doesn't
    /// convert colours); false with restrictToOFX14() or restrictToOFX11()
    virtual bool supportsParamColourManagement() const { return _profile == Profile::eDefault; }

    /// Create a new instance of an image effect plug-in.
    ///
    /// It is called by ImageEffectPlugin::createInstance which the
    /// client code calls when it wants to make a new instance.
    /// 
    ///   \arg clientData - the clientData passed into the ImageEffectPlugin::createInstance
    ///   \arg plugin - the plugin being created
    ///   \arg desc - the descriptor for that plugin
    ///   \arg context - the context to be created in
    virtual OFX::Host::ImageEffect::Instance* newInstance(void* clientData,
                                                          OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                                                          OFX::Host::ImageEffect::Descriptor& desc,
                                                          const std::string& context);

    /// Override this to create a descriptor, this makes the 'root' descriptor
    virtual OFX::Host::ImageEffect::Descriptor *makeDescriptor(OFX::Host::ImageEffect::ImageEffectPlugin* plugin);

    /// used to construct a context description, rootContext is the main context
    virtual OFX::Host::ImageEffect::Descriptor *makeDescriptor(const OFX::Host::ImageEffect::Descriptor &rootContext, 
                                                               OFX::Host::ImageEffect::ImageEffectPlugin *plug);        

    /// used to construct populate the cache
    virtual OFX::Host::ImageEffect::Descriptor *makeDescriptor(const std::string &bundlePath, 
                                                               OFX::Host::ImageEffect::ImageEffectPlugin *plug);

    /// vmessage
    virtual OfxStatus vmessage(const char* type,
                               const char* id,
                               const char* format,
                               va_list args);

    /// vmessage
    virtual OfxStatus setPersistentMessage(const char* type,
                                           const char* id,
                                           const char* format,
                                           va_list args);
    /// vmessage
    virtual OfxStatus clearPersistentMessage();

#ifdef OFX_SUPPORTS_DIALOG
    /// @see OfxDialogSuiteV.requestDialog()
    virtual OfxStatus requestDialog(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs, void *instanceData);

    /// @see OfxDialogSuiteV.notifyRedrawPending()
    virtual OfxStatus notifyRedrawPending(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs);
#endif

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /// @see OfxImageEffectOpenGLRenderSuiteV1.flushResources()
    /// "kOfxStatReplyDefault - nothing the host could do.." (flushResources, ofxGPURender.h): hostDemo
    /// keeps no GPU resources between renders
    virtual OfxStatus flushOpenGLResources() const { return kOfxStatReplyDefault; };
#endif
  private:
    /// restrictToOFX11(): remove kOfxImageEffectInstancePropSequentialRender from a plug-in
    /// descriptor or instance
    void removeSequentialRender(OFX::Host::Property::Set& props) const;

    Profile _profile; ///< restrictToOFX14(), restrictToOFX11()
    bool _keepLoaded; ///< OFX_HOSTDEMO_KEEP_LOADED=1
    bool _normalisedDefaults; ///< false after disableNormalisedDefaults()
  };

  // my ofx host object
  extern Host gOfxHost;

}

#endif // HOST_DEMO_HOST_DESCRIPTOR_H
