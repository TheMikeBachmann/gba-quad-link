/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_fetch.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>

#include "fetch.h"
#include "minijson.h"

namespace gql {
namespace {

namespace fs = std::filesystem;

constexpr const char* kReleases =
    "https://api.github.com/repos/ArchipelagoMW/Archipelago/releases/latest";

// The asset built for this kind of machine. The release also carries an
// installer for Windows, an Android package and a tarball; the AppImage is
// the one that unpacks into the layout the rest of this already looks for.
bool wanted_asset(const std::string& name) {
    return name.size() > 9 &&
           name.compare(name.size() - 9, 9, ".AppImage") == 0 &&
           name.find("linux") != std::string::npos &&
           name.find("x86_64") != std::string::npos;
}

// Runs the downloaded AppImage to unpack itself. --appimage-extract is served
// by the runtime embedded in the file and needs no FUSE, which matters
// because SteamOS does not ship libfuse2.
bool extract(const fs::path& appimage, const fs::path& into, std::string* err) {
    std::error_code ec;
    fs::permissions(appimage, fs::perms::owner_all, ec);
    if (ec) { if (err) *err = "could not make the download executable"; return false; }

    const pid_t pid = ::fork();
    if (pid < 0) { if (err) *err = "fork failed"; return false; }
    if (pid == 0) {
        if (::chdir(into.c_str()) != 0) ::_exit(127);
        // Its output is noise on success and is reported by the exit status
        // on failure.
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) { ::dup2(null, STDOUT_FILENO); ::dup2(null, STDERR_FILENO); }
        ::execl(appimage.c_str(), appimage.c_str(), "--appimage-extract",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (err) *err = "the download would not unpack";
        return false;
    }
    return true;
}

}  // namespace

bool find_ap_release(std::string* url, std::string* filename, std::string* err) {
    std::string body;
    if (!net::get_to_string(kReleases, &body, err)) return false;

    json::Value v;
    if (!json::parse(body, &v) || v.type != json::Value::Type::Object) {
        if (err) *err = "GitHub sent something unreadable";
        return false;
    }
    const json::Value* assets = v.find("assets");
    if (!assets || assets->type != json::Value::Type::Array) {
        if (err) *err = "that release lists no downloads";
        return false;
    }
    for (const json::Value& a : assets->array) {
        const std::string name = a.str("name");
        if (!wanted_asset(name)) continue;
        const std::string href = a.str("browser_download_url");
        if (href.empty()) continue;
        if (url) *url = href;
        if (filename) *filename = name;
        return true;
    }
    if (err) *err = "that release has no Linux build";
    return false;
}

ApInstall::~ApInstall() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

std::string ApInstall::note() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return note_;
}

std::string ApInstall::installed() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return installed_;
}

bool ApInstall::busy() const {
    const Stage s = stage();
    return s == Stage::Asking || s == Stage::Downloading || s == Stage::Unpacking;
}

void ApInstall::cancel() { cancel_.store(true); }

void ApInstall::start(const std::string& dest) {
    if (busy()) return;
    if (thread_.joinable()) thread_.join();
    cancel_.store(false);
    progress_.store(-1.0f);
    stage_.store(Stage::Asking);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = "asking GitHub for the latest release";
        installed_.clear();
    }
    thread_ = std::thread(&ApInstall::run, this, dest);
}

void ApInstall::run(std::string dest) {
    const auto fail = [this](const std::string& why) {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = why;
        stage_.store(Stage::Failed);
    };

    std::string url, name, err;
    if (!find_ap_release(&url, &name, &err)) { fail(err); return; }
    if (cancel_.load()) { fail("cancelled"); return; }

    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) { fail("cannot create " + dest); return; }

    const fs::path file = fs::path(dest) / name;
    stage_.store(Stage::Downloading);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = "downloading " + name;
    }

    const bool ok = net::get_to_file(
        url, file.string(),
        [this](long long got, long long total) {
            if (cancel_.load(std::memory_order_relaxed)) return false;
            progress_.store(total > 0 ? static_cast<float>(
                                            static_cast<double>(got) /
                                            static_cast<double>(total))
                                      : -1.0f,
                            std::memory_order_relaxed);
            return true;
        },
        &err);
    if (!ok) { fail(err); return; }

    stage_.store(Stage::Unpacking);
    progress_.store(-1.0f);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = "unpacking";
    }
    if (!extract(file, dest, &err)) { fail(err); return; }

    // Where --appimage-extract puts it, which is the layout find_ap_install()
    // already knows how to recognise.
    const fs::path home = fs::path(dest) / "squashfs-root/opt/Archipelago";
    if (!fs::exists(home / "ArchipelagoBizHawkClient", ec)) {
        fail("unpacked, but there is no client in it");
        return;
    }
    // The AppImage itself is three hundred megabytes of no further use once
    // its contents are on disk.
    fs::remove(file, ec);

    {
        std::lock_guard<std::mutex> lk(mutex_);
        installed_ = home.string();
        note_ = "installed " + name;
    }
    stage_.store(Stage::Done);
}

}  // namespace gql
