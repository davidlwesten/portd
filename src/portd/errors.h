#pragma once

#include <errno.h>

[[nodiscard]] static inline int errno_or(int r) { return r < 0 ? -errno : r; }
