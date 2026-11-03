// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Platform detection and socket primitives. They live in libcore's libsys (sys/net.hpp); this header keeps the
// old path and the http:: names.
#if defined(_WIN32)
#define HTTP_WINDOWS 1
#else
#define HTTP_LINUX 1
#endif

#include "sys/net.hpp"

namespace http {
using namespace sys;
}
