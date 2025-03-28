// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_I18N_ICU_UTIL_H_
#define BASE_I18N_ICU_UTIL_H_

#include <stdint.h>

#include "base/files/memory_mapped_file.h"
#include "base/i18n/base_i18n_export.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"

#if BUILDFLAG(IS_CHROMEOS_LACROS)
#include "icu_mergeable_data_file.h"
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)

#define ICU_UTIL_DATA_FILE 0
#define ICU_UTIL_DATA_STATIC 1

namespace base::i18n {

#if !BUILDFLAG(IS_NACL)
// Call this function to load ICU's data tables for the current process.  This
// function should be called before ICU is used.
BASE_I18N_EXPORT bool InitializeICU();

#if ICU_UTIL_DATA_IMPL == ICU_UTIL_DATA_FILE

// In ChromeOS, two versions of icudtl.dat are shipped separately with Ash and
// Lacros. Although the two files can differ, there's a big overlap in content.
// We can take advantage of that and save memory by scanning the two files and
// merging the pages that are in common.
#if BUILDFLAG(IS_CHROMEOS_LACROS)
using IcuDataFile = IcuMergeableDataFile;
#else
// Outside of Lacros, we simply memory map the ICU data file.
using IcuDataFile = MemoryMappedFile;
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)
// Returns the PlatformFile and Region that was initialized by InitializeICU().
// Use with InitializeICUWithFileDescriptor().
BASE_I18N_EXPORT PlatformFile
GetIcuDataFileHandle(MemoryMappedFile::Region* out_region);

// Loads ICU data file from file descriptor passed by browser process to
// initialize ICU in render processes.
BASE_I18N_EXPORT bool InitializeICUWithFileDescriptor(
    PlatformFile data_fd,
    const MemoryMappedFile::Region& data_region);

// Returns a void pointer to the memory mapped ICU data file.
//
// There are cases on Android where we would be unsafely reusing a file
// descriptor within the same process when initializing two copies of ICU from
// different binaries in the same address space. This returns an unowned
// pointer to the memory mapped icu data file; consumers copies of base must
// not outlive the copy of base that owns the memory mapped file.
BASE_I18N_EXPORT const uint8_t* GetRawIcuMemory();

// Initializes ICU memory
//
// This does nothing in component builds; this initialization should only be
// done in cases where there could be two copies of base in a single process in
// non-component builds. (The big example is standalone service libraries: the
// Service Manager will have a copy of base linked in, and the majority of
// service libraries will have base linked in but in non-component builds,
// these will be separate copies of base.)
BASE_I18N_EXPORT bool InitializeICUFromRawMemory(const uint8_t* raw_memory);

BASE_I18N_EXPORT void ResetGlobalsForTesting();

#if BUILDFLAG(IS_FUCHSIA)
// Overrides the directory used by ICU for external time zone data.
BASE_I18N_EXPORT void SetIcuTimeZoneDataDirForTesting(const char* dir);
#endif  // BUILDFLAG(IS_FUCHSIA)
#endif  // ICU_UTIL_DATA_IMPL == ICU_UTIL_DATA_FILE

// In a test binary, initialize functions might be called twice.
BASE_I18N_EXPORT void AllowMultipleInitializeCallsForTesting();
#endif  // !BUILDFLAG(IS_NACL)

}  // namespace base::i18n

#endif  // BASE_I18N_ICU_UTIL_H_
