# Cinder

A Redis-compatible in-memory key-value server written from scratch in C++17.
It speaks the Redis protocol (RESP), so the official `redis-cli` and
`redis-benchmark` work with it unmodified.

```
$ redis-cli -p 6380
127.0.0.1:6380> SET greeting hello EX 60
OK
127.0.0.1:6380> GET greeting
"hello"
127.0.0.1:6380> TTL greeting
(integer) 58
```

## Features (V1)

- Single-threaded, non-blocking **`poll()` event loop** serving many clients at once
- **RESP parser** that handles commands split across packets and many commands in one packet (pipelining)
- Commands: `PING`, `ECHO`, `GET`, `SET [EX|PX] [NX|XX]`, `DEL`, `EXISTS`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `DBSIZE`, `FLUSHALL`
- **Lazy key expiry** on a monotonic clock
- Unit tests, an end-to-end test using `redis-cli`, and AddressSanitizer builds

## Build, test, run (Linux / WSL)

```bash
cmake -S . -B build && cmake --build build -j
./build/unit_tests               # parser, store and command tests
bash tests/integration_test.sh   # real redis-cli against a running server
./build/cinder 6380              # start the server
bash bench/benchmark.sh          # redis-benchmark
```

Debug build with sanitizers: `cmake -S . -B build-asan -DSANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`.

## Benchmark

`redis-benchmark`, 50 clients, on a laptop under WSL2 (results vary by machine):

| Workload | SET | GET |
|---|---|---|
| 1 command per round trip | ~100k ops/sec | ~99k ops/sec |
| 16 pipelined commands per round trip | ~1.15M ops/sec | ~1.39M ops/sec |

Pipelining is roughly 10× faster because the cost per request is dominated by
the network round trip and system calls, not by the hash-map lookup.

## How it works

```
 clients ──TCP──►  poll() event loop  ──►  per-connection read buffer
                        │                         │
                        │                  RESP parser (resp.cpp)
                        │                         │  one complete command
                        │                  command table (commands.cpp)
                        │                         │
                        │                  Store: unordered_map (store.cpp)
                        │                         │  RESP reply
                        └──── send() ◄──── per-connection write buffer
```

| File | Responsibility |
|---|---|
| `src/server.cpp` | Sockets, the event loop, per-connection buffers |
| `src/resp.cpp` | Parsing commands, encoding replies |
| `src/commands.cpp` | One function per command, looked up in a table |
| `src/store.cpp` | The hash map, expiry times, lazy expiry |

**One event-loop iteration:**
1. Build a list of sockets for `poll()`: the listening socket, every client for "readable", and clients with unsent replies for "writable".
2. `poll()` sleeps until at least one socket is ready.
3. New connections are accepted and made non-blocking.
4. Readable clients: read everything available into `rbuf`, parse as many *complete* commands as possible, execute each, append replies to `wbuf`. A half-received command stays in `rbuf`.
5. Writable clients: send as much of `wbuf` as the kernel accepts.

## Design decisions (and the interview questions behind them)

**Why one thread?** The work per command is tiny (a hash lookup), so the
bottleneck is I/O, not CPU. One thread means the store needs no locks and there
are no race conditions. Redis made the same choice. The cost: one slow command
blocks everyone, and only one CPU core is used.

**Why non-blocking sockets?** With blocking I/O, a `read()` on a client that has
sent nothing would freeze the whole server. Non-blocking calls return `EAGAIN`
instead, and `poll()` tells us when to try again.

**Why buffers per connection?** TCP is a byte stream, not a message stream. One
`read()` can return half a command or three commands. The parser returns
`Incomplete` until a whole command is buffered (see the unit test that splits a
command at every byte).

**Why lazy expiry?** Checking expiry only when a key is accessed keeps every
operation O(1). The trade-off: an expired key that is never read again stays
in memory. Redis fixes this with an additional background sampler, which is
planned for V2.

**Why a monotonic clock?** `steady_clock` never jumps backwards, so a system
clock change cannot make keys expire early or live forever.

**What happens when a client disconnects mid-send?** `send()` uses
`MSG_NOSIGNAL`, so we get an `EPIPE` error instead of a `SIGPIPE` that would
kill the server.

**What stops a client from using all the memory?** A connection that buffers
more than 64 MB of unparsed input is closed, and the parser rejects oversized
lengths before allocating.

**Why poll() and not epoll()?** `poll()` is simpler and portable, but it passes
the whole list of sockets to the kernel on every call, which is O(n). `epoll`
keeps the list inside the kernel and returns only ready sockets. Switching is
the first V2 item.

## Roadmap (V2)

- `epoll` instead of `poll`
- Own hash table with incremental rehashing (no latency spike when it grows)
- Active expiry (background sampling of keys with a TTL)
- `maxmemory` with LRU eviction (hash map + doubly linked list)
- Append-only file persistence and crash recovery
- `INCR`/`DECR`, sorted sets on a skip list
- Use Cinder as SeatLock's cache
