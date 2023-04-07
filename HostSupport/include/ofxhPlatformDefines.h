#ifndef OFXH_PLATFORM_DEFINES_H
#define OFXH_PLATFORM_DEFINES_H

#if defined(_WIN32) || defined(_WIN64)
#ifndef WINDOWS
#define WINDOWS
#endif
#elif defined(__linux__) || defined(__FreeBSD__) || defined( __APPLE__) || defined(unix) || defined(__unix) || defined(_XOPEN_SOURCE) || defined(_POSIX_SOURCE)
#define UNIX
#else
#error cannot detect operating system
#endif

#endif