// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenCLDevice.h: Device::compileProgram(), the OpenCL program suite (OfxOpenCLProgramSuiteV1).

#include "ofxhOpenCLDevice.h"
#include "ofxhOpenCLFunctions.h"

#include "ofxCore.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace OFX {
  namespace Host {
    namespace OpenCL {

      using namespace Detail;

      namespace {

        /// A cache file: this magic line, then the size of the key (8 bytes), the key, the size of
        /// the binary (8 bytes), its FNV-1a hash (8 bytes) and the binary. Every part is checked on
        /// read: a torn or damaged file is a miss, never a binary for the driver.
        const char kCacheMagic[] = "OFXHostSupportOpenCLProgram2\n";

        /// FNV-1a, 64 bits: the name of a cache file (from the key, which the file holds and which
        /// is compared: a collision only costs a build), and the checksum of the binary
        std::uint64_t fnv1a(const unsigned char *p, size_t n)
        {
          std::uint64_t h = 14695981039346656037ull;
          for (size_t i = 0; i < n; ++i) {
            h = (h ^ p[i]) * 1099511628211ull;
          }
          return h;
        }

        std::uint64_t fnv1a(const std::string &s)
        {
          return fnv1a(reinterpret_cast<const unsigned char *>(s.data()), s.size());
        }

        std::string cacheFile(const std::string &directory, const std::string &key)
        {
          char name[32];
          std::snprintf(name, sizeof(name), "%016llx.clbin", static_cast<unsigned long long>(fnv1a(key)));
          const char last = directory.empty() ? '/' : directory.back();
          return directory + (last == '/' || last == '\\' ? "" : "/") + name;
        }

#if defined(_WIN32)
        /// Hooks::programCacheDirectory is UTF-8: the wide path of the Windows API
        std::wstring widePath(const std::string &utf8)
        {
          const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
          if (n <= 0) {
            return std::wstring();
          }
          std::wstring w(static_cast<size_t>(n), L'\0');
          MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
          w.resize(static_cast<size_t>(n - 1));
          return w;
        }
#endif

        /// the whole file, false if it can't be read. On POSIX, not through a symbolic link, and only
        /// a regular file owned by the user (one someone else wrote before the directory was made
        /// private is refused: a file only costs a build)
        bool readFile(const std::string &path, std::vector<unsigned char> &bytes)
        {
          typedef std::unique_ptr<FILE, int (*)(FILE *)> File; // (closed on every path, R.1)
#if defined(_WIN32)
          File f(_wfopen(widePath(path).c_str(), L"rbN"), &std::fclose); // (N: not inherited)
#else
          const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
          if (fd < 0) {
            return false;
          }
          struct stat st;
          if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
            ::close(fd);
            return false;
          }
          File f(::fdopen(fd, "rb"), &std::fclose);
          if (!f) {
            ::close(fd);
          }
#endif
          if (!f) {
            return false;
          }
          unsigned char buffer[65536];
          size_t n = 0;
          while ((n = std::fread(buffer, 1, sizeof(buffer), f.get())) > 0) {
            bytes.insert(bytes.end(), buffer, buffer + n);
          }
          return !std::ferror(f.get());
        }

        std::uint64_t readU64(const unsigned char *p)
        {
          std::uint64_t v = 0;
          std::memcpy(&v, p, sizeof(v));
          return v;
        }

        void appendU64(std::vector<unsigned char> &bytes, std::uint64_t v)
        {
          const unsigned char *p = reinterpret_cast<const unsigned char *>(&v);
          bytes.insert(bytes.end(), p, p + sizeof(v));
        }

        /// the binary of key in directory, empty if none, another key's, or damaged
        std::vector<unsigned char> readCache(const std::string &directory, const std::string &key)
        {
          std::vector<unsigned char> f;
          if (!readFile(cacheFile(directory, key), f)) {
            return {};
          }
          const size_t magic = sizeof(kCacheMagic) - 1, u64 = sizeof(std::uint64_t);
          size_t at = 0;
          if (f.size() < magic + u64 || std::memcmp(f.data(), kCacheMagic, magic) != 0 ||
              readU64(&f[magic]) != key.size()) {
            return {};
          }
          at = magic + u64;
          if (f.size() - at < key.size() + 2 * u64 || std::memcmp(&f[at], key.data(), key.size()) != 0) {
            return {};
          }
          at += key.size();
          const std::uint64_t size = readU64(&f[at]), hash = readU64(&f[at + u64]);
          at += 2 * u64;
          if (size == 0 || f.size() - at != size || fnv1a(&f[at], f.size() - at) != hash) {
            return {}; // torn (a short write, a crash) or damaged
          }
          return std::vector<unsigned char>(f.begin() + static_cast<std::ptrdiff_t>(at), f.end());
        }

        /// a temporary file name no other thread or process uses
        std::string temporaryName(const std::string &path)
        {
          static std::atomic<unsigned long> counter{ 0 };
#if defined(_WIN32)
          const unsigned long pid = GetCurrentProcessId();
#else
          const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
          return path + ".tmp." + std::to_string(pid) + "." + std::to_string(++counter);
        }

        /// false if it can't be written (the cache is an optimization: the caller goes on)
        bool writeCache(const std::string &directory, const std::string &key, const std::vector<unsigned char> &binary)
        {
          std::vector<unsigned char> bytes(kCacheMagic, kCacheMagic + sizeof(kCacheMagic) - 1);
          appendU64(bytes, key.size());
          bytes.insert(bytes.end(), key.begin(), key.end());
          appendU64(bytes, binary.size());
          appendU64(bytes, fnv1a(binary.data(), binary.size()));
          bytes.insert(bytes.end(), binary.begin(), binary.end());
          // written to a new file (O_EXCL, readable by the user only), then renamed over the old
          // one: a reader never sees half a file
          const std::string path = cacheFile(directory, key);
          const std::string tmp = temporaryName(path);
#if defined(_WIN32)
          const std::wstring wtmp = widePath(tmp);
          const int fd = _wopen(wtmp.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                                _S_IREAD | _S_IWRITE);
#else
          // (not inherited by a process the host starts meanwhile)
          const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
          if (fd < 0) {
            return false;
          }
          bool ok = true;
          size_t done = 0;
          while (ok && done < bytes.size()) {
#if defined(_WIN32)
            const int n = _write(fd, bytes.data() + done, static_cast<unsigned int>(std::min<size_t>(bytes.size() - done, 1u << 30)));
#else
            const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
#endif
            ok = n > 0;
            done += ok ? static_cast<size_t>(n) : 0;
          }
#if defined(_WIN32)
          ok = _close(fd) == 0 && ok;
          ok = ok && MoveFileExW(wtmp.c_str(), widePath(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
          if (!ok) {
            _wremove(wtmp.c_str());
          }
#else
          ok = ::close(fd) == 0 && ok; // (close() may report a write error)
          ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
          if (!ok) {
            std::remove(tmp.c_str());
          }
#endif
          return ok;
        }

      } // namespace

      namespace Detail {

        std::string privateCacheDirectory(const Hooks &hooks)
        {
          if (hooks.programCacheDirectory.empty()) {
            return std::string();
          }
          // the path checked is the one used: without its trailing separators, since lstat("link/")
          // and lstat("link/.") follow a final symbolic link (a lone "/" stays)
#if defined(_WIN32)
          const char *const separators = "/\\";
#else
          const char *const separators = "/";
#endif
          std::string dir = hooks.programCacheDirectory;
          while (dir.size() > 1 && std::strchr(separators, dir.back())) {
            dir.pop_back();
          }
          const size_t slash = dir.find_last_of(separators);
          const std::string last = slash == std::string::npos ? dir : dir.substr(slash + 1);
          std::string why;
          if (last == "." || last == "..") {
            why = "a path ending in \"" + last + "\" (it would resolve a symbolic link)";
          }
#if defined(_WIN32)
          else {
            // (no cheap equivalent of the owner and mode check: the host keeps the directory private)
            const DWORD attributes = GetFileAttributesW(widePath(dir).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
              why = "not a directory";
            }
          }
#else
          // a binary can carry native code (CPU devices, e.g. PoCL): whoever can write to the
          // directory could run code in the host, so it must be the user's own, not group- or
          // world-writable
          // lstat(): a symbolic link would be resolved again at each open, so the check wouldn't
          // hold (it could be repointed after this); refused, as is a link's target
          struct stat st;
          if (!why.empty()) {
            // (refused above)
          } else if (::lstat(dir.c_str(), &st) != 0) {
            why = "not a directory";
          } else if (S_ISLNK(st.st_mode)) {
            why = "a symbolic link";
          } else if (!S_ISDIR(st.st_mode)) {
            why = "not a directory";
          } else if (st.st_uid != geteuid()) {
            why = "not owned by the user";
          } else if (st.st_mode & (S_IWGRP | S_IWOTH)) {
            why = "writable by other users";
          }
#endif
          if (why.empty()) {
            return dir;
          }
          if (hooks.log) {
            hooks.log(LogLevel::Warning, "OpenCL: the program cache directory " + hooks.programCacheDirectory + " is " + why +
                      ": the program binaries are kept in memory only");
          }
          return std::string();
        }

      } // Detail

      Device::ProgramCacheStats Device::programCacheStats() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _programStats;
      }

      OfxStatus Device::compileProgram(void *queue, const char *pszProgramSource, int fOptional, void *pResult)
      {
        if (!pszProgramSource || !pResult) {
          if (pResult) {
            *static_cast<cl_program *>(pResult) = nullptr; // ("NULL unless kOfxStatOK")
          }
          return kOfxStatErrBadHandle;
        }
        cl_program &result = *static_cast<cl_program *>(pResult);
        result = nullptr;
        if (!queue) {
          // the program must be built for the context of the render's queue ("The OpenCL context can
          // be found from the command queue using clGetCommandQueueInfo with CL_QUEUE_CONTEXT"),
          // and there is none
          if (fOptional) {
            return kOfxStatReplyDefault; // "if non-zero, host may skip compiling on this call"
          }
          log(LogLevel::Error, "OpenCL: compileProgram outside an OpenCL render: no context to build the program for");
          return kOfxStatFailed;
        }
        const CLFunctions &cl = *_cl;
        cl_context context = nullptr;
        cl_int err = kCLSuccess;
        driverCall([&] {
          err = cl.GetCommandQueueInfo(static_cast<cl_command_queue>(queue), kCLQueueContext, sizeof(context), &context, nullptr);
        });
        if (err != kCLSuccess || context != _context) {
          log(LogLevel::Error, "OpenCL: compileProgram: the render's command queue isn't of this device's context (" +
              errorName(err) + ")");
          return kOfxStatFailed;
        }

        // the key: the OpenCL environment (platform, device, driver), the options and the source
        const std::string &options = _hooks.buildOptions;
        std::string key = _info.platformName + '\n' + _info.platformVersion + '\n' + _info.name + '\n' + _info.vendor + '\n' +
          _info.version + '\n' + _info.driverVersion + '\n' + options + '\n';
        key += pszProgramSource;

        std::vector<unsigned char> binary;
        bool fromFile = false;
        {
          std::lock_guard<std::mutex> lock(_mutex);
          auto it = _binaries.find(key);
          if (it != _binaries.end()) {
            binary = it->second;
          }
        }
        if (binary.empty() && !_cacheDirectory.empty()) {
          binary = readCache(_cacheDirectory, key);
          fromFile = !binary.empty();
        }

        cl_device_id device = static_cast<cl_device_id>(_device);
        cl_program program = nullptr;
        auto build = [&]() {
          err = cl.BuildProgram(program, 1, &device, options.c_str(), nullptr, nullptr);
          if (err != kCLSuccess) {
            size_t n = 0;
            std::string buildLog;
            if (cl.GetProgramBuildInfo(program, device, kCLProgramBuildLog, 0, nullptr, &n) == kCLSuccess && n > 1) {
              buildLog.assign(n, '\0');
              cl.GetProgramBuildInfo(program, device, kCLProgramBuildLog, n, &buildLog[0], nullptr);
              buildLog.resize(n - 1);
            }
            cl.ReleaseProgram(program);
            program = nullptr;
            return buildLog;
          }
          return std::string();
        };
        if (!binary.empty()) {
          driverCall([&] {
            const size_t size = binary.size();
            const unsigned char *data = binary.data();
            cl_int binaryStatus = kCLSuccess;
            program = cl.CreateProgramWithBinary(static_cast<cl_context>(_context), 1, &device, &size, &data, &binaryStatus, &err);
            if (program && (err != kCLSuccess || binaryStatus != kCLSuccess)) {
              cl.ReleaseProgram(program);
              program = nullptr;
            }
            if (program) {
              build();
            }
          });
          if (program) {
            std::lock_guard<std::mutex> lock(_mutex);
            ++(fromFile ? _programStats.fileHits : _programStats.memoryHits);
            if (fromFile) {
              _binaries[key] = binary;
            }
          } else {
            // (a binary the driver refuses: built again from the source, and replaced)
            log(LogLevel::Warning, "OpenCL: compileProgram: a cached binary was refused (" + errorName(err) + "), building the source");
          }
        }
        if (!program) {
          std::string buildLog;
          driverCall([&] {
            program = cl.CreateProgramWithSource(static_cast<cl_context>(_context), 1, &pszProgramSource, nullptr, &err);
            if (program) {
              buildLog = build();
            }
          });
          if (!program) {
            log(LogLevel::Error, "OpenCL: compileProgram: " + errorName(err) + (buildLog.empty() ? std::string() : ":\n" + buildLog));
            return kOfxStatFailed;
          }
          // the binary, for the next calls
          std::vector<unsigned char> built;
          driverCall([&] {
            size_t size = 0;
            if (cl.GetProgramInfo(program, kCLProgramBinarySizes, sizeof(size), &size, nullptr) == kCLSuccess && size > 0) {
              built.resize(size);
              unsigned char *data = built.data();
              if (cl.GetProgramInfo(program, kCLProgramBinaries, sizeof(data), &data, nullptr) != kCLSuccess) {
                built.clear();
              }
            }
          });
          {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_programStats.builds;
            if (!built.empty()) {
              _binaries[key] = built;
            }
          }
          if (!built.empty() && !_cacheDirectory.empty() && !writeCache(_cacheDirectory, key, built)) {
            log(LogLevel::Warning, "OpenCL: compileProgram: the binary couldn't be written to " + _cacheDirectory);
          }
        }
        result = program;
        return kOfxStatOK;
      }

    } // OpenCL
  } // Host
} // OFX
