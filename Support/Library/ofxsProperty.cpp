// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "ofxsSupportPrivate.h"

using namespace OFX::Private;

namespace OFX {

  static
  void throwPropertyException(OfxStatus stat,
    const std::string &propName)
  {
    switch (stat) 
    {
    case kOfxStatOK :
    case kOfxStatReplyYes :
    case kOfxStatReplyNo :
    case kOfxStatReplyDefault :
      // Throw nothing!
      break;

    case kOfxStatErrUnknown :
    case kOfxStatErrUnsupported : // unsupported implies unknow here
      if(OFX::PropertySet::getThrowOnUnsupportedProperties()) // are we suppressing this?
        throw OFX::Exception::PropertyUnknownToHost(propName.c_str());
      break;

    case kOfxStatErrMemory :
      throw std::bad_alloc();
      break;

    case kOfxStatErrValue :
      throw  OFX::Exception::PropertyValueIllegalToHost(propName.c_str());
      break;

    case kOfxStatErrBadHandle :
    case kOfxStatErrBadIndex :
    default :
      throwSuiteStatusException(stat);
      break;
    }
  }

  /** @brief are we logging property get/set */
  std::atomic<int> PropertySet::_gPropLogging(1);

  /** @brief Do we throw an exception if a host returns 'unsupported' when setting a property */
  bool PropertySet::_gThrowOnUnsupported = true;

  /** @brief Virtual destructor */
  PropertySet::~PropertySet() {}

  /** @brief, returns wether the given property exists in this property set */
  bool PropertySet::propExists(const char* property, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int dimension = 0;
    OfxStatus stat = gPropSuite->propGetDimension(_propHandle, property, &dimension);
    Log::error(stat != kOfxStatOK && stat != kOfxStatErrUnknown && stat != kOfxStatErrUnsupported, "Failed on fetching dimension for property %s, host returned status %s.", property, mapStatusToString(stat));
    if(throwOnFailure && stat != kOfxStatErrUnknown && stat != kOfxStatErrUnsupported)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) 
      Log::print("Fetched dimension of property %s, returned status %s.",  property, mapStatusToString(stat));

    return stat == kOfxStatOK;
  }

  // a failure worth an ERROR in the log: any failure of a required call (throwOnFailure), and
  // for optional calls (throwOnFailure=false) every failure but a property the host doesn't know
  // (e.g. newer than the host's OFX version, or host_optional; kOfxStatErrUnsupported is not a
  // property suite status, but some hosts return it). kOfxStatErrBadIndex stays an error: it is
  // also what an off-by-one index gives; read the dimension first when it may be short
  static inline bool logPropertyFailure(OfxStatus stat, bool throwOnFailure)
  {
    return stat != kOfxStatOK &&
           (throwOnFailure || (stat != kOfxStatErrUnknown && stat != kOfxStatErrUnsupported));
  }

  /** @brief, returns the dimension of the given property from this property set */
  int PropertySet::propGetDimension(const char* property, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int dimension = 0;
    OfxStatus stat = gPropSuite->propGetDimension(_propHandle, property, &dimension);
    Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on fetching dimension for property %s, host returned status %s.", property, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) 
      Log::print("Fetched dimension of property %s, returned %d.",  property, dimension);

    return dimension;
  }

  /** @brief, resets the property to it's default value */
  void PropertySet::propReset(const char* property)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propReset(_propHandle, property);
    Log::error(stat != kOfxStatOK, "Failed on reseting property %s to its defaults, host returned status %s.", property, mapStatusToString(stat));
    throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Reset property %s.",  property);
  }

  /** @brief, Set a single dimension pointer property */
  void PropertySet::propSetPointer(const char* property, void *value, int idx, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propSetPointer(_propHandle, property, idx, value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting pointer property %s[%d] to %p, host returned status %s;", 
      property, idx, value, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);  

    if(_gPropLogging > 0) Log::print("Set pointer property %s[%d] to be %p.",  property, idx, value);
  }

  /** @brief, Set a single dimension string property */
  void PropertySet::propSetString(const char* property, const std::string &value, int idx, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propSetString(_propHandle, property, idx, value.c_str());
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting string property %s[%d] to %s, host returned status %s;", 
      property, idx, value.c_str(), mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Set string property %s[%d] to be %s.",  property, idx, value.c_str());
  }

  /** @brief, Set a single dimension double property */
  void PropertySet::propSetDouble(const char* property, double value, int idx, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propSetDouble(_propHandle, property, idx, value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting double property %s[%d] to %lf, host returned status %s;", 
      property, idx, value, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Set double property %s[%d] to be %lf.",  property, idx, value);
  }

  /** @brief, Set a single dimension int property */
  void PropertySet::propSetInt(const char* property, int value, int idx, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propSetInt(_propHandle, property, idx, value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting int property %s[%d] to %d, host returned status %s (%d);", 
      property, idx, value, mapStatusToString(stat), stat);
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Set int property %s[%d] to be %d.",  property, idx, value);
  }

  void PropertySet::propSetStringN(const char* property, const std::vector<std::string> &values, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    std::vector<const char*> data(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      data[i] = values[i].c_str();
    }
    OfxStatus stat = gPropSuite->propSetStringN(_propHandle, property, (int)data.size(), data.empty() ? NULL : &data[0]);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting string property %s[0..%d], host returned status %s;",
                    property, (int)values.size()-1, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);
    
    if(_gPropLogging > 0) Log::print("Set string property %s[0..%d].",  property, (int)values.size()-1);
  }

  /** @brief, Set a multiple dimension double property */
  void PropertySet::propSetDoubleN(const char* property, const std::vector<double> &values, bool throwOnFailure)
  {
    propSetDoubleN(property, values.empty() ? NULL : &values[0], (int)values.size(), throwOnFailure);
  }
  
  /** @brief, Set a multiple dimension double property */
  void PropertySet::propSetDoubleN(const char* property, const double* values, int count, bool throwOnFailure)
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propSetDoubleN(_propHandle, property, count, values);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting double property %s[0..%d], host returned status %s;",
      property, count-1, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Set double property %s[0..%d].",  property, count-1);
  }
  
  void PropertySet::propSetIntN(const char* property, const std::vector<int> &values, bool throwOnFailure)
  {
    propSetIntN(property, values.empty() ? NULL : &values[0], (int)values.size(), throwOnFailure);
  }
  
  void PropertySet::propSetIntN(const char* property, const int *values, int count, bool throwOnFailure)
  {
    assert(_propHandle != 0);

    OfxStatus stat = gPropSuite->propSetIntN(_propHandle, property, count, values);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on setting int property %s[0..%d], host returned status %s;",
                    property, count-1, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);

    if(_gPropLogging > 0) Log::print("Set int property %s[0..%d].",  property, count-1);

  }

  /** @brief Get single pointer property */
  void*  PropertySet::propGetPointer(const char* property, int idx, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    void *value = 0;
    OfxStatus stat = gPropSuite->propGetPointer(_propHandle, property, idx, &value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting pointer property %s[%d], host returned status %s;", 
      property, idx, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Retrieved pointer property %s[%d], was given %p.",  property, idx, value);

    return value;
  }

  /** @brief Get single string property */
  std::string PropertySet::propGetString(const char* property, int idx, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    const char *value = NULL;
    OfxStatus stat = gPropSuite->propGetString(_propHandle, property, idx, &value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting string property %s[%d], host returned status %s;", 
      property, idx, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);

    if(_gPropLogging > 0) Log::print("Retrieved string property %s[%d], was given %s.",  property, idx, value);
    return value != NULL ?  std::string(value) : std::string();
  }

  /** @brief Get single double property */
  double PropertySet::propGetDouble(const char* property, int idx, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    double value = 0;
    OfxStatus stat = gPropSuite->propGetDouble(_propHandle, property, idx, &value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting double property %s[%d], host returned status %s;", 
      property, idx, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Retrieved double property %s[%d], was given %lf.",  property, idx, value);
    return value;
  }

  /** @brief Get single int property */
  int PropertySet::propGetInt(const char* property, int idx, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int value = 0;
    OfxStatus stat = gPropSuite->propGetInt(_propHandle, property, idx, &value);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting int property %s[%d], host returned status %s;", 
      property, idx, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property); 

    if(_gPropLogging > 0) Log::print("Retrieved int property %s[%d], was given %d.",  property, idx, value);
    return value;
  }
    
  void PropertySet::propGetStringN(const char* property, std::vector<std::string>* values, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int dimension = propGetDimension(property,throwOnFailure);
    if (dimension <= 0 || !values) {
      return;
    }
    std::vector<const char*> rawValue(dimension, (char*)0);
    OfxStatus stat = gPropSuite->propGetStringN(_propHandle, property, dimension, &rawValue.front());
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting string property %s, host returned status %s;",
                      property, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);
      
    values->resize(dimension);
    for (int i = 0; i < dimension; ++i) {
      if (rawValue[i] != NULL) {
        (*values)[i] = std::string(rawValue[i]);
      } else {
        (*values)[i] = std::string();
      }
      // log the strings, not the bytes of the pointer array
      if(_gPropLogging > 0) Log::print("Retrieved string property %s[%d], was given %s.", property, i, (*values)[i].c_str());
    }
  }

  void PropertySet::propGetDoubleN(const char* property, std::vector<double>* values, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int dimension = propGetDimension(property,throwOnFailure);
    if (dimension <= 0 || !values) {
      return;
    }

    values->resize(dimension);
    propGetDoubleN(property, &values->front(), dimension, throwOnFailure);
  }

  void PropertySet::propGetDoubleN(const char* property, double* values, int count, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propGetDoubleN(_propHandle, property, count, values);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting double property %s, host returned status %s;",
                    property, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);

    if(_gPropLogging > 0) {
      switch (count) {
      case 1:
        Log::print("Retrieved doublex1 property %s, was given %g.", property, values[0]);
        break;
      case 2:
        Log::print("Retrieved doublex2 property %s, was given %g %g.", property, values[0], values[1]);
        break;
      case 3:
        Log::print("Retrieved doublex3 property %s, was given %g %g %g.", property, values[0], values[1], values[2]);
        break;
      case 4:
        Log::print("Retrieved doublex4 property %s, was given %g %g %g %g.", property, values[0], values[1], values[2], values[3]);
        break;
      default:
        Log::print("Retrieved doublex%d property %s.", count, property);
      }
    }
  }

  void PropertySet::propGetIntN(const char* property, std::vector<int>* values, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    int dimension = propGetDimension(property,throwOnFailure);
    if (dimension <= 0 || !values) {
      return;
    }

    values->resize(dimension);
    propGetIntN(property, &values->front(), dimension, throwOnFailure);
  }

  void PropertySet::propGetIntN(const char* property, int* values, int count, bool throwOnFailure) const
  {
    assert(_propHandle != 0);
    OfxStatus stat = gPropSuite->propGetIntN(_propHandle, property, count, values);
    OFX::Log::error(logPropertyFailure(stat, throwOnFailure), "Failed on getting int property %s, host returned status %s;",
                    property, mapStatusToString(stat));
    if(throwOnFailure)
      throwPropertyException(stat, property);

    if(_gPropLogging > 0) {
      switch (count) {
      case 1:
        Log::print("Retrieved intx1 property %s, was given %d.", property, values[0]);
        break;
      case 2:
        Log::print("Retrieved intx2 property %s, was given %d %d.", property, values[0], values[1]);
        break;
      case 3:
        Log::print("Retrieved intx3 property %s, was given %d %d %d.", property, values[0], values[1], values[2]);
        break;
      case 4:
        Log::print("Retrieved intx4 property %s, was given %d %d %d %d.", property, values[0], values[1], values[2], values[3]);
        break;
      default:
        Log::print("Retrieved intx%d property %s.", count, property);
      }
    }

  }

  namespace Private {

    // the host doesn't have the property: kOfxStatErrUnknown (kOfxStatErrUnsupported is not a
    // property suite status, but some hosts return it)
    static inline bool hostLacksProperty(OfxStatus stat)
    {
      return stat == kOfxStatErrUnknown || stat == kOfxStatErrUnsupported;
    }

    int propGetIntOr(const PropertySet& props, const char* property, int defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      int value = 0;
      OfxStatus stat = gPropSuite->propGetInt(props.propSetHandle(), property, idx, &value);
      if (hostLacksProperty(stat)) {
        return defaultValue;
      }
      Log::error(stat != kOfxStatOK, "Failed on getting int property %s[%d], host returned status %s;",
                 property, idx, mapStatusToString(stat));
      throwPropertyException(stat, property);
      return value;
    }

    double propGetDoubleOr(const PropertySet& props, const char* property, double defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      double value = 0.;
      OfxStatus stat = gPropSuite->propGetDouble(props.propSetHandle(), property, idx, &value);
      if (hostLacksProperty(stat)) {
        return defaultValue;
      }
      Log::error(stat != kOfxStatOK, "Failed on getting double property %s[%d], host returned status %s;",
                 property, idx, mapStatusToString(stat));
      throwPropertyException(stat, property);
      return value;
    }

    std::string propGetStringOr(const PropertySet& props, const char* property, const std::string& defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      const char* value = NULL;
      OfxStatus stat = gPropSuite->propGetString(props.propSetHandle(), property, idx, &value);
      if (hostLacksProperty(stat)) {
        return defaultValue;
      }
      Log::error(stat != kOfxStatOK, "Failed on getting string property %s[%d], host returned status %s;",
                 property, idx, mapStatusToString(stat));
      throwPropertyException(stat, property);
      return value != NULL ? std::string(value) : std::string();
    }

    void propSetIntOr(PropertySet& props, const char* property, int value, int defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      OfxStatus stat = gPropSuite->propSetInt(props.propSetHandle(), property, idx, value);
      if (hostLacksProperty(stat)) {
        Log::warning(value != defaultValue, "The host does not have the property %s: it ignores the value %d "
                     "(and behaves as with the default, %d)", property, value, defaultValue);
        return;
      }
      Log::error(stat != kOfxStatOK, "Failed on setting int property %s[%d] to %d, host returned status %s;",
                 property, idx, value, mapStatusToString(stat));
      throwPropertyException(stat, property);
    }

    void propSetDoubleOr(PropertySet& props, const char* property, double value, double defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      OfxStatus stat = gPropSuite->propSetDouble(props.propSetHandle(), property, idx, value);
      if (hostLacksProperty(stat)) {
        Log::warning(value != defaultValue, "The host does not have the property %s: it ignores the value %g "
                     "(and behaves as with the default, %g)", property, value, defaultValue);
        return;
      }
      Log::error(stat != kOfxStatOK, "Failed on setting double property %s[%d] to %g, host returned status %s;",
                 property, idx, value, mapStatusToString(stat));
      throwPropertyException(stat, property);
    }

    void propSetStringOr(PropertySet& props, const char* property, const std::string& value,
                         const std::string& defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      OfxStatus stat = gPropSuite->propSetString(props.propSetHandle(), property, idx, value.c_str());
      if (hostLacksProperty(stat)) {
        Log::warning(value != defaultValue, "The host does not have the property %s: it ignores the value \"%s\" "
                     "(and behaves as with the default, \"%s\")", property, value.c_str(), defaultValue.c_str());
        return;
      }
      Log::error(stat != kOfxStatOK, "Failed on setting string property %s[%d] to %s, host returned status %s;",
                 property, idx, value.c_str(), mapStatusToString(stat));
      throwPropertyException(stat, property);
    }

    void propSetPointerOr(PropertySet& props, const char* property, void* value, void* defaultValue, int idx)
    {
      assert(props.propSetHandle() != 0);
      OfxStatus stat = gPropSuite->propSetPointer(props.propSetHandle(), property, idx, value);
      if (hostLacksProperty(stat)) {
        Log::warning(value != defaultValue, "The host does not have the property %s: it ignores the value %p "
                     "(and behaves as with the default, %p)", property, value, defaultValue);
        return;
      }
      Log::error(stat != kOfxStatOK, "Failed on setting pointer property %s[%d] to %p, host returned status %s;",
                 property, idx, value, mapStatusToString(stat));
      throwPropertyException(stat, property);
    }

    bool propSetStringIfSupported(PropertySet& props, const char* property, const std::string& value, int idx)
    {
      assert(props.propSetHandle() != 0);
      OfxStatus stat = gPropSuite->propSetString(props.propSetHandle(), property, idx, value.c_str());
      if (stat == kOfxStatErrMemory) {
        throw std::bad_alloc();
      }
      Log::error(stat != kOfxStatOK && !hostLacksProperty(stat),
                 "Failed on setting string property %s[%d] to %s, host returned status %s;",
                 property, idx, value.c_str(), mapStatusToString(stat));
      return stat == kOfxStatOK;
    }

  } // namespace Private

}
