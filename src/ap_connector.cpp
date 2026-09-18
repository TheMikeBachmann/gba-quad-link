/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_connector.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gba_instance.h"

namespace gql {
namespace {

// The connector script Archipelago expects to be talking to reports 1.
constexpr const char* kScriptVersion = "1";

// How long the socket thread will wait for the machine to run a batch.
//
// The client gives up after five seconds, so this has to be shorter, and it
// has to exist at all: a guest waiting on Dolphin for cycles that are not
// coming would otherwise hold the socket thread for as long as that lasts.
// Answering with an error is recoverable; the client simply asks again.
constexpr auto kServeTimeout = std::chrono::milliseconds(1500);

// A client that asks for emulation to stop and then dies would freeze a guest
// for good. Nothing legitimate holds a lock for a second.
constexpr long long kLockLimitMs = 1000;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

json::Value error(const std::string& what) {
    json::Object o;
    o["type"] = json::Value::of(std::string("ERROR"));
    o["err"] = json::Value::of(what);
    return json::Value::of(std::move(o));
}

json::Value typed(const char* type) {
    json::Object o;
    o["type"] = json::Value::of(std::string(type));
    return json::Value::of(std::move(o));
}

}  // namespace

ApConnector::~ApConnector() { close(); }

bool ApConnector::open(int port, int player) {
    close();
    port_ = port;
    player_ = player;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only. This hands out read and write access to a running game's
    // memory; it has no business being reachable from anywhere else.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return false;
    }
    fd_.store(fd);

    quit_.store(false);
    thread_ = std::thread(&ApConnector::run, this);
    return true;
}

void ApConnector::close() {
    quit_.store(true);
    // Shut the sockets down rather than only closing them, so a thread parked
    // in accept() or recv() comes back instead of being joined forever.
    const int listen_fd = fd_.load();
    if (listen_fd >= 0) ::shutdown(listen_fd, SHUT_RDWR);
    const int c = client_.exchange(-1);
    if (c >= 0) ::shutdown(c, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_ = nullptr;
        done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    const int left = fd_.exchange(-1);
    if (left >= 0) ::close(left);
    if (c >= 0) ::close(c);
}

bool ApConnector::locked() const {
    if (!lock_requested_.load(std::memory_order_relaxed)) return false;
    return now_ms() < lock_deadline_ms_.load(std::memory_order_relaxed);
}

std::string ApConnector::message() const {
    std::lock_guard<std::mutex> lk(message_mutex_);
    return message_;
}

void ApConnector::run() {
    while (!quit_.load(std::memory_order_relaxed)) {
        const int listen_fd = fd_.load(std::memory_order_relaxed);
        if (listen_fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        pollfd p{listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;

        const int c = ::accept(listen_fd, nullptr, nullptr);
        if (c < 0) continue;
        const int yes = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        client_.store(c);

        // Stop listening while somebody is attached.
        //
        // A game client finds its emulator by walking ports 43055 upwards and
        // taking the first that answers, so with four of them looking and the
        // listener left open, the second client's connection is accepted by
        // the kernel into the backlog, never picked up by us, and times out —
        // instead of being refused and moving on to the next port. Refusing is
        // what makes four clients sort themselves across four machines.
        ::close(listen_fd);
        fd_.store(-1);

        std::printf("p%d archipelago: client connected on port %d\n",
                    player_ + 1, port_);
        std::fflush(stdout);

        std::string buffer;
        while (!quit_.load(std::memory_order_relaxed)) {
            char chunk[4096];
            const ssize_t n = ::recv(c, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(n));

            std::size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                std::string out = handle(line);
                out.push_back('\n');
                if (::send(c, out.data(), out.size(), MSG_NOSIGNAL) < 0) {
                    buffer.clear();
                    break;
                }
            }
        }

        client_.store(-1);
        ::close(c);
        lock_requested_.store(false);
        std::printf("p%d archipelago: client disconnected\n", player_ + 1);
        std::fflush(stdout);

        // The port is deliberately not reclaimed here.
        //
        // Whether this machine should be listening again is a question about
        // all four machines at once, because only one may listen at a time,
        // and this thread can only see one. So it ends, leaving the connector
        // quiet until whoever can see all four opens it again.
        return;
    }
}

std::string ApConnector::handle(const std::string& line) {
    // The one request that is not JSON. Everything else is a list in and a
    // list out.
    if (line.rfind("VERSION", 0) == 0) return kScriptVersion;

    json::Value req;
    if (!json::parse(line, &req) || req.type != json::Value::Type::Array)
        return json::dump(json::Value::of(json::Array{error("Bad request")}));

    // Park the whole batch for the machine and wait. Whole, because a guarded
    // read means nothing if the guard and the read happen at two different
    // moments in emulated time.
    std::unique_lock<std::mutex> lk(mutex_);
    pending_ = &req;
    done_ = false;
    cv_.notify_all();
    const bool served = cv_.wait_for(lk, kServeTimeout, [this] { return done_; });
    pending_ = nullptr;
    if (!served) {
        // The guest is not running: stalled on a link, or being handed a
        // different cartridge. The client copes with this by asking again.
        json::Array errs;
        for (std::size_t i = 0; i < req.array.size(); ++i)
            errs.push_back(error("Emulator did not respond"));
        return json::dump(json::Value::of(std::move(errs)));
    }
    return json::dump(reply_);
}

void ApConnector::serve(GbaInstance& gba) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!pending_) return;
    const json::Value& req = *pending_;

    json::Array out;
    out.reserve(req.array.size());

    // Guards are evaluated as they appear, and once one fails every later read
    // or write in the batch is skipped — that is the whole point of a guard,
    // and it is why the batch has to run at a single instant.
    bool guard_failed = false;

    for (const json::Value& r : req.array) {
        const std::string type = r.str("type");

        if (type == "PING") { out.push_back(typed("PONG")); continue; }
        if (type == "SYSTEM") {
            json::Object o;
            o["type"] = json::Value::of(std::string("SYSTEM_RESPONSE"));
            o["value"] = json::Value::of(std::string("GBA"));
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }
        if (type == "PREFERRED_CORES") {
            // Only systems with more than one core have an entry, and the GBA
            // here has exactly one.
            json::Object o;
            o["type"] = json::Value::of(std::string("PREFERRED_CORES_RESPONSE"));
            o["value"] = json::Value::of(json::Object{});
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }
        if (type == "HASH") {
            // Informational. Every Archipelago world identifies a cartridge by
            // reading its memory, not by asking for this.
            json::Object o;
            o["type"] = json::Value::of(std::string("HASH_RESPONSE"));
            o["value"] = json::Value::of(gba.rom_title());
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }
        if (type == "MEMORY_SIZE") {
            json::Object o;
            o["type"] = json::Value::of(std::string("MEMORY_SIZE_RESPONSE"));
            o["value"] = json::Value::of(
                static_cast<std::int64_t>(gba.memory_size(r.str("domain"))));
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }
        if (type == "LOCK" || type == "UNLOCK") {
            const bool on = (type == "LOCK");
            lock_requested_.store(on);
            lock_deadline_ms_.store(on ? now_ms() + kLockLimitMs : 0);
            out.push_back(typed(on ? "LOCKED" : "UNLOCKED"));
            continue;
        }
        if (type == "DISPLAY_MESSAGE") {
            {
                std::lock_guard<std::mutex> mk(message_mutex_);
                message_ = r.str("message");
            }
            out.push_back(typed("DISPLAY_MESSAGE_RESPONSE"));
            continue;
        }
        if (type == "SET_MESSAGE_INTERVAL") {
            out.push_back(typed("SET_MESSAGE_INTERVAL_RESPONSE"));
            continue;
        }

        if (type == "GUARD") {
            std::vector<std::uint8_t> want;
            bool matched = false;
            if (base64::decode(r.str("expected_data"), &want) && !want.empty()) {
                std::vector<std::uint8_t> got(want.size());
                matched = gba.read_memory(r.str("domain"),
                                          static_cast<std::uint32_t>(r.integer("address")),
                                          got.data(), got.size()) &&
                          got == want;
            }
            if (!matched) guard_failed = true;
            json::Object o;
            o["type"] = json::Value::of(std::string("GUARD_RESPONSE"));
            o["value"] = json::Value::of(matched);
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }

        if (type == "READ") {
            if (guard_failed) { out.push_back(typed("READ_RESPONSE")); continue; }
            const auto size = static_cast<std::size_t>(r.integer("size"));
            std::vector<std::uint8_t> got(size);
            if (size == 0 ||
                !gba.read_memory(r.str("domain"),
                                 static_cast<std::uint32_t>(r.integer("address")),
                                 got.data(), size)) {
                out.push_back(error("Failed to read " + r.str("domain")));
                continue;
            }
            json::Object o;
            o["type"] = json::Value::of(std::string("READ_RESPONSE"));
            o["value"] = json::Value::of(base64::encode(got.data(), got.size()));
            out.push_back(json::Value::of(std::move(o)));
            continue;
        }

        if (type == "WRITE") {
            if (guard_failed) { out.push_back(typed("WRITE_RESPONSE")); continue; }
            std::vector<std::uint8_t> data;
            if (!base64::decode(r.str("value"), &data)) {
                out.push_back(error("Bad value"));
                continue;
            }
            if (!data.empty() &&
                !gba.write_memory(r.str("domain"),
                                  static_cast<std::uint32_t>(r.integer("address")),
                                  data.data(), data.size())) {
                out.push_back(error("Failed to write " + r.str("domain")));
                continue;
            }
            out.push_back(typed("WRITE_RESPONSE"));
            continue;
        }

        out.push_back(error("Unknown command: " + type));
    }

    reply_ = json::Value::of(std::move(out));
    done_ = true;
    lk.unlock();
    cv_.notify_all();
}

}  // namespace gql
