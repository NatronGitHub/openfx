#ifndef _ofxsLog_H_
#define _ofxsLog_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file This file contains OFX logging header code
*/

#include <string> // std::string (SF.11: the header includes what it uses)

/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {

  /** @brief this namespace wraps up logging functionality */
  namespace Log {
    /** @brief Indent it, per thread */
    void indent(void);

    /** @brief Outdent it, per thread */
    void outdent(void);

    /** @brief Sets the name of the log file. */
    void setFileName(const std::string &value);

    /** @brief Opens the log file, returns whether this was successful or not. */
    bool open(void);

    /** @brief Closes the log file. */
    void close(void);

    // format is a printf format: gcc and clang check the arguments, and warn about a non-literal
    // format with -Wformat-security (text should be passed as a "%s" argument)
#if defined(__GNUC__) || defined(__clang__)
#  define OFXS_LOG_PRINTF_FORMAT(f, a) __attribute__((format(printf, f, a)))
#else
#  define OFXS_LOG_PRINTF_FORMAT(f, a)
#endif

    /** @brief Prints to the log file. */
    void print(const char *format, ...) OFXS_LOG_PRINTF_FORMAT(1, 2);

    /** @brief Prints to the log file only if the condition is true and prepends a warning notice. */
    void warning(bool condition, const char *format, ...) OFXS_LOG_PRINTF_FORMAT(2, 3);

    /** @brief Prints to the log file only if the condition is true and prepends an error notice. */
    void error(bool condition, const char *format, ...) OFXS_LOG_PRINTF_FORMAT(2, 3);
  };
};

#endif
