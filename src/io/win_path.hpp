// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#ifdef _WIN32

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace cyclone {

// Converts a UTF-8 path (the encoding embedders pass in) to UTF-16 for the
// wide Win32 file APIs, so non-ASCII directory and file names survive instead
// of being mangled through the ANSI code page. Returns false when the input is
// not valid UTF-8; callers then fall back to the ANSI API with the original
// bytes.
inline bool utf8_path_to_wide(const std::string& utf8, std::wstring& out) {
  out.clear();
  if (utf8.empty()) {
    return true;
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                          static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return false;
  }
  out.resize(static_cast<size_t>(needed));
  const int written =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                          static_cast<int>(utf8.size()), out.data(), needed);
  if (written != needed) {
    out.clear();
    return false;
  }
  return true;
}

// CreateFile for a UTF-8 path: CreateFileW when the path converts, CreateFileA
// with the original bytes otherwise. Flags, share mode and disposition are the
// caller's, unchanged.
inline HANDLE create_file_utf8(const std::string& utf8_path, DWORD access,
                               DWORD share, DWORD creation, DWORD attributes) {
  std::wstring wide;
  if (utf8_path_to_wide(utf8_path, wide)) {
    return CreateFileW(wide.c_str(), access, share, nullptr, creation,
                       attributes, nullptr);
  }
  return CreateFileA(utf8_path.c_str(), access, share, nullptr, creation,
                     attributes, nullptr);
}

}  // namespace cyclone

#endif  // _WIN32
