/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_session.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>

namespace gql {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRecentLines = 60;

// Shared by every session, so lines from different players can be ordered
// against each other.
std::atomic<unsigned long long> g_line_seq{0};
constexpr const char* kClient = "ArchipelagoBizHawkClient";

// Noise the frozen build prints on the way up, every time, which would
// otherwise be the first thing anyone saw in the status line.
bool boring(const std::string& line) {
    static const char* const kNoise[] = {
        "UserWarning", "RequestsDependencyWarning", "pkg_resources",
        "logging initialized", "warnings.warn",
    };
    for (const char* n : kNoise)
        if (line.find(n) != std::string::npos) return true;
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

// What a line means, coarsely. These are log messages, not an interface, so
// nothing here assumes more than a substring and everything falls through to
// leaving the status alone.
const char* classify(const std::string& line) {
    if (line.find("has joined") != std::string::npos) return "in multiworld";
    if (line.find("Connected to BizHawk") != std::string::npos) return "attached";
    if (line.find("Running handler") != std::string::npos) return "attached";
    if (line.find("Waiting to connect to BizHawk") != std::string::npos) return "waiting for game";
    if (line.find("Connecting to Archipelago") != std::string::npos) return "connecting";
    if (line.find("Please connect") != std::string::npos) return "needs server";
    if (line.find("Lost connection") != std::string::npos) return "lost connection";
    if (line.find("Error") != std::string::npos ||
        line.find("Traceback") != std::string::npos ||
        line.find("FileNotFoundError") != std::string::npos) return "error";
    return nullptr;
}

}  // namespace

ApSession::~ApSession() { stop(); }

bool ApSession::start(const std::string& ap_dir, const std::string& patch,
                      const std::string& server, const std::string& expected_rom,
                      std::string* err) {
    stop();

    const fs::path exe = fs::path(ap_dir) / kClient;
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        if (err) *err = std::string(kClient) + " not found in " + ap_dir;
        return false;
    }
    if (!fs::exists(patch, ec)) {
        if (err) *err = "patch file is gone";
        return false;
    }

    int in_pipe[2], out_pipe[2];
    if (::pipe(in_pipe) != 0) { if (err) *err = "pipe failed"; return false; }
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        if (err) *err = "pipe failed";
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (err) *err = "fork failed";
        return false;
    }

    if (pid == 0) {
        // Child. Its own process group, so stopping it stops whatever it
        // starts; an orphan here holds a port the next run needs.
        ::setpgid(0, 0);
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(out_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        // The frozen build finds its own libraries and settings relative to
        // where it sits.
        if (::chdir(ap_dir.c_str()) != 0) ::_exit(127);
        // argv[0] must be the full path. A frozen build locates its own
        // libraries and settings from it, and given a bare name it looks along
        // PATH, does not find itself, and dies with "Unable to locate
        // executable on PATH".
        ::execl(exe.c_str(), exe.c_str(), "--nogui", patch.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    pid_ = pid;
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    patch_ = patch;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_ = "starting";
        rom_ = expected_rom;
        recent_.clear();
    }

    // Where to connect, on standard input rather than as an argument: a patch
    // made locally carries no address, and --connect is ignored whenever a
    // patch is given.
    if (!server.empty()) {
        const std::string cmd = "/connect " + server + "\n";
        if (::write(stdin_fd_, cmd.data(), cmd.size()) < 0) { /* it will say so */ }
    }

    quit_.store(false);
    thread_ = std::thread(&ApSession::pump, this);
    return true;
}

void ApSession::stop() {
    quit_.store(true);
    if (pid_ > 0) {
        // The group, not the process: see the note in start().
        ::kill(-pid_, SIGTERM);
        for (int i = 0; i < 40 && ::waitpid(pid_, nullptr, WNOHANG) == 0; ++i)
            ::usleep(50 * 1000);
        ::kill(-pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
    if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(mutex_);
    status_ = "stopped";
}

bool ApSession::running() const {
    return pid_ > 0 && ::kill(pid_, 0) == 0;
}

std::string ApSession::produced_rom() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::error_code ec;
    if (rom_.empty() || !fs::exists(rom_, ec)) return {};
    return rom_;
}

std::string ApSession::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

std::vector<ApSession::Line> ApSession::recent() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return {recent_.begin(), recent_.end()};
}

void ApSession::pump() {
    std::string buffer;
    while (!quit_.load(std::memory_order_relaxed)) {
        char chunk[2048];
        const ssize_t n = ::read(stdout_fd_, chunk, sizeof chunk);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            if (boring(line)) continue;

            std::lock_guard<std::mutex> lk(mutex_);
            recent_.push_back(Line{g_line_seq.fetch_add(1), line});
            while (recent_.size() > kRecentLines) recent_.pop_front();
            if (const char* s = classify(line)) status_ = s;
        }
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!quit_.load(std::memory_order_relaxed)) status_ = "client exited";
}

}  // namespace gql
