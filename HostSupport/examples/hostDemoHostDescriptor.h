// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef HOST_DEMO_HOST_DESCRIPTOR_H
#define HOST_DEMO_HOST_DESCRIPTOR_H

namespace MyHost {

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

    /// --no-normalised-defaults: behave as a host that doesn't support normalised parameter
    /// defaults. The double param descriptors have no kOfxParamPropDefaultCoordinateSystem (a
    /// plug-in's set fails with kOfxStatErrUnknown), so their defaults are canonical. Call it
    /// before the plug-in scan.
    void disableNormalisedDefaults() { _normalisedDefaults = false; }

    /// the base class suites, or only the OFX 1.4 ones with restrictToOFX14()
    virtual const void *fetchSuite(const char *suiteName, int suiteVersion);

    /// false with restrictToOFX14()
    virtual bool supportsOfxSetHost() const { return !_ofx14; }

    /// true if the environment variable OFX_HOSTDEMO_KEEP_LOADED is 1: the scanned plug-ins stay
    /// loaded (one kOfxActionLoad and kOfxActionDescribe), instead of being unloaded after the
    /// scan and loaded again at their first use (for the checks of both behaviours)
    virtual bool keepPluginsLoadedAfterScan() const { return _keepLoaded; }

    /// kOfxParamPropColourManagement on RGB and RGBA params (a test-host hook: hostDemo doesn't
    /// convert colours); false with restrictToOFX14()
    virtual bool supportsParamColourManagement() const { return !_ofx14; }

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
    virtual OfxStatus flushOpenGLResources() const { return kOfxStatFailed; };
#endif
  private:
    bool _ofx14; ///< restrictToOFX14()
    bool _keepLoaded; ///< OFX_HOSTDEMO_KEEP_LOADED=1
    bool _normalisedDefaults; ///< false after disableNormalisedDefaults()
  };

  // my ofx host object
  extern Host gOfxHost;

}

#endif // HOST_DEMO_HOST_DESCRIPTOR_H
