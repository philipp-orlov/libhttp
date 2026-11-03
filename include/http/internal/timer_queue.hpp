// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Moved to libcore's libsched (sched/timer_heap.hpp).
#include <functional>

#include "sched/timer_heap.hpp"

namespace http::internal {
using TimerQueue = sched::BasicTimerHeap<std::function<void()>>;
}
