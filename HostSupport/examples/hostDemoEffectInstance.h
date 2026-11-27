// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef HOST_DEMO_EFFECT_INSTANCE_H
#define HOST_DEMO_EFFECT_INSTANCE_H

#include <cstdarg> // va_list
#include <map>
#include <string>
#include <vector>

#ifdef HOSTDEMO_OPENGL
#include "ofxhOpenGLRender.h"
#endif
#ifdef HOSTDEMO_OPENCL
#include "ofxhOpenCLRender.h"
#endif

namespace MyHost {

  /// a message the plug-in sent (message suite or persistent message), for hostDemo --report
  struct MessageRecord {
    std::string kind; ///< "message", "persistentMessage" or "clearPersistentMessage"
    std::string type; ///< e.g. kOfxMessageError (empty for clearPersistentMessage)
    std::string id;
    std::string text; ///< the formatted message
  };

  /// a progress the plug-in started (progress suite), for hostDemo --report
  struct ProgressRecord {
    std::string message;
    std::string id;
    int         updates;    ///< the number of progressUpdate calls
    double      lastValue;  ///< the last progressUpdate value (only meaningful if updates > 0)
    int         ends;       ///< the number of progressEnd calls (more than 1 is a plug-in bug: test_tester.py checks 1)
  };

  // class definition
  class MyEffectInstance : public OFX::Host::ImageEffect::Instance {
  protected:
    // the render state, set by hostDemo around each renderAction, used by the clips to make images
    bool      _rendering;
    OfxPointD _renderScale;
    OfxRectI  _renderWindow;
    std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD> _rois;
    std::string _defaultDepth; ///< the depth of the output when no input is connected (hostDemo -d)
    std::vector<MessageRecord>  _messages; ///< the messages sent by the plug-in, in order
    std::vector<ProgressRecord> _progress; ///< the progresses started by the plug-in, in order
    double _timeLineTime; ///< the time of the current action (timeLineGetTime)
    bool _refusePersistentMessages; ///< setRefusePersistentMessages()
#ifdef HOSTDEMO_OPENGL
    OFX::Host::OpenGL::RenderSession *_glSession = nullptr; ///< setOpenGLSession()
#endif
#ifdef HOSTDEMO_OPENCL
    OFX::Host::OpenCL::RenderSession *_clSession = nullptr; ///< setOpenCLSession()
#endif

    /// format, print (on stdout) and record a message
    OfxStatus recordMessage(const char* kind, const char* type, const char* id, const char* format, va_list args);

  public:
    MyEffectInstance(OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                     OFX::Host::ImageEffect::Descriptor& desc,
                     const std::string& context);

    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    // overridden for ImageEffect::Instance
    
    /// get default output fielding. This is passed into the clip prefs action
    /// and  might be mapped (if the host allows such a thing)
    virtual const std::string &getDefaultOutputFielding() const;
    
    /// make a clip
    OFX::Host::ImageEffect::ClipInstance* newClipInstance(OFX::Host::ImageEffect::Instance* plugin,
                                                          OFX::Host::ImageEffect::ClipDescriptor* descriptor,
                                                          int index);

    
    /// vmessage
    virtual OfxStatus vmessage(const char* type,
                               const char* id,
                               const char* format,
                               va_list args);       
    
    virtual OfxStatus setPersistentMessage(const char* type,
                                           const char* id,
                                           const char* format,
                                           va_list args);

    virtual OfxStatus clearPersistentMessage();       

    // The size of the current project in canonical coordinates. 
    // The size of a project is a sub set of the kOfxImageEffectPropProjectExtent. For example a 
    // project may be a PAL SD project, but only be a letter-box within that. The project size is 
    // the size of this sub window. 
    virtual void getProjectSize(double& xSize, double& ySize) const;

    // The offset of the current project in canonical coordinates. 
    // The offset is related to the kOfxImageEffectPropProjectSize and is the offset from the origin 
    // of the project 'subwindow'. For example for a PAL SD project that is in letterbox form, the
    // project offset is the offset to the bottom left hand corner of the letter box. The project 
    // offset is in canonical coordinates. 
    virtual void getProjectOffset(double& xOffset, double& yOffset) const;

    // The extent of the current project in canonical coordinates. 
    // The extent is the size of the 'output' for the current project. See ProjectCoordinateSystems 
    // for more information on the project extent. The extent is in canonical coordinates and only 
    // returns the top right position, as the extent is always rooted at 0,0. For example a PAL SD 
    // project would have an extent of 768, 576. 
    virtual void getProjectExtent(double& xSize, double& ySize) const;

    // The pixel aspect ratio of the current project 
    virtual double getProjectPixelAspectRatio() const;

    // The duration of the effect 
    // This contains the duration of the plug-in effect, in frames. 
    virtual double getEffectDuration() const;

    // For an instance, this is the frame rate of the project the effect is in. 
    virtual double getFrameRate() const;

    /// This is called whenever a param is changed by the plugin so that
    /// the recursive instanceChangedAction will be fed the correct frame 
    virtual double getFrameRecursive() const;

    /// This is called whenever a param is changed by the plugin so that
    /// the recursive instanceChangedAction will be fed the correct
    /// renderScale
    virtual void getRenderScaleRecursive(double &x, double &y) const;

    /// the clip instances, by name
    const std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>& getClipInstances() const { return _clips; }

    /// set the render state before calling renderAction: the render scale, the render window (in pixel
    /// coordinates) and the regions of interest returned by getRegionOfInterestAction
    void setRenderState(const OfxPointD& renderScale, const OfxRectI& renderWindow,
                        const std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD>& rois);

    /// call this after renderAction (the render scale is kept)
    void clearRenderState();

    bool isRendering() const { return _rendering; }
    const OfxPointD& getRenderScale() const { return _renderScale; }
    const OfxRectI& getRenderWindow() const { return _renderWindow; }

    /// get the region of interest of a clip (in canonical coordinates) for the current render.
    /// Returns false outside of render, or if the plug-in did not ask for this clip.
    bool getRegionOfInterest(const OFX::Host::ImageEffect::ClipInstance* clip, OfxRectD& roi) const;

    /// the project frame (offset and size) in pixel coordinates at the given render scale
    OfxRectI getProjectPixelRect(const OfxPointD& renderScale) const;

    /// hostDemo crops and pads the inputs of a plug-in without multiple resolutions (fetchImage())
    virtual bool cropsAndPadsImages() const { return true; }

    /// set the depth the clip preferences use when no input clip is connected (default: byte)
    void setDefaultDepth(const std::string& depth) { _defaultDepth = depth; }

    /// the default clip preferences of HostSupport, except that with no connected input the
    /// output gets the default depth (the -d option), instead of float
    virtual void setDefaultClipPreferences();

    /// --ofx11 (Host::restrictToOFX11()): refuse the persistent messages (kOfxStatFailed,
    /// nothing recorded)
    void setRefusePersistentMessages() { _refusePersistentMessages = true; }

    /// the messages and progresses of the plug-in, for hostDemo --report
    const std::vector<MessageRecord>& getMessages() const { return _messages; }
    const std::vector<ProgressRecord>& getProgress() const { return _progress; }


    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    // overridden for Param::SetInstance
    
    /// make a parameter instance
    ///
    /// Client host code needs to implement this
    virtual OFX::Host::Param::Instance* newParam(const std::string& name, OFX::Host::Param::Descriptor& Descriptor);        
    
    /// Triggered when the plug-in calls OfxParameterSuiteV1::paramEditBegin
    ///
    /// Client host code needs to implement this
    virtual OfxStatus editBegin(const std::string& name);
    
    /// Triggered when the plug-in calls OfxParameterSuiteV1::paramEditEnd
    ///
    /// Client host code needs to implement this
    virtual OfxStatus editEnd();


    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    // overridden for Progress::ProgressI
    
    /// Start doing progress. 
    virtual void progressStart(const std::string &message, const std::string &messageid);
    
    /// finish yer progress
    virtual void progressEnd();
    
    /// set the progress to some level of completion, returns
    /// false if you should abandon processing, true to continue
    virtual bool progressUpdate(double t);        

    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////
    // overridden for TimeLine::TimeLineI

    /// get the current time on the timeline. This is not necessarily the same
    /// time as being passed to an action (eg render)
    virtual double timeLineGetTime();
    
    /// set the timeline to a specific time
    virtual void timeLineGotoTime(double t);
    
    /// get the first and last times available on the effect's timeline
    virtual void timeLineGetBounds(double &t1, double &t2);    

#ifdef HOSTDEMO_OPENGL
    /// --gpu=opengl: the session of the OpenGL action in progress, which MyClipInstance::loadTexture()
    /// forwards to (nullptr outside)
    void setOpenGLSession(OFX::Host::OpenGL::RenderSession *session) { _glSession = session; }
    OFX::Host::OpenGL::RenderSession *getOpenGLSession() const { return _glSession; }
#endif
#ifdef HOSTDEMO_OPENCL
    /// --gpu=opencl: the session of the OpenCL action in progress, whose images MyClipInstance's
    /// getImage() gives (nullptr outside; read on the plug-in's threads too, set before the action)
    void setOpenCLSession(OFX::Host::OpenCL::RenderSession *session) { _clSession = session; }
    OFX::Host::OpenCL::RenderSession *getOpenCLSession() const { return _clSession; }
#endif

#ifdef OFX_EXTENSIONS_NUKE
    /// Returns the number of views
    virtual OfxStatus getViewCount(int *nViews) const;
          
    /// Returns the view textual representation
    /// The string is owned by the host and must be valid throughout the calling action
    virtual OfxStatus getViewName(int viewIndex, const char** name) const;
#endif
  };

}

#endif // HOST_DEMO_EFFECT_INSTANCE_H
