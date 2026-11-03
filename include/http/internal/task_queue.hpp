// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Moved to libcore's libsched (sched/task_queue.hpp).
#include <functional>

#include "sched/task_queue.hpp"

namespace http::internal {
using TaskQueue = sched::BasicTaskQueue<std::function<void()>>;
}
