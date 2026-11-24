// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include <string>
#include <map>
#include <stdexcept>

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"

// ofx host
#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhClip.h"
#include "ofxhParam.h"
#include "ofxhMemory.h"
#include "ofxhImageEffect.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhHost.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhXml.h"

namespace OFX
{
  namespace Host
  {
    namespace APICache
    {
      void PluginAPICacheI::registerInCache(OFX::Host::PluginCache &pluginCache) {
        pluginCache.registerAPICache(_apiName, _apiVersionMin, _apiVersionMax, this);
      }      

      /// the value of an attribute of an XML element, or "" if it has none
      static std::string attributeValue(const std::map<std::string, std::string> &map, const char *name)
      {
        const std::map<std::string, std::string>::const_iterator it = map.find(name);
        return it == map.end() ? std::string() : it->second;
      }

      void propertySetXMLRead(const std::string &el,
                              const std::map<std::string, std::string> &map,
                              Property::Set &set,
                              Property::Property *&currentProp) {
        if (el == "property") {
          const std::string propName = attributeValue(map, "name");
          const std::string propType = attributeValue(map, "type");
          int dimension = atoi(attributeValue(map, "dimension").c_str());
          if (dimension < 0) {
            throw std::runtime_error("malformed plugin cache: negative property dimension");
          }
          
          currentProp = set.fetchProperty(propName, false);
          
          if(!currentProp) {
            if (propType == "int") {
              currentProp = new Property::Int(propName, dimension, false, 0);
            } else if (propType == "string") {
              currentProp = new Property::String(propName, dimension, false, "");
            } else if (propType == "double") {
              currentProp = new Property::Double(propName, dimension, false, 0);
            } else if (propType == "pointer") {
              currentProp = new Property::Pointer(propName, dimension, false, 0);
            } else {
              // unknown property type: ignore the property and its values
              return;
            }
            set.addProperty(currentProp);
          }
          return;
        }
        
        if (el == "value" && !currentProp) {
          // value of an ignored property
          return;
        }

        if (el == "value") {
          const std::string indexStr = attributeValue(map, "index");
          char *end = NULL;
          errno = 0;
          const long index = strtol(indexStr.c_str(), &end, 10);
          if (indexStr.empty() || *end != '\0' || errno == ERANGE) {
            throw std::runtime_error("malformed plugin cache: bad property index");
          }
          if (index < 0) {
            throw std::runtime_error("malformed plugin cache: negative property index");
          }
          // the values are written in order: a variable-dimension property grows by one value at
          // most (a huge index would make setValue allocate index+1 values), and a fixed-dimension
          // property has no index beyond its dimension
          const int fixedDimension = currentProp->getFixedDimension();
          if (fixedDimension ? index >= fixedDimension : index > currentProp->getDimension()) {
            throw std::runtime_error("malformed plugin cache: property index out of range");
          }
          const std::string value = attributeValue(map, "value");
          
          switch (currentProp->getType()) {
          case Property::eInt:
            set.setIntProperty(currentProp->getName(), atoi(value.c_str()), (int)index);
            break;
          case Property::eString:
            set.setStringProperty(currentProp->getName(), value, (int)index);
            break;
          case Property::eDouble:
            set.setDoubleProperty(currentProp->getName(), atof(value.c_str()), (int)index);
            break;
          case Property::ePointer:
            break;
          default:
            break;
          }

          return;
        }

        // a corrupt cache: a parse error discards it, and the plug-ins are scanned again
        throw std::runtime_error("malformed plugin cache: unexpected element <" + el + ">");
      }
      
      static void propertyXMLWrite(std::ostream &o, Property::Property *prop, const std::string &indent="")
      {        
        if (prop->getType() != Property::ePointer)  {
          
          o << indent << "<property "
            << XML::attribute("name", prop->getName())
            << XML::attribute("type", Property::gTypeNames[prop->getType()])
            << XML::attribute("dimension", prop->getFixedDimension()) 
            << ">\n";
          
          for (int i=0;i<prop->getDimension();i++) {
            o << indent << "  <value " 
              << XML::attribute("index", i)
              << XML::attribute("value", prop->getStringValue(i)) 
              << "/>\n";
          }
          
          o << indent << "</property>\n";
        }
      }

      void propertyXMLWrite(std::ostream &o, const Property::Set &set, const std::string &name, int indent)
      {
        Property::Property *prop = set.fetchProperty(name);

        if(prop) {
          std::string indent_prefix(indent, ' ');
          propertyXMLWrite(o, prop, indent_prefix);
        }
      }

      void propertySetXMLWrite(std::ostream &o, const Property::Set &set, int indent) 
      {
        std::string indent_prefix(indent, ' ');

        for (Property::PropertyMap::const_iterator i = set.getProperties().begin();
             i != set.getProperties().end();
             i++)
          {
            Property::Property *prop = i->second;
            propertyXMLWrite(o, prop, indent_prefix);
          }
      }

    }
  }
}
