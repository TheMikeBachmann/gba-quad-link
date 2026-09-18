/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// fetch.h — the little bit of HTTP this needs, which is one GET.
//
// Linked against libcurl rather than shelling out to the curl binary, so an
// AppImage carries its own and does not depend on what the host happens to
// have installed. That choice has one consequence worth knowing: the path to
// the system's CA certificates is compiled into libcurl, and the path is not
// the same on every distribution, so a copy built here would fail to verify
// anything on a machine that keeps them somewhere else. The implementation
// looks for them at run time instead.

#pragma once

#include <functional>
#include <string>

namespace gql::net {

// Called as the transfer runs. Returning false cancels it. `total` is zero
// while the server has not said how big the thing is.
using Progress = std::function<bool(long long got, long long total)>;

// Both follow redirects — GitHub answers a release asset with one — and both
// treat any HTTP status of 400 or above as a failure with a readable reason.
bool get_to_file(const std::string& url, const std::string& path,
                 const Progress& progress, std::string* err);
bool get_to_string(const std::string& url, std::string* out, std::string* err);

}  // namespace gql::net
