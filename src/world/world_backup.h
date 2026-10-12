#pragma once
// LevelStorageAccess.makeWorldBackup: the save zipped to <backups>/<yyyy-MM-dd_HH-mm-ss>_<folder>.zip, entries
// "<folder>/<path>" without session.lock (Edit World's Make Backup, Recover World).
#include "core/common.h"

#include <optional>

// The zip's size in bytes, or nothing when it could not be written; `zip` receives its path.
std::optional<u64> makeWorldBackup(const fs::path& world, const fs::path& backups, fs::path* zip = nullptr);
