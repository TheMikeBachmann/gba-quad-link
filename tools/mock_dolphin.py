#!/usr/bin/env python3
# Copyright (c) 2026 Mike Bachmann
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""A stand-in for Dolphin's SI GBA ports, for testing the link without Dolphin.

Speaks enough of the protocol to drive a GBA core: listens on both ports,
pairs each client's two connections in arrival order, hands out cycle slices
on the clock socket and issues JOY commands on the data socket.

It is not an emulator and knows nothing about GameCube games. What it is for
is the half of the link that is ours — that a core dials out correctly, runs
exactly the cycles it is granted, keeps its place in the slot order, and stops
dead when the slices stop. Behaviour that depends on what a real game asks for
can only be tested against Dolphin.

  ./tools/mock_dolphin.py --players 1 --verbose
"""

import argparse
import socket
import struct
import sys
import threading
import time

CLOCK_PORT = 49420
DATA_PORT = 54970

# The GBA's clock, and how much of it one video frame is worth. Dolphin is the
# timing master: a core runs the cycles it is granted and then waits.
GBA_HZ = 16777216
FRAME_HZ = 59.7275
CYCLES_PER_FRAME = int(GBA_HZ / FRAME_HZ)   # 280896

JOY_RESET, JOY_POLL, JOY_TRANS, JOY_RECV = 0xFF, 0x00, 0x14, 0x15

# What GBASIOJOYSendCommand() sends back, by command. A core that is not in
# JOY bus mode answers nothing at all, which is normal and not an error: a
# commercial cartridge only enters that mode when it wants the link.
REPLY_BYTES = {JOY_RESET: 3, JOY_POLL: 3, JOY_TRANS: 5, JOY_RECV: 1}


class Player:
    def __init__(self, slot, data, clock, verbose):
        self.slot = slot
        self.data = data
        self.clock = clock
        self.verbose = verbose
        self.granted = 0
        self.sent = 0
        self.replies = 0
        self.pending = b""       # a reply that arrived in pieces
        self.alive = True

    def log(self, msg):
        if self.verbose:
            print(f"  [slot {self.slot}] {msg}", flush=True)

    def run(self, rate, stop):
        """Grant a frame of cycles, then poll, at roughly console speed."""
        # Never block on a reply. A core that is not in JOY bus mode answers
        # nothing, and waiting for it would throttle the grants — which makes
        # the instrument, not the core, decide how fast the GBA runs, and
        # quietly turns every timing measurement into a measurement of this
        # loop. Replies are drained when they turn up.
        self.data.setblocking(False)
        period = 1.0 / rate
        next_tick = time.monotonic()
        while not stop.is_set():
            next_tick += period
            try:
                # The clock socket is the whole point: four big-endian bytes
                # saying how many cycles this core may now run.
                self.clock.sendall(struct.pack(">i", CYCLES_PER_FRAME))
                self.granted += CYCLES_PER_FRAME

                # A command has to follow each slice or the core spins in
                # WAIT_FOR_COMMAND without advancing.
                self.data.sendall(bytes([JOY_POLL]))
                self.sent += 1

                while True:
                    try:
                        reply = self.data.recv(4096)
                    except (BlockingIOError, InterruptedError):
                        break
                    if not reply:
                        raise ConnectionResetError
                    self.pending += reply
                    while len(self.pending) >= REPLY_BYTES[JOY_POLL]:
                        n = REPLY_BYTES[JOY_POLL]
                        got, self.pending = self.pending[:n], self.pending[n:]
                        self.replies += 1
                        if self.replies <= 3:
                            self.log(f"poll -> {got.hex()}")
            except (BrokenPipeError, ConnectionResetError, OSError):
                break
            sleep = next_tick - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
        self.alive = False
        self.log("gone")


def accept_pair(data_srv, clock_srv, slot, verbose):
    """mGBA dials data first, then clock, so accept them in that order."""
    data, data_addr = data_srv.accept()
    data.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    clock, _ = clock_srv.accept()
    clock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"slot {slot}: connected from {data_addr[0]}", flush=True)
    return Player(slot, data, clock, verbose)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--players", type=int, default=1,
                    help="how many GBAs to wait for (1-4)")
    ap.add_argument("--rate", type=float, default=FRAME_HZ,
                    help="cycle slices per second; below 59.7275 runs the "
                         "core slow, which is the point of the knob")
    ap.add_argument("--stall-after", type=float, default=0.0,
                    help="seconds, then stop granting cycles without closing "
                         "the socket — the case the compositor must survive")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--data-port", type=int, default=DATA_PORT)
    ap.add_argument("--clock-port", type=int, default=CLOCK_PORT)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    srvs = []
    for port in (args.data_port, args.clock_port):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((args.bind, port))
        except OSError as e:
            # Almost always a real Dolphin already holding the port. Failing
            # loudly matters: a mock that dies here leaves the client to
            # connect to Dolphin instead, and the test silently measures
            # something other than what it claims to.
            print(f"FATAL: cannot bind {args.bind}:{port} - {e}\n"
                  f"Something else is listening, most likely Dolphin itself. "
                  f"Stop it, or pass --data-port/--clock-port to move the mock "
                  f"and match them on the client.", file=sys.stderr, flush=True)
            return 1
        s.listen(4)
        srvs.append(s)
    data_srv, clock_srv = srvs
    print(f"mock dolphin: listening on {args.bind} "
          f"(data {args.data_port}, clock {args.clock_port}), "
          f"waiting for {args.players} GBA(s)", flush=True)

    stop = threading.Event()
    players, threads = [], []
    try:
        for slot in range(args.players):
            p = accept_pair(data_srv, clock_srv, slot, args.verbose)
            players.append(p)
            t = threading.Thread(target=p.run, args=(args.rate, stop),
                                 daemon=True)
            t.start()
            threads.append(t)

        started = time.monotonic()
        while any(p.alive for p in players):
            time.sleep(1.0)
            elapsed = time.monotonic() - started
            if args.stall_after and elapsed >= args.stall_after:
                print(f"--- stalling after {elapsed:.0f}s: sockets stay open, "
                      f"cycles stop ---", flush=True)
                stop.set()
                while True:
                    time.sleep(1.0)
            for p in players:
                print(f"slot {p.slot}: granted {p.granted / GBA_HZ:6.2f}s of "
                      f"GBA time, {p.replies}/{p.sent} polls answered",
                      flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for s in srvs:
            s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
