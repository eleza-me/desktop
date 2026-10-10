// Names and path rules shared by everything that touches an installed copy of
// the app: the unpacker (what an archive entry may be called), the swap (what
// it moves) and the Windows updater. No Qt here: the updater links nothing but
// the C++ runtime.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Install {

using Path = std::filesystem::path;

// The list of the files of a package (one relative path per line), shipped in
// the package root. Its presence next to the executable marks a copy that
// updates itself; the next update removes what the list names and the new
// package no longer has.
inline constexpr auto kFileListName = "eleza-files.list";
// Written into the install directory while a swap is running (see
// install_swap.h); a start that finds it finishes or undoes the swap first.
inline constexpr auto kJournalName = "_update_journal.txt";
// <install>/_backup_<version>/: the files of the old version during a swap.
inline constexpr auto kBackupPrefix = "_backup_";
// <install>/_update_<version>/: the new files, brought to the volume of the
// install directory before anything of the old version is touched.
inline constexpr auto kIncomingPrefix = "_update_";

// A '/'-separated relative path (UTF-8) that is safe to create under a
// directory of ours on every platform we ship for:
//   * not empty, not absolute, no drive letter or stream (':'), no '\';
//   * no "." / ".." / empty components;
//   * no control characters, none of < > " | ? *;
//   * no component ending with a dot or a space (Windows drops them);
//   * no reserved device name (CON, NUL, COM1…, with or without extension);
//   * well-formed UTF-8, at most 255 bytes per component and 1024 in all;
//   * nothing under the names the swap keeps for itself (_backup_…,
//     _update_…, the journal).
// A trailing '/' (a directory entry of an archive) is not accepted: strip it
// first.
[[nodiscard]] bool IsSafeRelativePath(std::string_view path);

// "0.2.0", "1.10.3": what may become a part of a directory name.
[[nodiscard]] bool IsSafeVersion(std::string_view version);

[[nodiscard]] Path PathFromUtf8(std::string_view text);
[[nodiscard]] std::string PathToUtf8(const Path &path);

} // namespace Install
