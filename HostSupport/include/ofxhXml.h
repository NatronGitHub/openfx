#ifndef OFX_XML_H
#define OFX_XML_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <string>
#include <sstream> // stringstream

namespace OFX {

  namespace XML {

    /// append the character reference "&#xHH;" for code point c
    inline void appendCharRef(std::string &ns, unsigned int c) {
      static const char hex[] = "0123456789ABCDEF";
      char buf[8];
      int n = 0;
      do {
        buf[n++] = hex[c & 0xf];
        c >>= 4;
      } while (c != 0 && n < 8);
      ns += "&#x";
      while (n > 0) {
        ns += buf[--n];
      }
      ns += ';';
    }

    /// length of the well-formed UTF-8 sequence starting at s[i], or 0 if
    /// s[i] does not start a well-formed sequence (RFC 3629: no overlong forms,
    /// no surrogates, nothing above U+10FFFF, no truncated sequence).
    inline size_t utf8SequenceLength(const std::string &s, size_t i) {
      const unsigned char c = (unsigned char)s[i];
      size_t len;
      unsigned char lo = 0x80, hi = 0xBF; // valid range for the second byte
      if (0xC2 <= c && c <= 0xDF) {
        len = 2;
      } else if (0xE0 <= c && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;      // overlong
        else if (c == 0xED) hi = 0x9F; // surrogates
      } else if (0xF0 <= c && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;      // overlong
        else if (c == 0xF4) hi = 0x8F; // > U+10FFFF
      } else {
        return 0;
      }
      if (i + len > s.size()) {
        return 0;
      }
      const unsigned char c1 = (unsigned char)s[i + 1];
      if (c1 < lo || c1 > hi) {
        return 0;
      }
      for (size_t k = 2; k < len; ++k) {
        const unsigned char ck = (unsigned char)s[i + k];
        if (ck < 0x80 || ck > 0xBF) {
          return 0;
        }
      }
      return len;
    }

    /// Escape a UTF-8 string so that it can be used as XML 1.0 character data
    /// or attribute value.
    ///
    /// Well-formed UTF-8 sequences are copied as is. Bytes that are not part of a
    /// well-formed UTF-8 sequence are assumed to be ISO-8859-1 (Latin-1) and are
    /// written as character references, so that the output is always valid
    /// UTF-8. C0 control characters other than tab, line feed and carriage return
    /// cannot be represented in XML 1.0 (not even as character references) and
    /// are replaced by U+FFFD, as are the noncharacters U+FFFE and U+FFFF.
    inline std::string escape(const std::string &s) {
      std::string ns;
      for (size_t i=0;i<s.size();i++) {
        // The are exactly five characters which must be escaped
        // http://www.w3.org/TR/xml/#syntax
        switch (s[i]) {
          case '<':
            ns += "&lt;";
            break;
          case '>':
            ns += "&gt;";
            break;
          case '&':
            ns += "&amp;";
            break;
          case '"':
            ns += "&quot;";
            break;
          case '\'':
            ns += "&apos;";
            break;
          default: {
            unsigned char c = (unsigned char)(s[i]);
            if (c == 0x09 || c == 0x0A || c == 0x0D) {
              // Escape even the whitespace characters '\n' '\r' '\t', although they are valid
              // XML, because they would be converted to space when re-read.
              // See http://www.w3.org/TR/xml/#AVNormalize
              appendCharRef(ns, c);
            } else if (c < 0x20) {
              // Not a legal XML 1.0 Char, even as a character reference.
              // http://www.w3.org/TR/xml/#charsets
              appendCharRef(ns, 0xFFFD);
            } else if (c == 0x7F) {
              // DEL: legal, but discouraged http://www.w3.org/TR/xml/#charsets
              appendCharRef(ns, c);
            } else if (c < 0x80) {
              ns += s[i];
            } else {
              size_t len = utf8SequenceLength(s, i);
              if (len == 3 && c == 0xEF && (unsigned char)s[i + 1] == 0xBF &&
                  ((unsigned char)s[i + 2] == 0xBE || (unsigned char)s[i + 2] == 0xBF)) {
                // U+FFFE and U+FFFF are valid UTF-8 but not XML 1.0 Chars
                appendCharRef(ns, 0xFFFD);
                i += len - 1;
              } else if (len > 0) {
                ns.append(s, i, len);
                i += len - 1;
              } else {
                // not UTF-8: assume Latin-1, the code point is the byte value
                appendCharRef(ns, c);
              }
            }
          } break;
        }
      }
      return ns;
    }

    inline std::string attribute(const std::string &at, const std::string &val)
    {
      return at + "=" + "\"" + escape(val) + "\" ";
    }

    inline std::string attribute(const std::string &st, int val)
    {
      std::ostringstream o;
      o << val;
      return attribute(st, o.str());
    }

  }
}

#endif
