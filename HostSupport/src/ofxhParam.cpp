/*
Software License :

Copyright (c) 2007-2009, The Open Effects Association Ltd. All rights reserved.

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

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParametricParam.h" // kOfxParamTypeParametric, a known type even without OFX_SUPPORTS_PARAMETRIC
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnPublicOfxExtensions.h"
#endif
#ifdef OFX_EXTENSIONS_NATRON
#include "ofxNatron.h"
#endif

// ofx host
#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhParam.h"
#include "ofxhImageEffect.h"
#include "ofxOld.h" // old plugins may rely on deprecated properties being present


#include <assert.h>
#include <float.h>
#include <limits> // numeric_limits (DBL_MAX is missing with some C++20 toolchains)
#include <limits.h>
#include <stdarg.h>
#include <new> // bad_alloc
#include <sstream> // stringstream

namespace OFX {

  namespace Host {

    namespace Param {

      //
      // Base
      //
      Base::Base(const std::string &name, const std::string& type) : 
        _paramName(name),
        _paramType(type)
      {
        assert(_paramType.c_str());
      }

      Base::Base(const std::string &name, const std::string &type, const Property::Set &properties) :
        _paramName(name),
        _paramType(type), 
        _properties(properties)
      {
        assert(_paramType.c_str());
      }


      Base::~Base() {}

      /// grab a handle on the parameter for passing to the C API
      OfxParamHandle Base::getHandle() const {
        return (OfxParamHandle)this;
      }

      /// grab a handle on the properties of this parameter for the C api
      OfxPropertySetHandle Base::getPropHandle() const {
        return _properties.getHandle();
      }

      Property::Set &Base::getProperties() {
        return _properties;
      }

      const Property::Set &Base::getProperties() const {
        return _properties;
      }

      const std::string &Base::getType() const {
        return _paramType;
      } 

      const std::string &Base::getName() const {
        return _paramName;
      }

      const std::string &Base::getParentName() const {
        return _properties.getStringProperty(kOfxParamPropParent);
      }

      const std::string &Base::getScriptName() const {
        return _properties.getStringProperty(kOfxParamPropScriptName);
      }

      const std::string &Base::getLabel() const {
        return _properties.getStringProperty(kOfxPropLabel);
      }

      const std::string &Base::getLongLabel() const {
        return _properties.getStringProperty(kOfxPropLongLabel);
      }

      const std::string &Base::getShortLabel() const {
        return _properties.getStringProperty(kOfxPropShortLabel);
      }

      const std::string &Base::getDoubleType() const {
        return _properties.getStringProperty(kOfxParamPropDoubleType, 0);
      }

      const std::string &Base::getDefaultCoordinateSystem() const {
        return _properties.getStringProperty(kOfxParamPropDefaultCoordinateSystem, 0);
      }

      const std::string &Base::getHint() const {
        return _properties.getStringProperty(kOfxParamPropHint, 0);
      }

#ifdef OFX_EXTENSIONS_NATRON
      // Is the hint string encoded in markdown instead of plain-text ?
      bool Base::isHintInMarkdown() const
      {
        return _properties.getIntProperty(kNatronOfxPropDescriptionIsMarkdown) != 0;
      }
#endif

      bool Base::getEnabled() const {
        return _properties.getIntProperty(kOfxParamPropEnabled, 0) != 0;
      }
      
      bool Base::getSecret() const {
        return _properties.getIntProperty(kOfxParamPropSecret, 0) != 0;
      }

      bool Base::getIsPersistent() const {
        return _properties.getIntProperty(kOfxParamPropPersistent, 0) != 0;
      }

      bool Base::getEvaluateOnChange() const {
        return _properties.getIntProperty(kOfxParamPropEvaluateOnChange, 0) != 0;
      }
      
      bool Base::getCanUndo() const {
        if (_properties.fetchProperty(kOfxParamPropCanUndo))  {
          return _properties.getIntProperty(kOfxParamPropCanUndo) != 0;
        }
        return false;
      }
      
      bool Base::getCanAnimate() const {
        if (_properties.fetchProperty(kOfxParamPropAnimates))  {
          return _properties.getIntProperty(kOfxParamPropAnimates) != 0;
        }
        return false;
      }

      //
      // Descriptor
      //

      struct TypeMap {
        const char *paramType;
        Property::TypeEnum propType;
        int propDimension;
      };

      static
      bool isDoubleParam(const std::string &paramType)
      {
        return paramType == kOfxParamTypeDouble ||
               paramType == kOfxParamTypeDouble2D ||
               paramType == kOfxParamTypeDouble3D
#ifdef OFX_SUPPORTS_PARAMETRIC
            || paramType == kOfxParamTypeParametric
#endif
          ;
      }

      bool isColourParam(const std::string &paramType)
      {
        return 
          paramType == kOfxParamTypeRGBA ||
          paramType == kOfxParamTypeRGB;
      }

      bool isIntParam(const std::string &paramType)
      {
        return paramType == kOfxParamTypeInteger ||
               paramType == kOfxParamTypeInteger2D ||
               paramType == kOfxParamTypeInteger3D;
      }

      static const TypeMap typeMap[] = {
        { kOfxParamTypeInteger,   Property::eInt,    1 },
        { kOfxParamTypeDouble,    Property::eDouble, 1 },
        { kOfxParamTypeBoolean,   Property::eInt,    1 },
        { kOfxParamTypeChoice,    Property::eInt,    1 },
        { kOfxParamTypeStrChoice, Property::eString, 1 },
        { kOfxParamTypeRGBA,      Property::eDouble, 4 },
        { kOfxParamTypeRGB,       Property::eDouble, 3 },
        { kOfxParamTypeDouble2D,  Property::eDouble, 2 },
        { kOfxParamTypeInteger2D, Property::eInt,    2 },
        { kOfxParamTypeDouble3D,  Property::eDouble, 3 },
        { kOfxParamTypeInteger3D, Property::eInt,    3 },
        { kOfxParamTypeString,    Property::eString, 1 },
        { kOfxParamTypeCustom,    Property::eString, 1 },
        { kOfxParamTypeBytes,     Property::ePointer, 1 }, // OFX 1.5: the value is an OfxBytes
        { kOfxParamTypeGroup,     Property::eNone,   0 },
        { kOfxParamTypePage,      Property::eNone,   0 },
        { kOfxParamTypePushButton,Property::eNone,   0 },
#ifdef OFX_SUPPORTS_PARAMETRIC
        { kOfxParamTypeParametric,Property::eDouble, 0 },
#endif
        { 0,                      Property::eNone,   0  }
      };
      
      /// is this a standard type
      bool isStandardType(const std::string &type)
      {
        const TypeMap *tm = typeMap;
        while (tm->paramType) {
          if (tm->paramType == type) 
            return true;
          tm++;
        }
        return false;
      }
  
      static
      bool findType(const std::string paramType, Property::TypeEnum &propType, int &propDim)
      {
        const TypeMap *tm = typeMap;
        while (tm->paramType) {
          if (tm->paramType == paramType) {
            propType = tm->propType;
            propDim = tm->propDimension;
            return true;
          }
          tm++;
        }
        return false;
      }

      /// make a parameter, with the given type and name
      Descriptor::Descriptor(const std::string &type, 
                             const std::string &name) : Base(name, type)
      {
        _bytesDefault.data = NULL;
        _bytesDefault.length = 0;
        const char *ctype = type.c_str();
        const char *cname = name.c_str();

        Property::PropSpec universalProps[] = {
          { kOfxPropType,    Property::eString,    1,    true,    kOfxTypeParameter },
          { kOfxParamPropSecret,  Property::eInt,    1,    false,    "0"},
          { kOfxParamPropHint,    Property::eString,    1,    false,    ""},
          { kOfxParamPropScriptName, Property::eString, 1, false, cname },
          { kOfxParamPropParent,  Property::eString,    1,    false,    "" },
          { kOfxParamPropEnabled, Property::eInt,    1,    false,    "1" },
          { kOfxParamPropDataPtr, Property::ePointer,    1,    false,    0 },
          { kOfxParamPropType,  Property::eString, 1, true,  ctype },
          { kOfxPropName,       Property::eString, 1, false, cname },
          { kOfxPropLabel,      Property::eString, 1, false, cname },
          { kOfxPropShortLabel, Property::eString, 1, false, cname },
          { kOfxPropLongLabel,  Property::eString, 1, false, cname },
          { kOfxPropIcon,       Property::eString, 2, false, "" },
#ifdef OFX_EXTENSIONS_NUKE
          { kOfxParamPropLayoutHint,  Property::eInt, 1, false, "0" },
          { kOfxParamPropLayoutPadWidth,  Property::eInt, 1, false, "0" },
#endif
#ifdef OFX_EXTENSIONS_NATRON
          { kNatronOfxParamPropIsInstanceSpecific, Property::eInt, 1, false, "0"},
          { kNatronOfxPropDescriptionIsMarkdown,            Property::eInt,     1, false, "0" },
          { kNatronOfxParamPropInViewerContextLayoutHint,            Property::eInt,     1, false, "0" },
          { kNatronOfxParamPropInViewerContextLayoutPadWidth,            Property::eInt,     1, false, "3" },
          { kNatronOfxParamPropInViewerContextLabel,            Property::eString,     1, false, "" },
          { kNatronOfxParamPropInViewerContextSecret,            Property::eInt,     1, false, "" },
#endif
          Property::propSpecEnd
        };
        
        _properties.addProperties(universalProps);
      }

      /// make a parameter, with the given type and name
      void Descriptor::addStandardParamProps(const std::string &type) 
      {
        Property::TypeEnum propType = Property::eString;
        int propDim = 1;
        findType(type, propType, propDim);


        static const Property::PropSpec allString[] = {
          { kOfxParamPropStringMode,  Property::eString,    1,    false,    kOfxParamStringIsSingleLine },
          { kOfxParamPropStringFilePathExists, Property::eInt,    1,    false,    "1" },
          Property::propSpecEnd
        };

        static const Property::PropSpec allChoice[] = {
          { kOfxParamPropChoiceOption,    Property::eString,    0,    false,    "" },
#ifdef OFX_EXTENSIONS_TUTTLE
          { kOfxParamPropChoiceLabelOption, Property::eString,  0,    false,    "" },
#endif
#ifdef OFX_EXTENSIONS_NATRON
          { kOfxParamPropChoiceEnum,                    Property::eString, 0, false,    "" },
          { kNatronOfxParamPropChoiceCascading,         Property::eInt, 1,    false,    "0"},
          { kNatronOfxParamPropChoiceHostCanAddOptions, Property::eInt, 1,    false,    "0"},
#endif
          // OFX 1.5: UI order of the options; "if not set, the host will present the
          // options in their natural order"
          { kOfxParamPropChoiceOrder,    Property::eInt,       0,    false,    "" },
          Property::propSpecEnd
        };

        static const Property::PropSpec allStrChoice[] = {
          { kOfxParamPropChoiceEnum,      Property::eString,    0,    false,    "" },
          { kOfxParamPropChoiceOption,    Property::eString,    0,    false,    "" },
          // OFX 1.5: "StrChoice parameters may also be reordered using kOfxParamPropChoiceOrder"
          { kOfxParamPropChoiceOrder,     Property::eInt,       0,    false,    "" },
          Property::propSpecEnd
        };

        static const Property::PropSpec allCustom[] = {
          { kOfxParamPropCustomInterpCallbackV1,    Property::ePointer,    1,    false,    0 },
          Property::propSpecEnd
        };

        static const Property::PropSpec allPage[] = {
          { kOfxParamPropPageChild,    Property::eString,    0,    false,    "" },
#         ifdef OFX_EXTENSIONS_NATRON
          { kNatronOfxParamPropInViewerContextIsInToolbar, Property::eInt, 1, false, "0" },
#         endif
          Property::propSpecEnd
        };

        static const Property::PropSpec allGroup[] = {
          { kOfxParamPropGroupOpen, Property::eInt, 1, false, "1" },
#         ifdef OFX_EXTENSIONS_NUKE
          { kFnOfxParamPropGroupIsTab, Property::eInt, 1, false, "0" },
#         endif
#         ifdef OFX_EXTENSIONS_NATRON
          { kNatronOfxGroupParamPropIsDialog, Property::eInt, 1, false, "0" },
          { kNatronOfxParamPropInViewerContextIsInToolbar, Property::eInt, 1, false, "0" },
          { kNatronOfxParamPropInViewerContextCanHaveShortcut, Property::eInt, 1, false, "0" },
#         endif
          Property::propSpecEnd
        };

#       ifdef OFX_SUPPORTS_PARAMETRIC
        static const Property::PropSpec allParametric[] = {
          { kOfxParamPropParametricDimension,         Property::eInt,     1,  false, "1" },
          { kOfxParamPropParametricUIColour,          Property::eDouble,  0,  false, ""  },
          { kOfxParamPropParametricInteractBackground,Property::ePointer, 1,  false, 0   },
          { kOfxParamPropParametricRange,             Property::eDouble,  2,  false, "0" },
#        ifdef OFX_EXTENSIONS_NATRON
          { kNatronOfxParamPropParametricIsPeriodic,  Property::eInt,     1,  false, "0" },
#        endif
#        ifdef OFX_SUPPORTS_PARAMETRIC_V2
          { kOfxParamParametricInterpolationCustomInterpCallbackV1, Property::ePointer, 1,  false, 0  },
#        endif
          Property::propSpecEnd
        };
#       endif
               
        if (propType != Property::eNone) {
          addValueParamProps(type, propType, propDim);
        }

        if (type == kOfxParamTypeString) {
          _properties.addProperties(allString);
        }
  
        if (isDoubleParam(type) || isIntParam(type) || isColourParam(type)
#         ifdef OFX_SUPPORTS_PARAMETRIC
            || type == kOfxParamTypeParametric // although not explicitely stated in the OFX 1.4 spec, it seems logical
#         endif
            ) {
          addNumericParamProps(type, propType, propDim);
        }

        if (type != kOfxParamTypeGroup && type != kOfxParamTypePage) {
          addInteractParamProps(type);
        }

        if (type == kOfxParamTypeChoice) {
          _properties.addProperties(allChoice);
        }

        if (type == kOfxParamTypeStrChoice) {
          _properties.addProperties(allStrChoice);
        }

        if (type == kOfxParamTypeCustom) {
          _properties.addProperties(allCustom);
        }

        if (type == kOfxParamTypePage) {
          _properties.addProperties(allPage);
        }

        if (type == kOfxParamTypeGroup) {
          _properties.addProperties(allGroup);
        }

#       ifdef OFX_SUPPORTS_PARAMETRIC
        if (type == kOfxParamTypeParametric) {
          _properties.addProperties(allParametric);
          double range[2] = {0., 1.};
          _properties.setDoublePropertyN(kOfxParamPropParametricRange, range, 2);
        }
#       endif

#       ifdef OFX_EXTENSIONS_NATRON

        if (type == kOfxParamTypePushButton) {
          static const Property::PropSpec allPushButton[] = {
            { kNatronOfxParamPropInViewerContextCanHaveShortcut, Property::eInt, 1, false, "0" },
            Property::propSpecEnd
          };
          _properties.addProperties(allPushButton);
        } else if (type == kOfxParamTypeBoolean) {
          static const Property::PropSpec allBoolean[] = {
            { kNatronOfxParamPropInViewerContextIsInToolbar, Property::eInt, 1, false, "0" },
            { kNatronOfxParamPropInViewerContextCanHaveShortcut, Property::eInt, 1, false, "0" },
            { kNatronOfxBooleanParamPropIsToggableButton, Property::eInt, 1, false, "0" },
            Property::propSpecEnd
          };
          _properties.addProperties(allBoolean);
        }

#       endif // OFX_EXTENSIONS_NATRON
      }

      /// add standard properties to a params that can take an interact
      void Descriptor::addInteractParamProps(const std::string &/*type*/)
      {
        static const Property::PropSpec allButGroupPageProps[] = {
          { kOfxParamPropInteractV1,          Property::ePointer, 1, false, 0 },
          { kOfxParamPropInteractSize,        Property::eDouble,  2, false, "0" },
          { kOfxParamPropInteractSizeAspect,  Property::eDouble,  1, false, "1" },
          { kOfxParamPropInteractMinimumSize, Property::eDouble,  2, false, "10" },
          { kOfxParamPropInteractPreferedSize,Property::eInt,     2, false, "10" },
          // OFX 1.2 (ParamsAllButGroupPage): the host sets HasHostOverlayHandle (read only for the
          // plug-in), the plug-in sets UseHostOverlayHandle
          { kOfxParamPropHasHostOverlayHandle, Property::eInt,    1, true,  "0" },
          { kOfxParamPropUseHostOverlayHandle, Property::eInt,    1, false, "0" },
          Property::propSpecEnd
        };

        
        _properties.addProperties(allButGroupPageProps);
      }

      /// add standard properties to a value holding param
      void Descriptor::addValueParamProps(const std::string &type, Property::TypeEnum valueType, int dim)
      {
        static const Property::PropSpec invariantProps[] = {
          { kOfxParamPropIsAnimating, Property::eInt, 1,       false, "0" },
          { kOfxParamPropIsAutoKeying,Property::eInt, 1,       false, "0" },
          { kOfxParamPropPersistent,  Property::eInt, 1,       false, "1" },
          { kOfxParamPropEvaluateOnChange, Property::eInt, 1,  false, "1" },
#       ifdef kOfxParamPropPluginMayWrite
          { kOfxParamPropPluginMayWrite,    Property::eInt,    1,    false,    "0" }, // removed in OFX 1.4
#       endif
          { kOfxParamPropCanUndo,    Property::eInt,    1,    false,    "1" },
          { kOfxParamPropCacheInvalidation,    Property::eString,    1,    false,    kOfxParamInvalidateValueChange },
          Property::propSpecEnd
        };

        /// http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#ParametersAnimation
        /// The following may animate, depending on the host.
        /// Properties exist on the host to check this. If the host does support animation on them, then they do _not_ animate by default.
        /// They are...
        /// - kOfxParamTypeCustom
        /// - kOfxParamTypeString
        /// - kOfxParamTypeBoolean
        /// - kOfxParamTypeChoice
        /// If host doesn't support animation on them, then setting kOfxParamPropIsAnimating to 0 or 1 doesn't matter
        /// so just set the kOfxParamPropIsAnimating property to 0 for all those "extra animating" params.
        bool animates = type != kOfxParamTypeCustom && type != kOfxParamTypeString && type != kOfxParamTypeBoolean && type != kOfxParamTypeChoice;
        animates = animates && type != kOfxParamTypeStrChoice;
        // OFX 1.5 Bytes params: opaque plug-in data, not animated unless the plug-in asks for it
        animates = animates && type != kOfxParamTypeBytes;

        Property::PropSpec variantProps[] = {
          { kOfxParamPropAnimates,    Property::eInt, 1,       false, animates ? "1" : "0" },
          // a Pointer property's default is the pointer itself (see Set::createProperty), so use NULL:
          // the default of a Bytes param is "pointer to OfxBytes struct of one dimension, or nullptr"
          { kOfxParamPropDefault,     valueType,               dim, false, valueType == Property::eString ? "" : (valueType == Property::ePointer ? NULL : "0") },
          Property::propSpecEnd
        };

        _properties.addProperties(invariantProps);
        _properties.addProperties(variantProps);

        if (type == kOfxParamTypeBytes && !_bytesDefaultHook.get()) { // install the hook once
          _bytesDefaultHook.reset(new BytesDefaultHook(*this));
          _properties.addNotifyHook(kOfxParamPropDefault, _bytesDefaultHook.get());
        }
      }

      /// copy the data of a Bytes default (kOfxParamPropDefault, "pointer to OfxBytes struct of one
      /// dimension, or nullptr") into storage; used by Descriptor::BytesDefaultHook and BytesInstance.
      /// Returns kOfxStatOK, or kOfxStatErrValue ({NULL, length > 0}, as BytesInstance::setV) or
      /// kOfxStatErrMemory (the copy failed, e.g. bad_alloc or length_error for a garbage length),
      /// with an empty storage. Alias-safe: def may point into storage (the host's own copy).
      /// Never throws.
      static OfxStatus copyBytesDefault(const OfxBytes *def, std::vector<unsigned char> &storage)
      {
        OfxStatus stat = kOfxStatOK;
        try {
          if (def && !def->data && def->length > 0) {
            stat = kOfxStatErrValue; // no data, but a length
          } else if (def && def->data && def->length > 0) {
            std::vector<unsigned char> copy(def->data, def->data + def->length);
            storage.swap(copy);
          } else {
            storage.clear();
          }
        } catch (...) {
          stat = kOfxStatErrMemory;
        }
        if (stat != kOfxStatOK) {
          storage.clear();
        }
        return stat;
      }

      /// copy the plug-in's default of a Bytes param, and point the property to the copy
      void Descriptor::BytesDefaultHook::notify(const std::string &/*name*/, bool /*single*/, int /*num*/) OFX_EXCEPTION_SPEC
      {
        if (_copying) {
          return; // the property is being pointed to the copy
        }
        Property::Set &props = _descriptor.getProperties();
        const OfxBytes *def = (const OfxBytes *)props.getPointerProperty(kOfxParamPropDefault);
        if (def == &_descriptor._bytesDefault) {
          return; // already the copy
        }
        _copying = true;
        const OfxStatus stat = copyBytesDefault(def, _descriptor._bytesDefaultData);
        if (stat != kOfxStatOK) {
          // never keep the plug-in's pointer, which may be freed: the default becomes NULL
          def = NULL;
        }
        _descriptor._bytesDefault.data = _descriptor._bytesDefaultData.empty() ? NULL : &_descriptor._bytesDefaultData[0];
        _descriptor._bytesDefault.length = _descriptor._bytesDefaultData.size();
        // a NULL default stays NULL ("pointer to OfxBytes struct of one dimension, or nullptr")
        try {
          props.setPointerProperty(kOfxParamPropDefault, def ? (void*)&_descriptor._bytesDefault : NULL);
        } catch (...) {
          _copying = false;
          throw Property::Exception(kOfxStatFailed);
        }
        _copying = false;
        if (stat != kOfxStatOK) {
          throw Property::Exception(stat);
        }
      }

      /// add standard properties to a value holding param
      void Descriptor::addNumericParamProps(const std::string &type, Property::TypeEnum valueType, int dim)
      {
        static std::string dbl_minstr, dbl_maxstr, int_minstr, int_maxstr;
        static bool doneOne = false;

        if(!doneOne) {
          std::ostringstream dbl_min, dbl_max, int_min, int_max;
          doneOne = true;
          dbl_min << -std::numeric_limits<double>::max(); // not a bug, @see kOfxParamPropDisplayMin
          dbl_max << std::numeric_limits<double>::max();
          int_min << INT_MIN;
          int_max << INT_MAX;
          
          dbl_minstr = dbl_min.str();
          dbl_maxstr = dbl_max.str();
          int_minstr = int_min.str();
          int_maxstr = int_max.str();
        }
          
        Property::PropSpec allNumeric[] = {
          { kOfxParamPropDisplayMin, valueType, dim, false, isColourParam(type) ? "0" : (valueType == Property::eDouble ? dbl_minstr : int_minstr).c_str() },
          { kOfxParamPropDisplayMax, valueType, dim, false, isColourParam(type) ? "1" : (valueType == Property::eDouble ? dbl_maxstr : int_maxstr).c_str() },
          { kOfxParamPropMin, valueType, dim, false, (valueType == Property::eDouble ? dbl_minstr : int_minstr).c_str() },
          { kOfxParamPropMax, valueType, dim, false, (valueType == Property::eDouble ? dbl_maxstr : int_maxstr).c_str() },
          Property::propSpecEnd
        };

        _properties.addProperties(allNumeric);

        /// default interpolation of 1D integer and double params (kOfxParamInterpType: not in OFX
        /// 1.5 or 1.5.1; upstream main c91cce7b, for the next release)
        /// ("Default - kOfxParamInterpTypeLinear")
        if (dim == 1 && (isDoubleParam(type) || isIntParam(type))) {
          static const Property::PropSpec all1DInterp[] = {
            { kOfxParamInterpType, Property::eString, 1, false, kOfxParamInterpTypeLinear },
            Property::propSpecEnd
          };
          _properties.addProperties(all1DInterp);
        }

        /// if any double or a colour
        if (valueType == Property::eDouble) {
          static const Property::PropSpec allDouble[] = {
            { kOfxParamPropIncrement,  Property::eDouble,    1,    false,    "1" },
            { kOfxParamPropDigits,     Property::eInt,       1,    false,    "2" },
            Property::propSpecEnd
          };
          _properties.addProperties(allDouble);
        }

        /// if a double param type
        if(isDoubleParam(type)) {
          static const Property::PropSpec allDouble[] = {
            { kOfxParamPropDoubleType, Property::eString,    1,    false,    kOfxParamDoubleTypePlain },
            { kOfxParamPropDefaultCoordinateSystem, Property::eString,    1,    false,    kOfxParamCoordinatesCanonical },
            Property::propSpecEnd
          };
          _properties.addProperties(allDouble);
          
          if(dim == 1) {
            static const Property::PropSpec allDouble1D[] = {
              { kOfxParamPropShowTimeMarker, Property::eInt,   1,    false,    "0" },    
              Property::propSpecEnd
            };
            
            _properties.addProperties(allDouble1D);
          }
#ifdef OFX_EXTENSIONS_NATRON
          if (dim == 3) {
            static const Property::PropSpec matrixType[] = {
              { kNatronOfxParamPropDoubleTypeMatrix3x3,  Property::eInt, 1, false, "0" },
              Property::propSpecEnd
            };
            _properties.addProperties(matrixType);
          }
#endif
        }

        /// if a multi dimensional param
        if ((isDoubleParam(type) || isIntParam(type)) && (dim == 2 || dim == 3
#ifdef OFX_SUPPORTS_PARAMETRIC
                                    || dim == 0
#endif
                                    )) {
          // not static: depends on dim, which varies between param types
          const Property::PropSpec all2D3D[] = {
            { kOfxParamPropDimensionLabel,  Property::eString, dim, false, "" },
            Property::propSpecEnd
          };  

#ifdef OFX_EXTENSIONS_NATRON
          if (dim == 2) {
            static const Property::PropSpec rectangleType[] = {
              { kNatronOfxParamPropTypeRectangle,  Property::eInt, 1, false, "0" },
              Property::propSpecEnd
            };
            _properties.addProperties(rectangleType);
          }
#endif
          
          _properties.addProperties(all2D3D);
          _properties.setStringProperty(kOfxParamPropDimensionLabel, "x", 0);
          _properties.setStringProperty(kOfxParamPropDimensionLabel, "y", 1);
          if (dim == 3) {
            _properties.setStringProperty(kOfxParamPropDimensionLabel, "z", 2);
          }
        }

        /// if a multi dimensional param
        if (isColourParam(type)) {
          // not static: depends on dim, which varies between param types
          const Property::PropSpec allColor[] = {
            { kOfxParamPropDimensionLabel,  Property::eString, dim, false, "" },
            Property::propSpecEnd
          };  

          _properties.addProperties(allColor);
          _properties.setStringProperty(kOfxParamPropDimensionLabel, "r", 0);
          _properties.setStringProperty(kOfxParamPropDimensionLabel, "g", 1);
          _properties.setStringProperty(kOfxParamPropDimensionLabel, "b", 2);
          if (dim == 4) {
            _properties.setStringProperty(kOfxParamPropDimensionLabel, "a", 3);
          }

          /// kOfxParamPropColourManagement (next OFX release, after 1.5.1): "A host that supports
          /// this property MUST accept all three values" (kOfxParamColourManagementNone, Managed
          /// and SRGB; like the other string properties, the value is not checked), and "It
          /// defaults to ::kOfxParamColourManagementNone, so existing plug-ins are unaffected."
          /// "The property is ignored on parameter types other than RGB and RGBA": it is only on
          /// these. Hosts that convert colours read it from the param descriptor or instance.
          static const Property::PropSpec colourManagement[] = {
            { kOfxParamPropColourManagement, Property::eString, 1, false, kOfxParamColourManagementNone },
            Property::propSpecEnd
          };
          _properties.addProperties(colourManagement);
        }
      }

      BaseSet::~BaseSet() {}
      
      /// obtain a handle on this set for passing to the C api
      SetDescriptor::SetDescriptor()
      {
      }

       /// obtain a handle on this set for passing to the C api
      OfxParamSetHandle BaseSet::getParamSetHandle() const
      {
        return (OfxParamSetHandle)this;
      }

      SetDescriptor::~SetDescriptor()
      {
        // iterate the params and delete them
        std::list<Descriptor *>::iterator i;
        for(i = _paramList.begin(); i != _paramList.end(); ++i) {
          if(*i)
            delete (*i);
        }
      }

      const std::map<std::string, Descriptor*> &SetDescriptor::getParams() const
      {
        return _paramMap;
      }

      const std::list<Descriptor*> &SetDescriptor::getParamList() const
      {
        return _paramList;
      }

      void SetDescriptor::addParam(const std::string &name, Descriptor *p) {
        _paramList.push_back(p);
        _paramMap[name] = p;
      }

      /// define a param on this effect
      Descriptor *SetDescriptor::paramDefine(const char *paramType,
                                             const char *name)
      {
        if(!isStandardType(paramType)) 
          return NULL; /// << EEK! This is bad.

        Descriptor *desc = new Descriptor(paramType, name); 
        desc->addStandardParamProps(paramType);
        addParam(name, desc);
        return desc;
      }

      ////////////////////////////////////////////////////////////////////////////////
      //
      // Instance
      //

      /// the description of a plugin parameter
      Instance::~Instance() {}

      /// The parameter properties that OFX 1.5.1 ofxParam.h and ofxParametricParam.h document as
      /// "plugin parameter descriptor (read/write) and instance (read only)", or "plugin parameter
      /// instance (read only)" (kOfxParamPropInteractSize, kOfxParamPropIsAnimating,
      /// kOfxParamPropIsAutoKeying, which the host sets). 1.5.1 is the authority: upstream main
      /// dropped the per-property "Property Set" lines. An instance copies them from its
      /// descriptor, where the plug-in may write them, and they are read-only for the plug-in on
      /// the instance. What a plug-in write through the property suite does is decided by
      /// Property::PluginReadOnlyPolicyEnum (refused with kOfxStatErrValue by default); the host's
      /// C++ setters are not restricted. kOfxParamPropType is not listed: it is read-only on the
      /// descriptor too. Two more properties are documented the same way in this fork's headers:
      /// kOfxParamInterpType (not in OFX 1.5 or 1.5.1; upstream main c91cce7b, for the next
      /// release) and kOfxParamParametricInterpolationCustomInterpCallbackV1 (Natron's parametric
      /// suite V2, only defined with OFX_SUPPORTS_PARAMETRIC_V2). kOfxParamPropColourManagement
      /// (next release) has no "Property Set" line, but the spec says "A plug-in MUST NOT change
      /// it on a parameter instance".
      /// kOfxParamPropGroupOpen is also "descriptor (read/write) and instance (read only)", but it
      /// stays writable on instances: the Natron toolbar and dialog extensions (ofxNatron.h,
      /// extensions 4 and 9) let plug-ins open or close a group instance, e.g. openfx-misc's
      /// Shadertoy with the Support library's OFX::GroupParam::setOpen(); the host is told through
      /// the notify hook, GroupInstance::setOpen().
      static const char* const gInstanceReadOnlyProps[] = {
        kOfxParamPropInteractV1,
        kOfxParamPropInteractSize,
        kOfxParamPropInteractSizeAspect,
        kOfxParamPropInteractMinimumSize,
        kOfxParamPropInteractPreferedSize,
        kOfxParamPropAnimates,
        kOfxParamPropCanUndo,
        kOfxParamPropIsAnimating,
        kOfxParamPropPluginMayWrite,
        kOfxParamPropPersistent,
        kOfxParamPropScriptName,
        kOfxParamPropCacheInvalidation,
        kOfxParamPropDoubleType,
        kOfxParamPropUseHostOverlayHandle,
        kOfxPluginPropParamPageOrder,
        kOfxParamPropPageChild,
        kOfxParamPropParent,
        kOfxParamPropDimensionLabel,
        kOfxParamPropIsAutoKeying,
        kOfxParamPropCustomInterpCallbackV1,
        kOfxParamPropStringMode,
        kOfxParamPropStringFilePathExists,
        kOfxParamPropParametricDimension,
        kOfxParamPropParametricUIColour,
        kOfxParamPropParametricInteractBackground,
        kOfxParamPropParametricRange,
        kOfxParamInterpType,
        kOfxParamPropColourManagement,
#ifdef OFX_SUPPORTS_PARAMETRIC_V2
        kOfxParamParametricInterpolationCustomInterpCallbackV1,
#endif
        NULL
      };

      /// make a parameter, with the given type and name
      Instance::Instance(Descriptor& descriptor, Param::SetInstance* paramSet) 
        : Base(descriptor.getName(), descriptor.getType(), descriptor.getProperties())
        , _paramSetInstance(paramSet)
        , _parentInstance(NULL)
      {
        for (int i = 0; gInstanceReadOnlyProps[i]; ++i) {
          if (Property::Property *prop = _properties.fetchProperty(gInstanceReadOnlyProps[i])) {
            prop->setPluginReadOnly(true);
          }
        }
        _properties.addNotifyHook(kOfxParamPropEnabled, this);
        _properties.addNotifyHook(kOfxParamPropSecret, this);
        _properties.addNotifyHook(kOfxPropLabel, this);
        _properties.addNotifyHook(kOfxParamPropDefault, this);
        _properties.addNotifyHook(kOfxParamPropHint, this);
        _properties.addNotifyHook(kOfxParamPropMin, this);
        _properties.addNotifyHook(kOfxParamPropMax, this);
        _properties.addNotifyHook(kOfxParamPropDisplayMin, this);
        _properties.addNotifyHook(kOfxParamPropDisplayMax, this);
        _properties.addNotifyHook(kOfxParamPropEvaluateOnChange, this);

#ifdef OFX_EXTENSIONS_NATRON
        if (paramSet && paramSet->isInViewportParam(descriptor.getName())) {
          _properties.addNotifyHook(kNatronOfxParamPropInViewerContextLabel, this);
          _properties.addNotifyHook(kNatronOfxParamPropInViewerContextSecret, this);
        }
#endif
      }

      // callback which should set enabled state as appropriate
      void Instance::setEnabled()
      {
      }

      // callback which should set secret state as appropriate
      void Instance::setSecret()
      {
      }
      
      // callback which should update label
      void Instance::setLabel()
      {
      }
      
      // callback which should update hint
      void Instance::setHint()
      {
      }
      
      /// callback which should set default value
      void Instance::setDefault()
      {
      }

      /// callback which should set range
      void Instance::setRange()
      {
      }
      
      /// callback which should set display range
      void Instance::setDisplayRange()
      {
      }
      
      /// callback which should set evaluate on change
      void Instance::setEvaluateOnChange()
      {
      }

#ifdef OFX_EXTENSIONS_NATRON
      /// callback which should set secret state in the viewport as appropriate
      void Instance::setInViewportSecret()
      {
      }

      /// callback which should update the label on the viewport
      void Instance::setInViewportLabel()
      {
      }

#endif

      /// get a value, implemented by instances to deconstruct var args
      OfxStatus Instance::getV(va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }
      
      /// get a value, implemented by instances to deconstruct var args
      OfxStatus Instance::getV(OfxTime /*time*/, va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }

      /// set a value, implemented by instances to deconstruct var args
      OfxStatus Instance::setV(va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }

      /// key a value, implemented by instances to deconstruct var args
      OfxStatus Instance::setV(OfxTime /*time*/, va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }

      /// derive a value, implemented by instances to deconstruct var args
      OfxStatus Instance::deriveV(OfxTime /*time*/, va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }

      /// integrate a value, implemented by instances to deconstruct var args
      OfxStatus Instance::integrateV(OfxTime /*time1*/, OfxTime /*time2*/, va_list /*arg*/)
      {
        return kOfxStatErrUnsupported;
      }

      /// overridden from Property::NotifyHook
      void Instance::notify(const std::string &name, bool /*single*/, int /*num*/) OFX_EXCEPTION_SPEC
      {
        if (name == kOfxPropLabel) {
          setLabel();
        }
        else if (name == kOfxParamPropHint) {
          setHint();
        }
        else if (name == kOfxParamPropEnabled) {
          setEnabled();
        }
        else if (name == kOfxParamPropSecret) {
          setSecret();
        }
        else if (name == kOfxParamPropDefault) {
          setDefault();
        }
        else if (name == kOfxParamPropMin || name == kOfxParamPropMax) {
          setRange();
        }
        else if (name == kOfxParamPropDisplayMin || name == kOfxParamPropDisplayMax) {
          setDisplayRange();
        }
        else if (name == kOfxParamPropEvaluateOnChange) {
          setEvaluateOnChange();
        }
#ifdef OFX_EXTENSIONS_NATRON
        else if (name == kNatronOfxParamPropInViewerContextLabel) {
          setInViewportLabel();
        }
        else if (name == kNatronOfxParamPropInViewerContextSecret) {
          setInViewportSecret();
        }
#endif
      }

      // copy one parameter to another, with a range (NULL means to copy all animation)
      OfxStatus Instance::copyFrom(const Instance &/*instance*/, OfxTime /*offset*/, const OfxRangeD* /*range*/) {
        return kOfxStatErrMissingHostFeature; 
      }

      void Instance::setParentInstance(Instance* instance){
        _parentInstance = instance;
      }

      Instance* Instance::getParentInstance(){
        return _parentInstance;
      }

      //
      // KeyframeParam
      // 

      // The defaults are those of a parameter without keys (a host that doesn't animate
      // parameters), with the statuses ofxParam.h allows: paramGetNumKeys and paramDeleteAllKeys
      // only return kOfxStatOK or kOfxStatErrBadHandle, so a plug-in may treat any other status as
      // a failure.
      OfxStatus KeyframeParam::getNumKeys(unsigned int &nKeys) const {
        nKeys = 0;
        return kOfxStatOK;
      }

      OfxStatus KeyframeParam::getKeyTime(int /*nth*/, OfxTime& /*time*/) const {
        return kOfxStatErrBadIndex; // "the nthKey does not exist"
      }

      OfxStatus KeyframeParam::getKeyIndex(OfxTime /*time*/, int /*direction*/, int &index) const {
        index = -1; // "set to -1 if no key was found"
        return kOfxStatFailed; // "the search failed to find a key"
      }

      OfxStatus KeyframeParam::deleteKey(OfxTime /*time*/) {
        return kOfxStatErrBadIndex; // "no key at the given time"
      }

      OfxStatus KeyframeParam::deleteAllKeys() {
        return kOfxStatOK; // (no keys)
      }

      //
      // GroupInstance
      //

      /// make a parameter, with the given type and name
      GroupInstance::GroupInstance(Descriptor& descriptor, Param::SetInstance* instance)
        : Instance(descriptor,instance)
      {
        _properties.addNotifyHook(kOfxParamPropGroupOpen, this);
      }

      void GroupInstance::setChildren(std::vector<Param::Instance*> children)
      {
        _children = children;
        for (std::vector<Param::Instance*>::iterator it=children.begin(); it!=children.end(); ++it) {
          if(*it){
            (*it)->setParentInstance(this);
          }
        }
      }
      
      const std::vector<Param::Instance*> &GroupInstance::getChildren() const
      {
        return _children;
      }

      /// callback which should set open state
      void GroupInstance::setOpen()
      {
      }
      
      /// overridden from Instance
      void GroupInstance::notify(const std::string &name, bool single, int num) OFX_EXCEPTION_SPEC
      {
        Instance::notify(name, single, num);
        if (name == kOfxParamPropGroupOpen) {
          setOpen();
        }
      }

      //
      // Page Instance
      //

      const std::map<int,Param::Instance*> &PageInstance::getChildren() const 
      {
        // HACK!!!! this really should be done with a notify hook so we don't force
        // _children to be mutable
        if(_children.size() == 0 )
        {
          int nChildren = _properties.getDimension(kOfxParamPropPageChild);
          for(int i=0;i<nChildren;i++)
          {
            std::string childName = _properties.getStringProperty(kOfxParamPropPageChild,i);        
            Param::Instance* child = _paramSetInstance->getParam(childName);
            if(child)
              _children[i]=child;
          }
        }
        return _children;
      }

      //
      // ChoiceInstance
      //

      /// make a parameter, with the given type and name
      ChoiceInstance::ChoiceInstance(Descriptor& descriptor, Param::SetInstance* instance)
        : Instance(descriptor,instance)
      {
        _properties.addNotifyHook(kOfxParamPropChoiceOption, this);
      }

      // callback which should set option as appropriate
      void ChoiceInstance::setOption(int /*num*/)
      {
      }

      /// implementation of var args function
      OfxStatus ChoiceInstance::getV(va_list arg)
      {
        int *value = va_arg(arg, int*);
        OfxStatus stat = get(*value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus ChoiceInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value = va_arg(arg, int*);
        OfxStatus stat = get(time, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus ChoiceInstance::setV(va_list arg)
      {
        int value = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus ChoiceInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int value = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }

      /// overridden from Instance
      void ChoiceInstance::notify(const std::string &name, bool single, int num) OFX_EXCEPTION_SPEC
      {
        Instance::notify(name, single, num);
        if (name == kOfxParamPropChoiceOption) {
          setOption(num);
        }
      }

      //
      // StrChoiceInstance
      //

      /// make a parameter, with the given type and name
      StrChoiceInstance::StrChoiceInstance(Descriptor& descriptor, Param::SetInstance* instance)
        : Instance(descriptor,instance)
      {
        _properties.addNotifyHook(kOfxParamPropChoiceOption, this);
        _properties.addNotifyHook(kOfxParamPropChoiceEnum, this);
      }

      std::string StrChoiceInstance::getDefaultEnum() const
      {
        const std::string &def = getProperties().getStringProperty(kOfxParamPropDefault);
        if (!def.empty() || getProperties().getDimension(kOfxParamPropChoiceEnum) == 0) {
          return def;
        }
        return getProperties().getStringProperty(kOfxParamPropChoiceEnum, 0);
      }

      // callback which should set option as appropriate
      void StrChoiceInstance::setOption(int /*num*/)
      {
      }

      /// implementation of var args function
      OfxStatus StrChoiceInstance::getV(va_list arg)
      {
        const char **value = va_arg(arg, const char **);

        OfxStatus stat = get(_returnValue); // I so don't like this, temp storage should be delegated to the implementation
        *value = _returnValue.c_str();
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }

      /// implementation of var args function
      OfxStatus StrChoiceInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        const char **value = va_arg(arg, const char **);

        OfxStatus stat = get(time, _returnValue); // I so don't like this, temp storage should be delegated to the implementation
        *value = _returnValue.c_str();
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }

      /// implementation of var args function
      OfxStatus StrChoiceInstance::setV(va_list arg)
      {
        char *value = va_arg(arg, char*);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus StrChoiceInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        char *value = va_arg(arg, char*);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }

      /// overridden from Instance
      void StrChoiceInstance::notify(const std::string &name, bool single, int num) OFX_EXCEPTION_SPEC
      {
        Instance::notify(name, single, num);
        // options and enums are parallel arrays: notify the host if either changes
        if (name == kOfxParamPropChoiceOption || name == kOfxParamPropChoiceEnum) {
          setOption(num);
        }
      }

      //
      // IntegerInstance
      //
      OfxStatus IntegerInstance::derive(OfxTime /*time*/, int&) {
        return kOfxStatErrUnsupported; 
      }

      OfxStatus IntegerInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, int&) {
        return kOfxStatErrUnsupported; 
      }

      /// implementation of var args function
      OfxStatus IntegerInstance::getV(va_list arg)
      {
        int *value = va_arg(arg, int*);
        OfxStatus stat = get(*value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus IntegerInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value = va_arg(arg, int*);
        OfxStatus stat = get(time, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus IntegerInstance::setV(va_list arg)
      {
        int value = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus IntegerInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int value = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }
      
      /// implementation of var args function
      OfxStatus IntegerInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value = va_arg(arg, int*);
        OfxStatus stat = derive(time, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus IntegerInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        int *value = va_arg(arg, int*);
        OfxStatus stat = integrate(time1, time2, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }

      //
      // DoubleInstance
      //
      /// implementation of var args function
      OfxStatus DoubleInstance::getV(va_list arg)
      {
        double *value = va_arg(arg, double*);
        OfxStatus stat = get(*value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus DoubleInstance::getV(OfxTime time, va_list arg)
      {
        double *value = va_arg(arg, double*);
        OfxStatus stat = get(time, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus DoubleInstance::setV(va_list arg)
      {
        double value = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus DoubleInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double value = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }
      
      /// implementation of var args function
      OfxStatus DoubleInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *value = va_arg(arg, double*);
        OfxStatus stat = derive(time, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus DoubleInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        double *value = va_arg(arg, double*);
        OfxStatus stat = integrate(time1, time2, *value);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }

      //
      // BooleanInstance
      //
      /// implementation of var args function
      OfxStatus BooleanInstance::getV(va_list arg)
      {
        bool v;
        OfxStatus stat = get(v);

        int *value = va_arg(arg, int*);
        *value = v;
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus BooleanInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        bool v;
        OfxStatus stat = get(time, v);

        int *value = va_arg(arg, int*);
        *value = v;
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus BooleanInstance::setV(va_list arg)
      {
        bool value = va_arg(arg, int) != 0;
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus BooleanInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        bool value = va_arg(arg, int) != 0;
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }
      

      // 
      // RGBAInstance
      // 

      OfxStatus RGBAInstance::derive(OfxTime /*time*/, double&, double&, double&, double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus RGBAInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, double&,double&,double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      /// implementation of var args function
      OfxStatus RGBAInstance::getV(va_list arg)
      {
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        double *a = va_arg(arg, double*);
        OfxStatus stat = get(*r, *g, *b, *a);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b << ' ' << *a;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBAInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        double *a = va_arg(arg, double*);
        OfxStatus stat = get(time, *r, *g, *b, *a);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b << ' ' << *a;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBAInstance::setV(va_list arg)
      {
        double r = va_arg(arg, double);
        double g = va_arg(arg, double);
        double b = va_arg(arg, double);
        double a = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << r << ',' << g << ',' << b << ',' << a << ')';
#       endif
        return set(r, g, b, a);
      }

      /// implementation of var args function
      OfxStatus RGBAInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double r = va_arg(arg, double);
        double g = va_arg(arg, double);
        double b = va_arg(arg, double);
        double a = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << r << ',' << g << ',' << b << ',' << a << ')';
#       endif
        return set(time, r, g, b, a);
      }
      
      /// implementation of var args function
      OfxStatus RGBAInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        double *a = va_arg(arg, double*);
        OfxStatus stat = derive(time, *r, *g, *b, *a);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b << ' ' << *a;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBAInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        double *a = va_arg(arg, double*);
        OfxStatus stat = integrate(time1, time2, *r, *g, *b, *a);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b << ' ' << *a;
        }
#       endif
        return stat;
      }

      //
      // RGBInstance
      //
      OfxStatus RGBInstance::derive(OfxTime /*time*/, double&,double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus RGBInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, double&,double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      /// implementation of var args function
      OfxStatus RGBInstance::getV(va_list arg)
      {
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        OfxStatus stat = get(*r, *g, *b);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        OfxStatus stat = get(time, *r, *g, *b);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBInstance::setV(va_list arg)
      {
        double r = va_arg(arg, double);
        double g = va_arg(arg, double);
        double b = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << r << ',' << g << ',' << b << ')';
#       endif
        return set(r, g, b);
      }

      /// implementation of var args function
      OfxStatus RGBInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double r = va_arg(arg, double);
        double g = va_arg(arg, double);
        double b = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << r << ',' << g << ',' << b << ')';
#       endif
        return set(time, r, g, b);
      }
      
      /// implementation of var args function
      OfxStatus RGBInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        OfxStatus stat = derive(time, *r, *g, *b);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus RGBInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        double *r = va_arg(arg, double*);
        double *g = va_arg(arg, double*);
        double *b = va_arg(arg, double*);
        OfxStatus stat = integrate(time1, time2, *r, *g, *b);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *r << ' ' << *g << ' ' << *b;
        }
#       endif
        return stat;
      }

      //
      // Double2DInstance
      //

      OfxStatus Double2DInstance::derive(OfxTime /*time*/, double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Double2DInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Double2DInstance::getV(va_list arg)
      {
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        OfxStatus stat = get(*value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double2DInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        OfxStatus stat = get(time, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double2DInstance::setV(va_list arg)
      {
        double value1 = va_arg(arg, double);
        double value2 = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ')';
#       endif
        return set(value1, value2);
      }

      /// implementation of var args function
      OfxStatus Double2DInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double value1 = va_arg(arg, double);
        double value2 = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ')';
#       endif
        return set(time, value1, value2);
      }
      
      /// implementation of var args function
      OfxStatus Double2DInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        OfxStatus stat = derive(time, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double2DInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        OfxStatus stat = integrate(time1, time2, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }

      //
      // Integer2DInstance
      //

      OfxStatus Integer2DInstance::derive(OfxTime /*time*/, int&,int&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Integer2DInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, int&,int&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Integer2DInstance::getV(va_list arg)
      {
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        OfxStatus stat = get(*value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer2DInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        OfxStatus stat = get(time, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer2DInstance::setV(va_list arg)
      {
        int value1 = va_arg(arg, int);
        int value2 = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ')';
#       endif
        return set(value1, value2);
      }

      /// implementation of var args function
      OfxStatus Integer2DInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int value1 = va_arg(arg, int);
        int value2 = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ')';
#       endif
        return set(time, value1, value2);
      }
      
      /// implementation of var args function
      OfxStatus Integer2DInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        OfxStatus stat = derive(time, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer2DInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        OfxStatus stat = integrate(time1, time2, *value1, *value2);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2;
        }
#       endif
        return stat;
      }

      //
      // Double3DInstance
      //

      OfxStatus Double3DInstance::derive(OfxTime /*time*/, double&,double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Double3DInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, double&,double&,double&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Double3DInstance::getV(va_list arg)
      {
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        double *value3 = va_arg(arg, double*);
        OfxStatus stat = get(*value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double3DInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        double *value3 = va_arg(arg, double*);
        OfxStatus stat = get(time, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double3DInstance::setV(va_list arg)
      {
        double value1 = va_arg(arg, double);
        double value2 = va_arg(arg, double);
        double value3 = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ',' << value3 << ')';
#       endif
        return set(value1, value2, value3);
      }

      /// implementation of var args function
      OfxStatus Double3DInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double value1 = va_arg(arg, double);
        double value2 = va_arg(arg, double);
        double value3 = va_arg(arg, double);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ',' << value3 << ')';
#       endif
        return set(time, value1, value2, value3);
      }
      
      /// implementation of var args function
      OfxStatus Double3DInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        double *value3 = va_arg(arg, double*);
        OfxStatus stat = derive(time, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Double3DInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        double *value1 = va_arg(arg, double*);
        double *value2 = va_arg(arg, double*);
        double *value3 = va_arg(arg, double*);
        OfxStatus stat = integrate(time1, time2, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }

      //
      // Integer3DInstance
      //
      OfxStatus Integer3DInstance::derive(OfxTime /*time*/, int&,int&,int&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Integer3DInstance::integrate(OfxTime /*time1*/, OfxTime /*time2*/, int&,int&,int&) {
        return kOfxStatErrMissingHostFeature; 
      }

      OfxStatus Integer3DInstance::getV(va_list arg)
      {
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        int *value3 = va_arg(arg, int*);
        OfxStatus stat = get(*value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer3DInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        int *value3 = va_arg(arg, int*);
        OfxStatus stat = get(time, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer3DInstance::setV(va_list arg)
      {
        int value1 = va_arg(arg, int);
        int value2 = va_arg(arg, int);
        int value3 = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ',' << value3 << ')';
#       endif
        return set(value1, value2, value3);
      }

      /// implementation of var args function
      OfxStatus Integer3DInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int value1 = va_arg(arg, int);
        int value2 = va_arg(arg, int);
        int value3 = va_arg(arg, int);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << '(' << value1 << ',' << value2 << ',' << value3 << ')';
#       endif
        return set(time, value1, value2, value3);
      }
      
      /// implementation of var args function
      OfxStatus Integer3DInstance::deriveV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        int *value3 = va_arg(arg, int*);
        OfxStatus stat = derive(time, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus Integer3DInstance::integrateV(OfxTime time1, OfxTime time2, va_list arg)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
        int *value1 = va_arg(arg, int*);
        int *value2 = va_arg(arg, int*);
        int *value3 = va_arg(arg, int*);
        OfxStatus stat = integrate(time1, time2, *value1, *value2, *value3);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value1 << ' ' << *value2 << ' ' << *value3;
        }
#       endif
        return stat;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // string param
      OfxStatus StringInstance::getV(va_list arg)
      {
        const char **value = va_arg(arg, const char **);

        OfxStatus stat = get(_returnValue); // I so don't like this, temp storage should be delegated to the implementation
        *value = _returnValue.c_str();
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus StringInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        const char **value = va_arg(arg, const char **);

        OfxStatus stat = get(time, _returnValue); // I so don't like this, temp storage should be delegated to the implementation
        *value = _returnValue.c_str();
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *value;
        }
#       endif
        return stat;
      }
      
      /// implementation of var args function
      OfxStatus StringInstance::setV(va_list arg)
      {
        char *value = va_arg(arg, char*);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(value);
      }

      /// implementation of var args function
      OfxStatus StringInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        char *value = va_arg(arg, char*);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << value;
#       endif
        return set(time, value);
      }

      //
      // BytesInstance (OFX 1.5)
      //

      BytesInstance::BytesInstance(Descriptor& descriptor, Param::SetInstance* instance)
        : Instance(descriptor, instance)
        , _copyingDefault(false)
      {
        _bytesDefault.data = NULL;
        _bytesDefault.length = 0;
        // kOfxParamPropDefault: "pointer to OfxBytes struct of one dimension, or nullptr".
        // The property was copied from the descriptor, and points to its copy: point it to ours.
        try {
          (void)copyDefault();
        } catch (const Property::Exception &) {
          // the property could not be set, and still points to the descriptor's copy
        }
      }

      /// copy the OfxBytes that kOfxParamPropDefault points to, and point the property to the copy.
      /// Returns kOfxStatOK, or kOfxStatErrValue ({NULL, length > 0}, as setV) or kOfxStatErrMemory
      /// (the copy failed) with a NULL default. Throws Property::Exception(kOfxStatFailed) if the
      /// property cannot be set. Same rules as the descriptor's copy (Descriptor::BytesDefaultHook).
      OfxStatus BytesInstance::copyDefault() OFX_EXCEPTION_SPEC
      {
        Property::Set &props = getProperties();
        const OfxBytes *def = (const OfxBytes *)props.getPointerProperty(kOfxParamPropDefault);
        if (def == &_bytesDefault) {
          return kOfxStatOK; // already the copy
        }
        const OfxStatus stat = copyBytesDefault(def, _defaultValue);
        if (stat != kOfxStatOK) {
          // never keep the plug-in's pointer, which may be freed: the default becomes NULL
          def = NULL;
        }
        _bytesDefault.data = _defaultValue.empty() ? NULL : &_defaultValue[0];
        _bytesDefault.length = _defaultValue.size();
        // a NULL default stays NULL ("pointer to OfxBytes struct of one dimension, or nullptr")
        _copyingDefault = true;
        try {
          props.setPointerProperty(kOfxParamPropDefault, def ? (void*)&_bytesDefault : NULL);
        } catch (...) {
          _copyingDefault = false;
          throw Property::Exception(kOfxStatFailed);
        }
        _copyingDefault = false;
        return stat;
      }

      void BytesInstance::notify(const std::string &name, bool single, int num) OFX_EXCEPTION_SPEC
      {
        if (name != kOfxParamPropDefault) {
          Instance::notify(name, single, num);
          return;
        }
        if (_copyingDefault) {
          return; // the property is being pointed to the copy: setDefault() is called once, below
        }
        // copy first, so that setDefault(), which may be overridden, sees the copy
        const OfxStatus stat = copyDefault();
        Instance::notify(name, single, num);
        if (stat != kOfxStatOK) {
          throw Property::Exception(stat);
        }
      }

      /// fill the plug-in's OfxBytes from storage owned by this instance
      static void fillBytes(OfxBytes *value, const std::vector<unsigned char> &storage)
      {
        value->data = storage.empty() ? NULL : &storage[0];
        value->length = storage.size();
      }

      /// implementation of var args function
      OfxStatus BytesInstance::getV(va_list arg)
      {
        OfxBytes *value = va_arg(arg, OfxBytes *);
        if (!value) {
          return kOfxStatErrValue;
        }
        OfxStatus stat = get(_returnValue); // temp storage, valid until the next call (like strings)
        if (stat == kOfxStatOK) {
          fillBytes(value, _returnValue);
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << " (" << value->length << " bytes)";
#         endif
        }
        return stat;
      }

      /// implementation of var args function
      OfxStatus BytesInstance::getV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        OfxBytes *value = va_arg(arg, OfxBytes *);
        if (!value) {
          return kOfxStatErrValue;
        }
        OfxStatus stat = get(time, _returnValue);
        if (stat == kOfxStatOK) {
          fillBytes(value, _returnValue);
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << " (" << value->length << " bytes)";
#         endif
        }
        return stat;
      }

      /// implementation of var args function
      OfxStatus BytesInstance::setV(va_list arg)
      {
        const OfxBytes *value = va_arg(arg, const OfxBytes *);
        if (value && !value->data && value->length > 0) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << " (" << (value ? value->length : 0) << " bytes)";
#       endif
        // a NULL OfxBytes pointer sets an empty value
        return set(value ? value->data : NULL, value ? value->length : 0);
      }

      /// implementation of var args function
      OfxStatus BytesInstance::setV(OfxTime time, va_list arg)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
        const OfxBytes *value = va_arg(arg, const OfxBytes *);
        if (value && !value->data && value->length > 0) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << " (" << (value ? value->length : 0) << " bytes)";
#       endif
        return set(time, value ? value->data : NULL, value ? value->length : 0);
      }

      //////////////////////////////////////////////////////////////////////////////////
      // Param::SetInstance
      //

      /// ctor
      SetInstance::SetInstance()
      : _ownsParams(true)
      {}

      SetInstance::SetInstance(const SetInstance& other)
      : _params(other._params)
      , _paramList(other._paramList)
      , _ownsParams(false)
      {

      }

      /// dtor. 
      SetInstance::~SetInstance()
      {
        // iterate the params and delete them
        if (_ownsParams) {
          std::list<Instance *>::iterator i;
          for(i = _paramList.begin(); i != _paramList.end(); ++i) {
            if(*i)
              delete (*i);
          }
        }
      }

      const std::map<std::string, Instance*> &SetInstance::getParams() const
      {
        return _params;
      }

      const std::list<Instance*> &SetInstance::getParamList() const
      {
        return _paramList;
      }

#ifdef OFX_EXTENSIONS_NATRON
      // get the list of parameters (name) in the order they should appear in the host viewport
      bool SetInstance::isInViewportParam(const std::string& /*paramName*/) const
      {
        return false;
      }

#endif

      OfxStatus SetInstance::addParam(const std::string& name, Instance* instance)
      {
        if(_params.find(name)==_params.end()){
          _params[name] = instance;
          _paramList.push_back(instance);
        }
        else
          return kOfxStatErrExists;

        return kOfxStatOK;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Suite functions below

      static OfxStatus paramDefineImpl(OfxParamSetHandle paramSet,
                                   const char *paramType,
                                   const char *name,
                                   OfxPropertySetHandle *propertySet)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramDefine - " << paramSet << ' ' << (paramType ? paramType : "(null)") << ' ' << (name ? name : "(null)") << ' ' << propertySet << " ...";
#       endif
        // on failure, hand back a NULL property set rather than leaving it uninitialised
        // (kOfxStatErrExists hands back the existing param's, below)
        if (propertySet) {
          *propertySet = NULL;
        }

        // the handle is a BaseSet (getParamSetHandle()). Params are defined in a describe
        // action, on a descriptor's set: an instance's set is "kOfxStatErrBadHandle - if the
        // plugin handle was invalid" (a reinterpret_cast would use it as a SetDescriptor).
        BaseSet *baseSet = reinterpret_cast<BaseSet*>(paramSet);
        SetDescriptor *paramSetDescriptor = baseSet ? dynamic_cast<SetDescriptor*>(baseSet) : NULL;

        if (!paramSetDescriptor) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        // a NULL type is not a type: "kOfxStatErrUnknown - if the type is unknown"
        if (!paramType) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrUnknown) << std::endl;
#         endif
          return kOfxStatErrUnknown;
        }

        // "kOfxStatErrUnknown - if the type is unknown": none of the kOfxParamType* types.
        // SetDescriptor::paramDefine() returns NULL for it, and host overrides that call it
        // (e.g. Natron's) don't expect NULL. A parametric param is known even when HostSupport is
        // built without OFX_SUPPORTS_PARAMETRIC (not in isStandardType()): it stays
        // kOfxStatErrUnsupported, below.
        if (!isStandardType(paramType) && std::string(paramType) != kOfxParamTypeParametric) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrUnknown) << std::endl;
#         endif
          return kOfxStatErrUnknown;
        }

        // the spec lists no status for a NULL name (the "unique name of the parameter"):
        // it is "kOfxStatErrValue", "something failed due an illegal value" (ofxCore.h)
        if (!name) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrValue) << std::endl;
#         endif
          return kOfxStatErrValue;
        }

        if (paramSetDescriptor->getParams().count(name)) {
          // "kOfxStatErrExists - if a parameter of that name exists already in this plugin"
          // (defining it twice would make every instance of the effect fail in populate())
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrExists) << std::endl;
#         endif
          // like clipDefine, hand back the existing param's properties, so that a
          // plug-in ignoring the status doesn't use an uninitialised handle
          if (propertySet) {
            *propertySet = paramSetDescriptor->getParams().find(name)->second->getPropHandle();
          }
          return kOfxStatErrExists;
        }

        // The type is known, but the host application may not implement it (its
        // newParam() could not instantiate it): "kOfxStatErrUnsupported - if the type
        // is known but unsupported". This is checked here rather than in the virtual
        // SetDescriptor::paramDefine(), because host overrides of the latter (e.g.
        // Natron's) don't expect it to return NULL for a known type.
        // A host without the property at all is a host from before the type (an OFX 1.4 host,
        // e.g. hostDemo --ofx14) that doesn't know it: "kOfxStatErrUnknown - if the type is
        // unknown".
        if (std::string(paramType) == kOfxParamTypeStrChoice &&
            !(ImageEffect::gImageEffectHost &&
              ImageEffect::gImageEffectHost->getProperties().getIntProperty(kOfxParamHostPropSupportsStrChoice) != 0)) {
          const OfxStatus st = (ImageEffect::gImageEffectHost &&
                                !ImageEffect::gImageEffectHost->getProperties().fetchProperty(kOfxParamHostPropSupportsStrChoice, true)) ?
                               kOfxStatErrUnknown : kOfxStatErrUnsupported;
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(st) << std::endl;
#         endif
          return st;
        }

        // Same for Bytes params (OFX 1.5). The standard has no host property for them:
        // plug-ins find out from this status (see the upstream Examples/Custom plug-in).
        // HostSupport-based hosts opt in with kOfxHostSupportHostPropSupportsBytes.
        if (std::string(paramType) == kOfxParamTypeBytes &&
            !(ImageEffect::gImageEffectHost &&
              ImageEffect::gImageEffectHost->getProperties().getIntProperty(kOfxHostSupportHostPropSupportsBytes) != 0)) {
          const OfxStatus st = (ImageEffect::gImageEffectHost &&
                                !ImageEffect::gImageEffectHost->getProperties().fetchProperty(kOfxHostSupportHostPropSupportsBytes, true)) ?
                               kOfxStatErrUnknown : kOfxStatErrUnsupported;
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(st) << std::endl;
#         endif
          return st;
        }

        Descriptor *desc = paramSetDescriptor->paramDefine(paramType, name);

        if(desc) {
          if (propertySet)
            *propertySet = desc->getPropHandle();
          // desc is still referenced by _paramList and _paramMap
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatOK) << std::endl;
#         endif
          return kOfxStatOK;
        } else {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrUnsupported) << std::endl;
#         endif
          return kOfxStatErrUnsupported;
        }
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramDefine(OfxParamSetHandle paramSet, const char *paramType, const char *name, OfxPropertySetHandle *propertySet)
      {
        try {
          return paramDefineImpl(paramSet, paramType, name, propertySet);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramGetHandleImpl(OfxParamSetHandle paramSet,
                                      const char *name,
                                      OfxParamHandle *param,
                                      OfxPropertySetHandle *propertySet)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetHandle - " << paramSet << ' ' << (name ? name : "(null)") << ' ' << param << ' ' << propertySet << " ...";
#       endif
        // on failure, hand back NULL handles rather than leaving them uninitialised
        if (param) {
          *param = NULL;
        }
        if (propertySet) {
          *propertySet = NULL;
        }

        BaseSet *baseSet = reinterpret_cast<BaseSet*>(paramSet);

        if (!baseSet) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        // a NULL name names no parameter: "kOfxStatErrUnknown", the status of an
        // "operation on or request for an unknown object" (ofxCore.h), as for a name not found
        if (!name) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrUnknown) << std::endl;
#         endif
          return kOfxStatErrUnknown;
        }

        SetInstance *setInstance = dynamic_cast<SetInstance*>(baseSet);

        if(setInstance){          
          const std::map<std::string,Instance*>& params = setInstance->getParams();
          std::map<std::string,Instance*>::const_iterator it = params.find(name);         

          // if we can't find it return an error...
          if(it==params.end()) {
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' '<< StatStr(kOfxStatErrUnknown) << std::endl;
#           endif
            return kOfxStatErrUnknown;
          }

          // get the param
          if (param) {
            *param = (it->second)->getHandle();
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *param;
#           endif
          }

          // get the param property set
          if(propertySet) {
            *propertySet = (it->second)->getPropHandle();
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *propertySet;
#           endif
          }

#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatOK) << std::endl;
#         endif
          return kOfxStatOK;
        }

        // A descriptor's set is accepted too, at describe time, although ofxParam.h says "You
        // cannot call this in a plugin's describe function, as it needs an instance to work on":
        // the Support library's ParametricParamDescriptor::setParamSet() (ofxsParams.cpp) gets the
        // descriptor's handle this way to add the default control points, in the describe of every
        // plug-in with a parametric param. The handle is then a Param::Descriptor's, which the
        // value, key and copy functions reject (paramHandleToInstance()).
        SetDescriptor *setDescriptor = dynamic_cast<SetDescriptor*>(baseSet);
        
        if(setDescriptor){            
          const std::map<std::string,Descriptor*>& params = setDescriptor->getParams();
          std::map<std::string,Descriptor*>::const_iterator it = params.find(name);         
          
          // if we can't find it return an error...
          if(it==params.end()) {
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' '<< StatStr(kOfxStatErrUnknown) << std::endl;
#           endif
            return kOfxStatErrUnknown;
          }
          
          // get the param
          if (param) {
            *param = (it->second)->getHandle();  
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *param;
#           endif
          }

          // get the param property set
          if(propertySet) {
            *propertySet = (it->second)->getPropHandle();
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *propertySet;
#           endif
          }

#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatOK) << std::endl;
#         endif
          return kOfxStatOK;
        }

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#       endif
        return kOfxStatErrBadHandle;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char *name, OfxParamHandle *param, OfxPropertySetHandle *propertySet)
      {
        try {
          return paramGetHandleImpl(paramSet, name, param, propertySet);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramSetGetPropertySetImpl(OfxParamSetHandle paramSet,
                                              OfxPropertySetHandle *propHandle)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramSetGetPropertySet - " << paramSet << ' ' << propHandle << " ...";
#       endif
        BaseSet *baseSet = reinterpret_cast<BaseSet*>(paramSet);

        if (baseSet) {
          if (propHandle) {
            *propHandle = baseSet->getParamSetProps().getHandle();
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *propHandle;
#           endif
          }
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatOK) << std::endl;
#         endif
          return kOfxStatOK;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#       endif
        return kOfxStatErrBadHandle;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramSetGetPropertySet(OfxParamSetHandle paramSet, OfxPropertySetHandle *propHandle)
      {
        try {
          return paramSetGetPropertySetImpl(paramSet, propHandle);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      } 
      
      /// Map a param handle to a param instance. The handle is Base::getHandle(), the address
      /// of the (polymorphic) Param::Base, which may be a describe-time Param::Descriptor: the
      /// value, key and copy functions need an Instance, and get NULL for a NULL or a
      /// describe-time (descriptor) handle ("kOfxStatErrBadHandle - if the parameter handle was
      /// invalid"). Param::Base::verifyMagic() always returns true (there is no magic number):
      /// any other pointer is undefined behaviour.
      static Instance *paramHandleToInstance(OfxParamHandle paramHandle)
      {
        Base *paramBase = reinterpret_cast<Base*>(paramHandle);
        if (!paramBase || !paramBase->verifyMagic()) {
          return NULL;
        }
        return dynamic_cast<Instance*>(paramBase);
      }

      static OfxStatus paramGetPropertySetImpl(OfxParamHandle param,
                                           OfxPropertySetHandle *propHandle)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetPropertySet - " << param << ' ' << propHandle << " ...";
#       endif
        // valid on a descriptor (in describe) as on an instance: the handle is a Param::Base
        Base *paramBase = reinterpret_cast<Base*>(param);
        
        if(paramBase && paramBase->verifyMagic()){
          // get the param property set
          if (propHandle) {
            *propHandle = paramBase->getPropHandle();
#           ifdef OFX_DEBUG_PARAMETERS
            std::cout << ' ' << *propHandle;
#           endif
          }

#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatOK) << std::endl;
#         endif
          return kOfxStatOK;
        } else {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramGetPropertySet(OfxParamHandle param, OfxPropertySetHandle *propHandle)
      {
        try {
          return paramGetPropertySetImpl(param, propHandle);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }

      /// get the current param value
      static OfxStatus paramGetValue(OfxParamHandle  paramHandle,
                                     ...)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetValue - " << paramHandle << " ...";
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        va_list ap;
        va_start(ap,paramHandle);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->getV(ap);
        }
        catch(...) {}

        va_end(ap);

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// get the param value at a time
      static OfxStatus paramGetValueAtTime(OfxParamHandle  paramHandle,
                                           OfxTime time,
                                           ...)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetValueAtTime - " << paramHandle << ' ' << time << " ...";
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }


        va_list ap;
        va_start(ap, time);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->getV(time, ap);
        }
        catch(...) {}

        va_end(ap);

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }
      
      /// get the param's derivative at the given time
      static OfxStatus paramGetDerivative(OfxParamHandle  paramHandle,
                                          OfxTime time,
                                          ...)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetDerivative - " << paramHandle << ' ' << time << " ...";
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        va_list ap;
        va_start(ap, time);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->deriveV(time, ap);
        }
        catch(...) {}

        va_end(ap);

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      static OfxStatus paramGetIntegral(OfxParamHandle  paramHandle,
                                        OfxTime time1, OfxTime time2,
                                        ...)
      {
        if ( OFX::IsNaN(time1) || OFX::IsNaN(time2) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetIntegral - " << paramHandle << ' ' << time1 << ' ' << time2 << " ...";
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        va_list ap;
        va_start(ap, time2);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->integrateV(time1, time2, ap);
        }
        catch(...) {}

        va_end(ap);

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// set the param's value at the 'current' time
      static OfxStatus paramSetValue(OfxParamHandle  paramHandle,
                                     ...) 
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramSetValue - " << paramHandle << ' ';
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << " ... " << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        va_list ap;
        va_start(ap, paramHandle);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->setV(ap);
        }
        catch(...) {}

        va_end(ap);

        if (stat == kOfxStatOK && paramInstance->getParamSetInstance()) {
          paramInstance->getParamSetInstance()->paramChangedByPlugin(paramInstance);
        }

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << " ... " << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      
      /// set the param's value at the indicated time, and set a key
      static OfxStatus paramSetValueAtTime(OfxParamHandle  paramHandle,
                                           OfxTime time,  // time in frames
                                           ...)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramSetValueAtTime - " << paramHandle << ' ' << time << ' ';
#       endif
        Instance *paramInstance = paramHandleToInstance(paramHandle);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << " ... " << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        va_list ap;
        va_start(ap, time);
        OfxStatus stat = kOfxStatErrUnsupported;

        try {
          stat = paramInstance->setV(time, ap);
        }
        catch(...) {}

        va_end(ap);

        if (stat == kOfxStatOK && paramInstance->getParamSetInstance()) {
          paramInstance->getParamSetInstance()->paramChangedByPlugin(paramInstance);
        }

#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << " ... " << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      static OfxStatus paramGetNumKeysImpl(OfxParamHandle  paramHandle,
                                       unsigned int  *numberOfKeys)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetNumKeys - " << paramHandle << " ...";
#       endif
        Param::Instance *pInstance = paramHandleToInstance(paramHandle);

        if (!pInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        KeyframeParam *paramInstance = dynamic_cast<KeyframeParam*>(pInstance);
        if(!paramInstance) {
#       ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#       endif
          return kOfxStatErrBadHandle;
        }
        // a NULL return pointer is rejected as a bad handle, as in the timeline suite
        if (!numberOfKeys) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = paramInstance->getNumKeys(*numberOfKeys);
#       ifdef OFX_DEBUG_PARAMETERS
        if (stat == kOfxStatOK) {
          std::cout << ' ' << *numberOfKeys;
        }
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramGetNumKeys(OfxParamHandle paramHandle, unsigned int *numberOfKeys)
      {
        try {
          return paramGetNumKeysImpl(paramHandle, numberOfKeys);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramGetKeyTimeImpl(OfxParamHandle  paramHandle,
                                       unsigned int nthKey,
                                       OfxTime *time)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetKeyTime - " << paramHandle << " ...";
#       endif
        Param::Instance *pInstance = paramHandleToInstance(paramHandle);

        if (!pInstance) {
#       ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#       endif
          return kOfxStatErrBadHandle;
        }

        KeyframeParam *paramInstance = dynamic_cast<KeyframeParam*>(pInstance);
        if(!paramInstance) {
#       ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#       endif
          return kOfxStatErrBadHandle;
        }
        // a NULL return pointer is rejected as a bad handle, as in the timeline suite
        if (!time) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = paramInstance->getKeyTime(nthKey,*time);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramGetKeyTime(OfxParamHandle paramHandle, unsigned int nthKey, OfxTime *time)
      {
        try {
          return paramGetKeyTimeImpl(paramHandle, nthKey, time);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramGetKeyIndexImpl(OfxParamHandle  paramHandle,
                                        OfxTime time,
                                        int     direction,
                                        int    *index) 
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramGetKeyIndex - " << paramHandle << " ...";
#       endif
        Param::Instance *pInstance = paramHandleToInstance(paramHandle);

        if (!pInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        KeyframeParam *paramInstance = dynamic_cast<KeyframeParam*>(pInstance);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        // a NULL return pointer is rejected as a bad handle, as in the timeline suite
        if (!index) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = paramInstance->getKeyIndex(time,direction,*index);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramGetKeyIndex(OfxParamHandle paramHandle, OfxTime time, int direction, int *index)
      {
        try {
          return paramGetKeyIndexImpl(paramHandle, time, direction, index);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramDeleteKeyImpl(OfxParamHandle  paramHandle,
                                      OfxTime time)
      {
        if ( OFX::IsNaN(time) ) {
          return kOfxStatErrValue;
        }
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramDeleteKey - " << paramHandle << " ...";
#       endif
        Param::Instance *pInstance = paramHandleToInstance(paramHandle);

        if (!pInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        KeyframeParam *paramInstance = dynamic_cast<KeyframeParam*>(pInstance);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = paramInstance->deleteKey(time);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramDeleteKey(OfxParamHandle paramHandle, OfxTime time)
      {
        try {
          return paramDeleteKeyImpl(paramHandle, time);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramDeleteAllKeysImpl(OfxParamHandle  paramHandle) 
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramDeleteAllKeys - " << paramHandle << " ...";
#       endif
        Param::Instance *pInstance = paramHandleToInstance(paramHandle);

        if (!pInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        KeyframeParam *paramInstance = dynamic_cast<KeyframeParam*>(pInstance);
        if(!paramInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = paramInstance->deleteAllKeys();
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramDeleteAllKeys(OfxParamHandle paramHandle)
      {
        try {
          return paramDeleteAllKeysImpl(paramHandle);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramCopyImpl(OfxParamHandle  paramTo,
                                 OfxParamHandle  paramFrom, 
                                 OfxTime dstOffset, const OfxRangeD *frameRange)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramCopy - " << paramTo << " ...";
#       endif
        Instance *paramInstanceTo = paramHandleToInstance(paramTo);
        Instance *paramInstanceFrom = paramHandleToInstance(paramFrom);

        if(!paramInstanceTo || !paramInstanceFrom) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }

        OfxStatus stat = paramInstanceTo->copyFrom(*paramInstanceFrom,dstOffset,frameRange);
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramCopy(OfxParamHandle paramTo, OfxParamHandle paramFrom, OfxTime dstOffset, const OfxRangeD *frameRange)
      {
        try {
          return paramCopyImpl(paramTo, paramFrom, dstOffset, frameRange);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static OfxStatus paramEditBeginImpl(OfxParamSetHandle paramSet, const char *name)
      {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramEditBegin - " << paramSet << ' ' << (name ? name : "(null)") << " ...";
#       endif
        // the handle is a BaseSet (getParamSetHandle()); a descriptor's set (in describe) has no
        // undo/redo: "kOfxStatErrBadHandle - if the instance handle was invalid"
        BaseSet *baseSet = reinterpret_cast<BaseSet*>(paramSet);
        SetInstance *setInstance = baseSet ? dynamic_cast<SetInstance*>(baseSet) : NULL;
        if(!setInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        // name is the "label to attach to any undo/redo string": a NULL name is an empty one
        OfxStatus stat = setInstance->editBegin(name ? std::string(name) : std::string());
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramEditBegin(OfxParamSetHandle paramSet, const char *name)
      {
        try {
          return paramEditBeginImpl(paramSet, name);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }

      
      static OfxStatus paramEditEndImpl(OfxParamSetHandle paramSet) {
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << "OFX: paramEditEnd - " << paramSet << " ...";
#       endif
        // see paramEditBeginImpl()
        BaseSet *baseSet = reinterpret_cast<BaseSet*>(paramSet);
        SetInstance *setInstance = baseSet ? dynamic_cast<SetInstance*>(baseSet) : NULL;
        if(!setInstance) {
#         ifdef OFX_DEBUG_PARAMETERS
          std::cout << ' ' << StatStr(kOfxStatErrBadHandle) << std::endl;
#         endif
          return kOfxStatErrBadHandle;
        }
        OfxStatus stat = setInstance->editEnd();
#       ifdef OFX_DEBUG_PARAMETERS
        std::cout << ' ' << StatStr(stat) << std::endl;
#       endif
        return stat;
      }

      /// C entry point: C++ exceptions must not propagate into the plug-in
      static OfxStatus paramEditEnd(OfxParamSetHandle paramSet)
      {
        try {
          return paramEditEndImpl(paramSet);
        } catch (const std::bad_alloc &) {
          return kOfxStatErrMemory;
        } catch (const Property::Exception &e) {
          return e.getStatus();
        } catch (...) {
          return kOfxStatFailed;
        }
      }
      
      static const OfxParameterSuiteV1 gParamSuiteV1 = {
        paramDefine,
        paramGetHandle,
        paramSetGetPropertySet,
        paramGetPropertySet,
        paramGetValue,
        paramGetValueAtTime,
        paramGetDerivative,
        paramGetIntegral,
        paramSetValue,
        paramSetValueAtTime,
        paramGetNumKeys,
        paramGetKeyTime,
        paramGetKeyIndex,
        paramDeleteKey,
        paramDeleteAllKeys,
        paramCopy,
        paramEditBegin,
        paramEditEnd
      };


      const void *GetSuite(int version) {
        if(version ==1)
          return &gParamSuiteV1;
        return NULL;
      }

    } // Param

  } // Host

} // OFX
