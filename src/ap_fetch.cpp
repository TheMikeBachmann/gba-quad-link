/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_fetch.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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

std::string installed_ap_version(const std::string& ap_dir) {
    if (ap_dir.empty()) return {};
    std::ifstream f(fs::path(ap_dir) / "manifest.json", std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (!json::parse(ss.str(), &v)) return {};
    const json::Value* ver = v.find("version");
    if (!ver || ver->type != json::Value::Type::Array || ver->array.empty())
        return {};
    std::string out;
    for (const json::Value& part : ver->array) {
        if (part.type != json::Value::Type::Number) return {};
        if (!out.empty()) out.push_back('.');
        out += std::to_string(static_cast<long long>(part.number));
    }
    return out;
}

namespace {

std::vector<long long> version_parts(const std::string& v) {
    std::vector<long long> out;
    std::size_t i = 0;
    while (i < v.size()) {
        // Anything that is not a digit ends a component, which copes with a
        // tag written as "0.6.7" and one written as "v0.6.7rc1".
        while (i < v.size() && !std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i >= v.size()) break;
        long long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            n = n * 10 + (v[i] - '0');
            ++i;
        }
        out.push_back(n);
    }
    return out;
}

}  // namespace

bool version_is_newer(const std::string& candidate, const std::string& current) {
    if (candidate.empty()) return false;
    if (current.empty()) return true;
    const auto a = version_parts(candidate);
    const auto b = version_parts(current);
    for (std::size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const long long x = i < a.size() ? a[i] : 0;
        const long long y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void migrate_ap_state(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;
    std::error_code ec;
    const fs::path src(from), dst(to);

    // Taken whole, because every one of these is something a release does not
    // ship and cannot recreate: the settings naming each world's cartridge,
    // community worlds, generated seeds, and Archipelago's own saved state.
    for (const char* name : {"host.yaml", "_persistent_storage.yaml"})
        if (fs::exists(src / name, ec))
            fs::copy(src / name, dst / name,
                     fs::copy_options::overwrite_existing, ec);

    for (const char* name : {"custom_worlds", "output"})
        if (fs::is_directory(src / name, ec))
            fs::copy(src / name, dst / name,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing, ec);

    // Players holds both the templates a release ships and the configuration
    // somebody wrote, in one folder. Only what the new copy does not already
    // have is theirs.
    fs::directory_iterator players(src / "Players", ec);
    if (!ec) {
        fs::create_directories(dst / "Players", ec);
        for (const fs::directory_entry& e : players) {
            const fs::path target = dst / "Players" / e.path().filename();
            if (fs::exists(target, ec)) continue;
            fs::copy(e.path(), target, fs::copy_options::recursive, ec);
        }
    }
}

bool find_ap_release(std::string* url, std::string* filename, std::string* tag,
                     std::string* err) {
    std::string body;
    if (!net::get_to_string(kReleases, &body, err)) return false;

    json::Value v;
    if (!json::parse(body, &v) || v.type != json::Value::Type::Object) {
        if (err) *err = "GitHub sent something unreadable";
        return false;
    }
    if (tag) *tag = v.str("tag_name");
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

ApLatest::~ApLatest() {
    if (thread_.joinable()) thread_.join();
}

void ApLatest::check() {
    if (started_.exchange(true)) return;
    thread_ = std::thread([this] {
        std::string url, name, tag, err;
        if (!find_ap_release(&url, &name, &tag, &err)) return;   // stays silent
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tag_ = tag;
        }
        done_.store(true);
    });
}

std::string ApLatest::tag() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tag_;
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

void ApInstall::start(const std::string& dest, const std::string& migrate_from) {
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
    thread_ = std::thread(&ApInstall::run, this, dest, migrate_from);
}

void ApInstall::run(std::string dest, std::string migrate_from) {
    const auto fail = [this](const std::string& why) {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = why;
        stage_.store(Stage::Failed);
    };
    const auto say = [this](const std::string& what) {
        std::lock_guard<std::mutex> lk(mutex_);
        note_ = what;
    };

    std::string url, name, tag, err;
    if (!find_ap_release(&url, &name, &tag, &err)) { fail(err); return; }
    if (cancel_.load()) { fail("cancelled"); return; }

    std::error_code ec;
    // Everything happens in a staging folder beside the destination, so what
    // is already installed keeps working until the new copy is complete. An
    // update that fails halfway through is the one outcome worth engineering
    // against: it would take somebody's configured install with it.
    const fs::path staging = fs::path(dest) / ".staging";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) { fail("cannot create " + staging.string()); return; }

    const fs::path file = staging / name;
    stage_.store(Stage::Downloading);
    say("downloading " + name);

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
    if (!ok) { fs::remove_all(staging, ec); fail(err); return; }

    stage_.store(Stage::Unpacking);
    progress_.store(-1.0f);
    say("unpacking");
    if (!extract(file, staging, &err)) {
        fs::remove_all(staging, ec);
        fail(err);
        return;
    }
    fs::remove(file, ec);

    const fs::path fresh = staging / "squashfs-root/opt/Archipelago";
    if (!fs::exists(fresh / "ArchipelagoBizHawkClient", ec)) {
        fs::remove_all(staging, ec);
        fail("unpacked, but there is no client in it");
        return;
    }

    if (!migrate_from.empty() && fs::exists(migrate_from, ec)) {
        stage_.store(Stage::Migrating);
        say("keeping your settings and worlds");
        migrate_ap_state(migrate_from, fresh.string());
    }

    // Swap it in. The old copy is moved aside rather than deleted first, so
    // there is never a moment with no install at all.
    const fs::path live = fs::path(dest) / "squashfs-root";
    const fs::path previous = fs::path(dest) / ".previous";
    fs::remove_all(previous, ec);
    if (fs::exists(live, ec)) {
        fs::rename(live, previous, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            fail("could not move the old copy aside");
            return;
        }
    }
    fs::rename(staging / "squashfs-root", live, ec);
    if (ec) {
        // Put back what was there.
        std::error_code ec2;
        if (fs::exists(previous, ec2)) fs::rename(previous, live, ec2);
        fs::remove_all(staging, ec);
        fail("could not put the new copy in place");
        return;
    }
    fs::remove_all(previous, ec);
    fs::remove_all(staging, ec);

    {
        std::lock_guard<std::mutex> lk(mutex_);
        installed_ = (live / "opt/Archipelago").string();
        note_ = (migrate_from.empty() ? "installed " : "updated to ") +
                (tag.empty() ? name : tag);
    }
    stage_.store(Stage::Done);
}

}  // namespace gql
