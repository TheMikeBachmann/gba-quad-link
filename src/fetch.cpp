/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "fetch.h"

#include <curl/curl.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace gql::net {
namespace {

namespace fs = std::filesystem;

// Identifies itself because the GitHub API refuses a request that does not.
constexpr const char* kAgent = "gba-quad-link";

// Where the distributions keep their trust store. libcurl was built with one
// of these compiled in, and if this is a bundled copy it is probably not the
// one on the machine it is running on.
const char* ca_bundle() {
    static const char* const kPaths[] = {
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Arch, SteamOS
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // Alpine, and a common fallback
    };
    std::error_code ec;
    for (const char* p : kPaths)
        if (fs::exists(p, ec)) return p;
    return nullptr;
}

std::size_t to_file(char* data, std::size_t size, std::size_t n, void* out) {
    auto* f = static_cast<std::FILE*>(out);
    return std::fwrite(data, size, n, f);
}

std::size_t to_string(char* data, std::size_t size, std::size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

int on_progress(void* p, curl_off_t total, curl_off_t got, curl_off_t, curl_off_t) {
    const auto* cb = static_cast<const Progress*>(p);
    if (!*cb) return 0;
    return (*cb)(static_cast<long long>(got), static_cast<long long>(total))
               ? 0
               : 1;   // non-zero aborts the transfer
}

// curl_global_init is not thread safe and must happen once before anything
// else touches the library.
void init_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool run(const std::string& url, std::size_t (*writer)(char*, std::size_t,
                                                       std::size_t, void*),
         void* sink, const Progress* progress, std::string* err) {
    init_once();
    CURL* c = curl_easy_init();
    if (!c) {
        if (err) *err = "could not start a transfer";
        return false;
    }

    char error_buf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writer);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, sink);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buf);
    // This runs on a worker thread, and libcurl's timeout handling uses
    // signals unless told not to.
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    // No overall timeout: this is ninety megabytes over somebody's home
    // connection. Stalling is caught by the speed limit instead — nothing at
    // all for a minute is a dead transfer.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    if (const char* ca = ca_bundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    if (progress) {
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, progress);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    }

    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);

    if (rc == CURLE_OK) return true;
    if (err) {
        if (rc == CURLE_ABORTED_BY_CALLBACK) *err = "cancelled";
        else if (error_buf[0]) *err = error_buf;
        else *err = curl_easy_strerror(rc);
        if (status >= 400) *err += " (HTTP " + std::to_string(status) + ")";
    }
    return false;
}

}  // namespace

bool get_to_file(const std::string& url, const std::string& path,
                 const Progress& progress, std::string* err) {
    // Written to a partial name and renamed on success, so an interrupted
    // download is never mistaken for a finished one.
    const std::string partial = path + ".part";
    std::FILE* f = std::fopen(partial.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot write to " + path;
        return false;
    }
    const bool ok = run(url, to_file, f, &progress, err);
    std::fclose(f);

    std::error_code ec;
    if (!ok) {
        fs::remove(partial, ec);
        return false;
    }
    fs::rename(partial, path, ec);
    if (ec) {
        fs::remove(partial, ec);
        if (err) *err = "could not put the download in place";
        return false;
    }
    return true;
}

bool get_to_string(const std::string& url, std::string* out, std::string* err) {
    if (!out) return false;
    out->clear();
    return run(url, to_string, out, nullptr, err);
}

}  // namespace gql::net
