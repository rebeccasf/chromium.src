// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/chrome_paths_internal.h"

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/win/scoped_co_mem.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/install_static/install_util.h"
#include "components/nacl/common/nacl_switches.h"

#include "content/nw/src/nw_base.h"

namespace chrome {

namespace {

// Generic function to call SHGetFolderPath().
bool GetUserDirectory(int csidl_folder, base::FilePath* result) {
  // We need to go compute the value. It would be nice to support paths
  // with names longer than MAX_PATH, but the system functions don't seem
  // to be designed for it either, with the exception of GetTempPath
  // (but other things will surely break if the temp path is too long,
  // so we don't bother handling it.
  wchar_t path_buf[MAX_PATH];
  path_buf[0] = 0;
  if (FAILED(SHGetFolderPath(NULL, csidl_folder, NULL,
                             SHGFP_TYPE_CURRENT, path_buf))) {
    return false;
  }
  *result = base::FilePath(path_buf);
  return true;
}

}  // namespace

bool GetDefaultUserDataDirectory(base::FilePath* result) {
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, result))
    return false;
  if (nw::package()) //FIXME: crashpad initialized early in cr49
    *result = result->Append(base::FilePath::FromUTF8Unsafe(nw::package()->GetName()));
  *result = result->Append(chrome::kUserDataDirname);
  return true;
}

bool GetDefaultRoamingUserDataDirectory(base::FilePath* result) {
  if (!base::PathService::Get(base::DIR_ROAMING_APP_DATA, result))
    return false;
  *result = result->Append(install_static::GetChromeInstallSubDirectory());
  *result = result->Append(chrome::kUserDataDirname);
  return true;
}

void GetUserCacheDirectory(const base::FilePath& profile_dir,
                           base::FilePath* result) {
  // This function does more complicated things on Mac/Linux.
  *result = profile_dir;
}

bool GetUserDocumentsDirectory(base::FilePath* result) {
  return GetUserDirectory(CSIDL_MYDOCUMENTS, result);
}

// Return a default path for downloads that is safe.
// We just use 'Downloads' under DIR_USER_DOCUMENTS. Localizing
// 'downloads' is not a good idea because Chrome's UI language
// can be changed.
bool GetUserDownloadsDirectorySafe(base::FilePath* result) {
  if (!GetUserDocumentsDirectory(result))
    return false;

  *result = result->Append(L"Downloads");
  return true;
}

// On Vista and higher, use the downloads known folder. Since it can be
// relocated to point to a "dangerous" folder, callers should validate that the
// returned path is not dangerous before using it.
bool GetUserDownloadsDirectory(base::FilePath* result) {
  typedef HRESULT (WINAPI *GetKnownFolderPath)(
      REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
  GetKnownFolderPath f = reinterpret_cast<GetKnownFolderPath>(
      GetProcAddress(GetModuleHandle(L"shell32.dll"), "SHGetKnownFolderPath"));
  base::win::ScopedCoMem<wchar_t> path_buf;
  if (f && SUCCEEDED(f(FOLDERID_Downloads, 0, NULL, &path_buf))) {
    *result = base::FilePath(std::wstring(path_buf));
    return true;
  }
  return GetUserDownloadsDirectorySafe(result);
}

bool GetUserMusicDirectory(base::FilePath* result) {
  return GetUserDirectory(CSIDL_MYMUSIC, result);
}

bool GetUserPicturesDirectory(base::FilePath* result) {
  return GetUserDirectory(CSIDL_MYPICTURES, result);
}

bool GetUserVideosDirectory(base::FilePath* result) {
  return GetUserDirectory(CSIDL_MYVIDEO, result);
}

bool ProcessNeedsProfileDir(const std::string& process_type) {
  return install_static::ProcessNeedsProfileDir(process_type);
}

}  // namespace chrome
