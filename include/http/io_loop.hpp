// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The reactor loops (epoll, io_uring, IOCP) live in develop-core's libnet; this header keeps the old path and the
// http:: names.
#include "http/platform.hpp"
#include "net/io_loop.hpp"

namespace http {
using net::AcceptCallback;
using net::IoCallback;
using net::IoLoop;
using net::IoResult;
using net::TcpConnection;
using net::TcpListener;
using net::TimerCallback;
}  // namespace http
