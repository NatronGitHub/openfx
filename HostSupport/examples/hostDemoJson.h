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

#ifndef HOST_DEMO_JSON_H
#define HOST_DEMO_JSON_H

// A small JSON document model and writer (C++98, no external library), used by hostDemo's
// --report and --describe. Objects keep the order in which keys were added. Strings are written
// as UTF-8, with the control characters, '"' and '\\' escaped, and invalid UTF-8 bytes replaced
// by U+FFFD, so that the output is always valid JSON (RFC 8259). Numbers are written as %.17g
// would (enough digits to round-trip a double), in the classic locale (so with '.' as the decimal
// point and no digit grouping, whatever locale a plug-in sets); NaN and infinities, which JSON
// can't represent, are written as null.

#include <iterator> // std::advance
#include <list>
#include <locale> // std::locale::classic
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace MyHost {

  class Json {
  public:
    enum Type { eNull, eBool, eInt, eNumber, eString, eArray, eObject };

    Json() : _type(eNull), _b(false), _i(0), _d(0.) {}
    Json(bool b) : _type(eBool), _b(b), _i(0), _d(0.) {}
    Json(int i) : _type(eInt), _b(false), _i(i), _d(0.) {}
    Json(long i) : _type(eInt), _b(false), _i(i), _d(0.) {}
    Json(double d) : _type(eNumber), _b(false), _i(0), _d(d) {}
    Json(const char* s) : _type(s ? eString : eNull), _b(false), _i(0), _d(0.), _s(s ? s : "") {}
    Json(const std::string& s) : _type(eString), _b(false), _i(0), _d(0.), _s(s) {}

    static Json array() { Json j; j._type = eArray; return j; }
    static Json object() { Json j; j._type = eObject; return j; }

    /// append an item to an array (a null value becomes an empty array); returns the new item
    Json& push(const Json& v)
    {
      if (_type != eArray) {
        *this = array();
      }
      _items.push_back(v);
      return _items.back();
    }

    /// the member of an object with this key, added (as null) if it doesn't exist (a null value
    /// becomes an empty object)
    Json& operator[](const std::string& key)
    {
      if (_type != eObject) {
        *this = object();
      }
      std::list<Json>::iterator it = _items.begin();
      for (size_t i = 0; i < _keys.size(); ++i, ++it) {
        if (_keys[i] == key) {
          return *it;
        }
      }
      _keys.push_back(key);
      _items.push_back(Json());
      return _items.back();
    }

    /// the member of an object with this key, or null
    const Json& get(const std::string& key) const
    {
      static const Json null;
      if (_type != eObject) {
        return null;
      }
      std::list<Json>::const_iterator it = _items.begin();
      for (size_t i = 0; i < _keys.size(); ++i, ++it) {
        if (_keys[i] == key) {
          return *it;
        }
      }
      return null;
    }

    /// the i-th item of an array, or null
    const Json& at(size_t i) const
    {
      static const Json null;
      if (_type != eArray || i >= _items.size()) {
        return null;
      }
      std::list<Json>::const_iterator it = _items.begin();
      std::advance(it, (long)i);
      return *it;
    }

    /// write the value, indented by 2 spaces per level
    void write(std::ostream& os, int indent = 0) const
    {
      switch (_type) {
      case eNull:
        os << "null";
        break;
      case eBool:
        os << (_b ? "true" : "false");
        break;
      case eInt: {
        std::ostringstream ss;
        ss.imbue(std::locale::classic());
        ss << _i;
        os << ss.str();
        break;
      }
      case eNumber:
        writeNumber(os, _d);
        break;
      case eString:
        writeString(os, _s);
        break;
      case eArray:
      case eObject: {
        const bool obj = _type == eObject;
        if (_items.empty()) {
          os << (obj ? "{}" : "[]");
          break;
        }
        // arrays of numbers (rectangles, colours, ranges) stay on one line
        bool flat = !obj;
        for (std::list<Json>::const_iterator it = _items.begin(); flat && it != _items.end(); ++it) {
          flat = it->_type != eArray && it->_type != eObject && it->_type != eString;
        }
        os << (obj ? "{" : "[");
        std::list<Json>::const_iterator item = _items.begin();
        for (size_t i = 0; i < _items.size(); ++i, ++item) {
          if (i > 0) {
            os << (flat ? ", " : ",");
          }
          if (!flat) {
            os << "\n" << std::string((size_t)(indent + 1) * 2, ' ');
          }
          if (obj) {
            writeString(os, _keys[i]);
            os << ": ";
          }
          item->write(os, indent + 1);
        }
        if (!flat) {
          os << "\n" << std::string((size_t)indent * 2, ' ');
        }
        os << (obj ? "}" : "]");
        break;
      }
      }
    }

    /// as %.17g (the default floatfield with precision 17), in the classic locale; non-finite
    /// values are null
    static void writeNumber(std::ostream& os, double d)
    {
      if (d != d || d - d != 0.) { // NaN or infinite
        os << "null";
        return;
      }
      std::ostringstream ss;
      ss.imbue(std::locale::classic());
      ss.precision(17);
      ss << d;
      os << ss.str();
    }

    static void writeString(std::ostream& os, const std::string& s)
    {
      os << '"';
      const size_t n = s.size();
      for (size_t i = 0; i < n; ) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') {
          os << "\\\"";
        } else if (c == '\\') {
          os << "\\\\";
        } else if (c == '\n') {
          os << "\\n";
        } else if (c == '\r') {
          os << "\\r";
        } else if (c == '\t') {
          os << "\\t";
        } else if (c < 0x20 || c == 0x7f) {
          static const char hex[] = "0123456789abcdef";
          os << "\\u00" << hex[c >> 4] << hex[c & 0xf];
        } else if (c < 0x80) {
          os << (char)c;
        } else {
          // copy a valid UTF-8 sequence, else write U+FFFD for this byte
          const int len = utf8Length(s, i);
          if (len > 0) {
            os << s.substr(i, (size_t)len);
            i += (size_t)len;
            continue;
          }
          os << "\\ufffd";
        }
        ++i;
      }
      os << '"';
    }

  private:
    /// the length of the valid UTF-8 sequence (RFC 3629: no overlong forms, no surrogates, at most
    /// U+10FFFF) that starts at s[i], or 0
    static int utf8Length(const std::string& s, size_t i)
    {
      const unsigned char c = (unsigned char)s[i];
      int len;
      unsigned long cp;
      if (c >= 0xc2 && c <= 0xdf) {
        len = 2; cp = c & 0x1f;
      } else if (c >= 0xe0 && c <= 0xef) {
        len = 3; cp = c & 0x0f;
      } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4; cp = c & 0x07;
      } else {
        return 0;
      }
      if (i + (size_t)len > s.size()) {
        return 0;
      }
      for (int k = 1; k < len; ++k) {
        const unsigned char cc = (unsigned char)s[i + (size_t)k];
        if ((cc & 0xc0) != 0x80) {
          return 0;
        }
        cp = (cp << 6) | (cc & 0x3f);
      }
      if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
          (len == 4 && (cp < 0x10000 || cp > 0x10ffff))) {
        return 0;
      }
      return len;
    }

    Type _type;
    bool _b;
    long _i;
    double _d;
    std::string _s;
    std::vector<std::string> _keys; ///< for objects, the keys of _items
    // array items, or object values. push_back on a list keeps the references to the other items
    // valid, so a reference returned by push() or operator[] stays valid when items are added.
    // (A list of an incomplete type is allowed since C++17, and works with all C++98 libraries.)
    std::list<Json> _items;
  };

}

#endif // HOST_DEMO_JSON_H
