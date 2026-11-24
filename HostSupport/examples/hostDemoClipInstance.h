/*
Software License :

Copyright (c) 2007, The Open Effects Association Ltd. All rights reserved.

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
*/

#ifndef HOST_DEMO_CLIP_INSTANCE_H
#define HOST_DEMO_CLIP_INSTANCE_H

#define OFXHOSTDEMOCLIPLENGTH 1.0

#include <map>
#include <string>
#include <vector>

namespace MyHost {

  // foward
  class MyClipInstance;

  /// a PFM image (http://www.pauldebevec.com/Research/HDR/PFM/): "PF" (RGB, 3 components) or
  /// "Pf" (greyscale, 1 component), stored as in the file: "left to right, bottom to top order",
  /// i.e. data[(y * width + x) * nComps + c], with y = 0 the bottom row, as in OFX images.
  struct PFMImage {
    int width;
    int height;
    int nComps; ///< 3 (PF) or 1 (Pf)
    std::vector<float> data;
    PFMImage() : width(0), height(0), nComps(0) {}
  };

  /// read a little-endian PFM file (negative scale; its magnitude is ignored).
  /// Returns false, with the reason in error, if the file can't be read or is malformed: bad
  /// identifier, size or scale, or pixel data that doesn't match the size (truncated or trailing).
  bool readPFM(const std::string& path, PFMImage& image, std::string& error);

  /// what an input clip contains, set with "hostDemo -i Clip=spec"
  struct InputSpec {
    enum Type {
      eDefault, ///< the synthetic pattern: grey level t/OFXHOSTDEMOCLIPLENGTH, with the time and view drawn in the reverse colour
      eSolid,   ///< "solid:r,g,b,a": a constant colour
      eRamp,    ///< "ramp": r=g=b=a go from 0 (left) to 1 (right of the RoD)
      eChecker, ///< "checker:N": N×N pixel (at render scale 1) squares, alternating 0 and 1 (r=g=b=a)
      eFile,    ///< "file:path.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]": a PFM image, the same at all times (see PFMImage)
      eNone,    ///< "none": the clip is disconnected
      eRod      ///< "rod:x1,y1,x2,y2": transparent (0,0,0,0), with this RoD in canonical coordinates
                ///< (possibly empty): a placeholder for an input whose content doesn't matter
    };
    Type   type;
    double colour[4]; ///< for eSolid
    double rod[4];    ///< for eRod
    int    checkerSize; ///< for eChecker
    std::string path; ///< for eFile
    PFMImage    file; ///< for eFile: the content of the file, read by parseInputSpec
    /// for eFile: the pixel coordinates of the file's bottom-left pixel, at the render scale
    /// fileScale ("@x1,y1"; default 0,0). The file's pixels are clip pixels at that scale, so
    /// that the output of a render at that scale can be passed, with its bounds, as an input.
    int         fileX, fileY;
    /// for eFile: "+alpha.pfm", a Pf file of the same size with the alpha (else PF gives alpha 1)
    std::string alphaPath;
    PFMImage    alpha;
    /// for eFile: "#premult", "#unpremult" or "#opaque", the premultiplication state of the
    /// input (kOfxImagePreMultiplied...; empty: the default, unpremultiplied unless RGB)
    std::string premult;
    double      fileScaleX, fileScaleY; ///< for eFile: hostDemo --scale
    InputSpec() : type(eDefault), checkerSize(16), fileX(0), fileY(0), fileScaleX(1.), fileScaleY(1.)
    { colour[0] = colour[1] = colour[2] = colour[3] = 0.; rod[0] = rod[1] = rod[2] = rod[3] = 0.; }
  };

  /// parse "solid:r,g,b,a", "ramp", "checker:N", "file:path.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]",
  /// "none" or "default". Returns
  /// false if invalid, with the reason in error. "file:" reads the PFM file.
  bool parseInputSpec(const std::string& s, InputSpec& spec, std::string& error);

  /// an extra plane of an input clip (hostDemo -i Clip@Plane=spec, with --multiplane): one of the
  /// Nuke extension's planes other than the colour plane, which have two components
  struct InputPlane {
    std::string plane;      ///< kFnOfxImagePlaneForwardMotionVector, ...BackwardMotionVector, ...StereoDisparityLeft or ...Right
    std::string components; ///< its components: kFnOfxImageComponentMotionVectors or kFnOfxImageComponentStereoDisparity
    InputSpec   spec;       ///< its pixels: the first two components (r and g) of the spec's pattern
  };

  /// the plane of a short plane name of -i Clip@Plane=spec: ForwardMotionVector, BackMotionVector,
  /// StereoDisparityLeft or StereoDisparityRight (the plane names after "uk.co.thefoundry.OfxImagePlane").
  /// Returns false if it is not one of them.
  bool parseInputPlaneName(const std::string& name, InputPlane& plane);

  /// parse a finite decimal number (e.g. "0.5", "-2", "1e3"). Returns false for anything else,
  /// including nan, inf, hexadecimal and out-of-range (ERANGE) values, and surrounding spaces.
  bool parseFiniteDouble(const std::string& s, double& d);

  /// parse a decimal integer that fits in an int. Returns false for anything else.
  bool parseInteger(const std::string& s, int& i);

  /// intersection of two rectangles (may be empty, i.e. x2 <= x1 or y2 <= y1)
  OfxRectI intersect(const OfxRectI& a, const OfxRectI& b);

  /// the smallest rectangle in pixel coordinates enclosing a rectangle in canonical coordinates
  /// (coordinates beyond the int range, e.g. of an infinite RoD, are clamped to it)
  OfxRectI canonicalToPixelEnclosing(const OfxRectD& r, const OfxPointD& renderScale, double par);
  /// the largest image hostDemo makes, in pixels (MyImage refuses larger ones)
  const long long kMaxImagePixels = 1LL << 28;
  /// true if hostDemo can make an image with these bounds and bytes per pixel (at most
  /// kMaxImagePixels pixels, and rows of less than 2^31 bytes; the default is float RGBA)
  bool imageFits(const OfxRectI& bounds, int bytesPerPixel = 16);

  /// number of image fetches that failed because of an unsupported pixel depth or components
  /// (hostDemo then exits with a non-zero status)
  extern int gImageErrors;

  /// an image with the clip's (mapped) pixel depth and components, and the given bounds,
  /// region of definition and render scale.
  /// Input images are filled with the clip's InputSpec. Output images are filled with
  /// a sentinel (NaN for float, 0xCD bytes for byte and short), so that unwritten pixels show.
  class MyImage : public OFX::Host::ImageEffect::Image 
  {
  protected :
    unsigned char *_data; // where we are keeping our image data, allocated with new[]
    std::string    _depth;
    std::string    _components;
    int            _nComps;
    int            _bytesPerComp;
    int            _rowBytes;
    OfxRectI       _bounds;
  public :
    /// returns true if the depth and components are supported by MyImage
    static bool isSupported(const std::string& depth, const std::string& components);

    /// bounds and rod are in pixel coordinates (at the given render scale); plane: an extra
    /// plane of an input (NULL: the colour plane, with the clip's components)
    MyImage(MyClipInstance &clip, OfxTime t, int view,
            const OfxRectI& bounds, const OfxRectI& rod, const OfxPointD& renderScale,
            bool isOutput, const InputPlane* plane = NULL);
    ~MyImage();

    const std::string& getDepth() const { return _depth; }
    const std::string& getComponentsString() const { return _components; }
    const OfxRectI& getBounds() const { return _bounds; }
    int getNComponents() const { return _nComps; }
    int getBytesPerComponent() const { return _bytesPerComp; }

    /// the address of the pixel at (x,y), which must be inside the bounds
    const unsigned char* getPixelData(int x, int y) const
    {
      return _data + (size_t)(y - _bounds.y1) * _rowBytes + (size_t)(x - _bounds.x1) * _nComps * _bytesPerComp;
    }

    /// component c of the pixel at (x,y) (inside the bounds), in the native range of the depth
    /// (0..255, 0..65535, or the float value, NaN included)
    double getComponent(int x, int y, int c) const;

    /// component c of the pixel at (x,y) (inside the bounds), as a float: byte and short values
    /// are divided by 255 and 65535, float values are unchanged (NaN included)
    float getComponentFloat(int x, int y, int c) const;

    /// get the pixel at (x,y), converted to 8 bits RGB (Alpha images give a grey level).
    /// Returns false if (x,y) is outside the bounds. notWritten is set if the pixel still
    /// holds the NaN sentinel (float images only).
    bool getPixelRGB8(int x, int y, int rgb[3], bool& notWritten) const;
  };

  class MyClipInstance : public OFX::Host::ImageEffect::ClipInstance {
  protected:
    MyEffectInstance *_effect;
    std::string       _name;
    MyImage          *_outputImage; ///< only set for output clips, during a render
    InputSpec         _input; ///< what the input clip contains
    std::map<std::string, InputPlane> _planes; ///< its extra planes, by plane (-i Clip@Plane=spec)
    std::string       _unmappedDepth; ///< the depth of the input clip, before clip preferences
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    int               _view; ///< view used by the last renderAction
#endif
    mutable bool      _inRoDAction; ///< set while the output clip calls the effect's RoD action
    bool              _multiPlane; ///< hostDemo --multiplane (getComponentsPresent())

  public:
    MyClipInstance(MyEffectInstance* effect, OFX::Host::ImageEffect::ClipDescriptor* desc);

    virtual ~MyClipInstance();
    /// the output image fetched by the plug-in during the last render (NULL if it didn't fetch it)
    MyImage* getOutputImage() { return _outputImage; }

    /// release the output image: call this before each render, so that each render gets a new image
    void releaseOutputImage();

    void setInput(const InputSpec& input) { _input = input; }
    const InputSpec& getInput() const { return _input; }

    /// set the depth returned by getUnmappedBitDepth() (byte, short or float)
    void setUnmappedBitDepth(const std::string& depth) { _unmappedDepth = depth; }

    /// hostDemo --multiplane: kFnOfxImageEffectPropComponentsPresent lists the planes
    void setMultiPlane() { _multiPlane = true; }
    /// add an extra plane to the input (-i Clip@Plane=spec), replacing the one of that plane
    void setInputPlane(const InputPlane& plane);

    /// make the image for a fetch at the given time and view: the output image (shared by all fetches
    /// in a render) covers the render window, an input image covers optionalBounds (or the RoI if none
    /// was given, or the RoD outside of render) clipped to the RoD; plane: an extra plane of an input
    /// (NULL: the colour plane). Returns NULL if the clip is disconnected or the bounds are empty.
    MyImage* fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds, const InputPlane* plane = NULL);

    /// the RoD of the clip at the given time and view, in canonical coordinates: the PAL SD frame
    /// for a connected input (for a PFM file, its size in pixels at the clip's PAR), empty (0,0,0,0) for a disconnected one, and the result of the
    /// effect's getRegionOfDefinition action for the output clip (empty if the action fails)
    OfxRectD getRoD(OfxTime time, int view) const;

    /// the mapped pixel depth and components: kOfxBitDepthNone and kOfxImageComponentNone
    /// if the clip is disconnected ("implying a clip is unconnected")
    virtual const std::string &getPixelDepth() const;
    virtual const std::string &getComponents() const;

    /// Get the Raw Unmapped Pixel Depth from the host
    ///
    /// \returns
    ///    - kOfxBitDepthNone (implying a clip is unconnected image)
    ///    - kOfxBitDepthByte
    ///    - kOfxBitDepthShort
    ///    - kOfxBitDepthHalf
    ///    - kOfxBitDepthFloat
    const std::string &getUnmappedBitDepth() const;
    
    /// Get the Raw Unmapped Components from the host
    ///
    /// \returns
    ///     - kOfxImageComponentNone (implying a clip is unconnected, not valid for an image)
    ///     - kOfxImageComponentRGBA
    ///     - kOfxImageComponentAlpha
    virtual const std::string &getUnmappedComponents() const;

    // PreMultiplication -
    //
    //  kOfxImageOpaque - the image is opaque and so has no premultiplication state
    //  kOfxImagePreMultiplied - the image is premultiplied by it's alpha
    //  kOfxImageUnPreMultiplied - the image is unpremultiplied
    virtual const std::string &getPremult() const;

    // Pixel Aspect Ratio -
    //
    //  The pixel aspect ratio of a clip or image.
    virtual double getAspectRatio() const;

    // Frame Rate -
    //
    //  The frame rate of a clip or instance's project.
    virtual double getFrameRate() const;

    // Frame Range (startFrame, endFrame) -
    //
    //  The frame range over which a clip has images.
    virtual void getFrameRange(double &startFrame, double &endFrame) const ;

    /// Field Order - Which spatial field occurs temporally first in a frame.
    /// \returns 
    ///  - kOfxImageFieldNone - the clip material is unfielded
    ///  - kOfxImageFieldLower - the clip material is fielded, with image rows 0,2,4.... occuring first in a frame
    ///  - kOfxImageFieldUpper - the clip material is fielded, with image rows line 1,3,5.... occuring first in a frame
    virtual const std::string &getFieldOrder() const;

#ifdef OFX_EXTENSIONS_NATRON
    // Format -
    // The format of the clip or image (in pixel coordinates)
    virtual OfxRectI getFormat() const;
#endif

    // Connected -
    //
    //  Says whether the clip is actually connected at the moment.
    virtual bool getConnected() const;

    // Unmapped Frame Rate -
    //
    //  The unmaped frame range over which an output clip has images.
    virtual double getUnmappedFrameRate() const;

    // Unmapped Frame Range -
    //
    //  The unmaped frame range over which an output clip has images.
    virtual void getUnmappedFrameRange(double &unmappedStartFrame, double &unmappedEndFrame) const;

    // Continuous Samples -
    //
    //  0 if the images can only be sampled at discreet times (eg: the clip is a sequence of frames),
    //  1 if the images can only be sampled continuously (eg: the clip is infact an animating roto spline and can be rendered anywhen). 
    virtual bool getContinuousSamples() const;

    /// override this to fill in the image at the given time.
    /// The bounds of the image on the image plane should be 
    /// 'appropriate', typically the value returned in getRegionsOfInterest
    /// on the effect instance. Outside a render call, the optionalBounds should
    /// be 'appropriate' for the.
    /// If bounds is not null, fetch the indicated section of the canonical image plane.
    virtual OFX::Host::ImageEffect::Image* getImage(OfxTime time, const OfxRectD *optionalBounds);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /// override this to fill in the OpenGL texture at the given time.
    /// The bounds of the image on the image plane should be
    /// 'appropriate', typically the value returned in getRegionsOfInterest
    /// on the effect instance. Outside a render call, the optionalBounds should
    /// be 'appropriate' for the.
    /// If bounds is not null, fetch the indicated section of the canonical image plane.
    virtual OFX::Host::ImageEffect::Texture* loadTexture(OfxTime time, const char *format, const OfxRectD *optionalBounds) { return NULL; };
#endif

#ifdef OFX_EXTENSIONS_NUKE
    /// override this to fill in the given image plane at the given time.
    /// The bounds of the image on the image plane should be
    /// 'appropriate', typically the value returned in getRegionsOfInterest
    /// on the effect instance.
    /// Outside a render call, the optionalBounds should
    /// be 'appropriate' for the image.
    /// If bounds is not null, fetch the indicated section of the canonical image plane.
    ///
    /// This function implements both V1 of the image plane suite and V2. In the V1 the parameter view was not present and
    /// will be passed -1, indicating that you should on your own retrieve the correct index of the view at which the render called was issues
    /// by using thread local storage. In V2 the view index will be correctly set with a value >= 0.
    ///
    virtual OFX::Host::ImageEffect::Image* getImagePlane(OfxTime time, int view, const std::string& plane,const OfxRectD *optionalBounds);

    /// kFnOfxImageEffectPropComponentsPresent: with setMultiPlane(), the planes of the clip: the
    /// colour plane, then the extra planes of an input (else HostSupport's: the components)
    virtual const std::vector<std::string>& getComponentsPresent() const OFX_EXCEPTION_SPEC;

    /// override this to return the rod on the clip for the given view
    virtual OfxRectD getRegionOfDefinition(OfxTime time, int view) const;
#endif

#ifdef OFX_EXTENSIONS_VEGAS
    /// override this to fill in the image at the given time from a specific view
    /// (using the standard callback gets you the current view being rendered, @see getImage).
    /// The bounds of the image on the image plane should be
    /// 'appropriate', typically the value returned in getRegionsOfInterest
    /// on the effect instance. Outside a render call, the optionalBounds should
    /// be 'appropriate' for the.
    /// If bounds is not null, fetch the indicated section of the canonical image plane.
    virtual OFX::Host::ImageEffect::Image* getStereoscopicImage(OfxTime time, int view, const OfxRectD *optionalBounds);
#endif

#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    /// set the default view returned by getImage()
    virtual void setView(int view);
#endif

    /// override this to return the rod on the clip
    virtual OfxRectD getRegionOfDefinition(OfxTime time) const;

  };

}

#endif // HOST_DEMO_CLIP_INSTANCE_H
