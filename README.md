# MicroHTTP — A Concurrent Linux Web Server

**Team:** Tech_nova · **Language:** C11 · **Platform:** Linux (POSIX) · **Course:** Operating Systems

MicroHTTP is a small, concurrent HTTP/1.1 static file server built with POSIX
sockets and pthreads. It serves files from a document root (`www/`), handles
GET and HEAD, rejects malformed and malicious requests, and shuts down
gracefully on SIGINT/SIGTERM. A thread pool with a bounded queue keeps the
single accept loop responsive under load.

> Scope note: this is a **university Operating-Systems project**, not a
> production web server. It intentionally implements a small, well-tested
> subset of HTTP. See [Known Limitations](#15-known-limitations).

---

## 1. Project Overview

The server is a classic producer–consumer design:

- the **main thread** accepts TCP connections and submits the client socket
  to a **bounded queue**;
- a fixed pool of **worker threads** dequeues sockets, parses one HTTP
  request per connection, and streams the response;
- files are resolved **relative to an open document-root directory
  descriptor** using `openat()` + `O_NOFOLLOW`, so path-traversal and
  symlink escapes are blocked by construction.

## 2. Features and Scope

| Area | Supported |
|---|---|
| Methods | `GET`, `HEAD`; everything else → `405` + `Allow: GET, HEAD` |
| Versions | HTTP/1.0 and HTTP/1.1 request lines |
| Connections | One request per connection; always `Connection: close` |
| Files | Regular files under `www/`; `/` → `index.html` |
| MIME | `.html .htm .css .js .json .txt .png .jpg .jpeg .gif .svg .ico` |
| Security | percent-decoding, `..` rejection, `openat`/`O_NOFOLLOW` walk, regular-files-only, 8 KiB request cap, 5 s client timeouts |
| Concurrency | fixed pthread pool, bounded FIFO queue, close-on-full policy |
| Shutdown | SIGINT/SIGTERM → drain queue → join workers → exit 0 |

## 3. Architecture

```
                    ┌────────────────────────────────────────────┐
                    │                main thread                  │
                    │  socket → bind → listen → accept loop      │
                    │  SIGINT/SIGTERM → g_shutdown flag          │
                    └───────────────┬────────────────────────────┘
                                    │ thread_pool_submit(fd)
                                    ▼
                    ┌────────────────────────────────────────────┐
                    │        bounded FIFO queue (ring buffer)    │
                    │   capacity = -q   full → close socket      │
                    └───┬──────────┬──────────┬──────────┬───────┘
                        │          │          │          │
                        ▼          ▼          ▼          ▼
                    worker 1   worker 2   worker 3   worker N   (-t threads)
                        │          │          │          │
                        └──────────┴────┬─────┴──────────┘
                                        ▼
                          server_handle_client(fd)
                    read → parser_parse → route → respond → close
                                        │
                                        ▼
                          files_open_url_path(path)
                    percent-decode → openat() walk from
                    web-root dirfd (O_NOFOLLOW, regular files only)
```

## 4. Directory Structure

```
microhttp/
├── src/
│   ├── main.c          CLI, signals, socket setup, accept loop
│   ├── server.c/.h     per-connection HTTP handling (read/respond/log)
│   ├── parser.c/.h     HTTP/1.x request-line + header parser
│   ├── files.c/.h      document-root fd, secure path resolution, MIME
│   └── threadpool.c/.h bounded-queue pthread pool
├── client.c            interactive multi-command test client
├── www/                document root (index/about/contact/services/team/help)
├── tests/
│   ├── test_http.sh    server test suite (51 checks)
│   └── test_client.sh  client test suite (23 checks)
├── bench/bench.sh      reproducible curl-based benchmark
├── Makefile            all / run / test / debug / sanitize / clean
└── README.md
```

## 5. Prerequisites

- Linux (primary target) with glibc or musl; also builds on macOS for development
- `gcc` (or `clang`) with C11 support
- GNU `make`, `bash`, `curl` (tests), `awk`/`sort`/`ps` (bench)

## 6. Build Instructions

```sh
make              # release build: -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
make debug        # -O0 -g3 (gdb/lldb friendly)
make sanitize     # ASan + UBSan build
make clean
```

Both binaries are produced: `build/microhttp` (server) and `build/client`.

## 7. Server Startup

```sh
./build/microhttp                          # port 8080, 4 workers, queue 32
./build/microhttp -p 8080 -t 4 -q 64       # explicit configuration
```

| Flag | Meaning | Default | Valid range |
|---|---|---|---|
| `-p` | listen port | 8080 | 1–65535 |
| `-t` | worker threads | 4 | 1–128 |
| `-q` | queue capacity | 32 | 1–4096 |

Invalid values (including `abc`, `80abc`, empty strings, extra arguments)
are rejected with a message and non-zero exit status. The server must be
started from the directory containing `www/`.

Shut down with `Ctrl-C` (SIGINT) or `kill -TERM <pid>`. The server stops
accepting, workers finish already-queued jobs, then the process exits 0.

## 8. Interactive Client

```sh
./build/client          # connects to 127.0.0.1:8080
./build/client 18080    # custom port
```

Type one or more space-separated commands per line; each response is printed
between separator lines with its full status line, headers, and body:

```
MicroHTTP> home about contact services team help
```

| Command | Request |
|---|---|
| `home` | `GET /` |
| `about` / `contact` / `services` / `team` / `help` | `GET /<name>.html` |
| `head` | `HEAD /` |
| `missing` | `GET /missing.html` (expects 404) |
| `post` | `POST /` (expects 405) |
| `security` | sends two known traversal paths, prints only the verdict (never the body) |
| `exit` | quit (EOF also quits cleanly) |
| *anything else* | treated as a page name: `projects` → `GET /projects.html` |

The client uses one connection per request (matching the server's
`Connection: close` policy), handles partial writes, sets a 10 s I/O
timeout, ignores SIGPIPE, and never crashes when the server is down.

## 9. Adding Website Pages

Create a file in `www/` — no recompilation, no client changes:

```sh
cat > www/projects.html <<'EOF'
<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>Projects</title></head>
<body><h1>Projects</h1><p>Coming soon.</p><p><a href="/">Home</a></p></body></html>
EOF
```

Request it immediately: in the browser open `/projects.html`, with curl
`curl http://127.0.0.1:8080/projects.html`, or in the interactive client
type `projects`. The server looks the file up on every request; the
client derives `/projects.html` from the word automatically.

## 10. HTTP Behavior

- **GET** `/path` → `200` + body, or `404` (missing/directory/symlink),
  or `400` (malformed path/percent-encoding/oversized request).
- **HEAD** `/path` → identical headers (including `Content-Length`),
  no body.
- **Other methods** → `405 Method Not Allowed` with `Allow: GET, HEAD`.
- **`/`** → serves `www/index.html` (content-type `text/html`).
- Requests larger than 8192 bytes → `400`. No `Expect`, `Range`,
  conditional requests, or keep-alive.
- Every response ends with `Connection: close`; the server closes the
  socket after one request. HTTP/1.0 clients work identically.
- A request that is not completed within 5 seconds is dropped (no reply),
  freeing the worker for the next connection.

## 11. Thread-Pool Design

`src/threadpool.c` implements a classic bounded producer–consumer pool:

- a **ring buffer** of client sockets protected by one mutex;
- workers sleep on a condition variable (`work_available`) while the
  queue is empty and the pool is not shutting down;
- **submit** appends and signals one worker; if the queue is full it
  returns `-1` immediately and the *accept loop* closes the socket
  (**queue-overload policy: reject fast, never block the acceptor**);
- **shutdown** sets a flag and broadcasts; workers drain all queued
  jobs, then exit; the creator joins every thread before destroying
  the mutex/condition and freeing memory;
- partial `pthread_create` failure rolls back the threads that did
  start — no leaks on any error path.

Client sockets have exactly one owner at all times: the accept loop
until submit succeeds, then exactly one worker (which closes it), or
the accept loop again if submit failed.

## 12. Security Measures and Limitations

Implemented:

1. **Percent-decoding with strict validation** — malformed sequences
   (`%zz`, trailing `%`) and `%00` are rejected with `400`.
2. **`openat()` component walk from a web-root directory fd** — the
   decoded path is never concatenated into a filesystem path, so client
   input cannot name a location outside the root.
3. **`..` components rejected** (`400`) after decoding, so both literal
   and encoded traversal (`%2e%2e`) fail.
4. **`O_NOFOLLOW` on every component** — symlinks inside `www/` cannot
   point out of the root (they 404).
5. **Directories and special files 404** — only regular files are served
   (`fstat` + `S_ISREG` after open; the fd itself is used, removing
   time-of-check/time-of-use races).
6. **8 KiB request cap** and **32-header cap** in the parser.
7. **5-second read/write timeouts** per client so stalled connections
   cannot pin workers forever.
8. **Strict parser** — rejects control characters, bad versions, and
   malformed header lines before any filesystem work.

Limitations (by design, documented honestly):

- No TLS, authentication, caching, compression, or logging to file.
- Query strings are not stripped: `/index.html?x=1` → `404`.
- No per-directory indexes: only `/` maps to `index.html`.
- Oversized requests get `400` (not the more precise `414`/`431`).
- `Connection: close` only — no keep-alive, no pipelining.
- TOCTOU against *renames inside the web root during a request* is
  impossible for the served fd, but content can still change between
  requests (inherent to static serving).

## 13. Testing

```sh
make test          # builds, then runs both suites
bash tests/test_http.sh
bash tests/test_client.sh
PORT=18081 bash tests/test_http.sh   # custom port
```

`test_http.sh` (51 checks) covers: basic HTTP (status codes, MIME types,
HEAD semantics, Content-Length, binary integrity), security (traversal,
encoded traversal, malformed percent, NUL byte, symlink escape, directory
request, oversized/malformed requests), concurrency (30-client burst,
90 stalled connections vs 72 slots, recovery), shutdown (SIGTERM/SIGINT,
exit status, restart, shutdown under active load), and reliability
(10 invalid-argument cases, missing document root, bind conflict,
dynamic page add/remove without restart).

`test_client.sh` (23 checks) covers: multi-command lines, every mapped
command, security verdicts, dynamic page names, over-long input, EOF
handling, and graceful behavior when the server is down.

Test fixtures are created in `www/` and removed by a cleanup trap; no
user files are touched. Results on the development machine (macOS,
2026-10-13, release build):

```
tests/test_http.sh    TOTAL  pass=51  fail=0
tests/test_client.sh  CLIENT TOTAL  pass=23  fail=0
```

Sanitizer run (`make sanitize && make test`): both suites pass with **no
AddressSanitizer, UndefinedBehaviorSanitizer, or LeakSanitizer reports**
on the development machine.

**Linux VM verification of Phases 3–5 was performed on the project VM**
(`~/microhttp`, bash + curl): the 35-check server suite passed
(`TOTAL pass=35 fail=0`) before the Phase 6–11 additions in this update.
Re-run `make test` on the VM to verify the current tree.

## 14. Benchmark Methodology and Results

```sh
bash bench/bench.sh                      # 200 req, conc 1/10/40, workers 1/4/8
bash bench/bench.sh 500 "1 10 50" "1 4 8"
```

Method: for each (workers, concurrency) cell the script starts a fresh
server, sends 20 warm-up requests, then issues N requests in batches of
the given concurrency, timing every transfer with curl's
`time_total` (includes connect + request + full response). It reports
throughput (N / wall-clock), median and p95 latency, error count, and a
`ps` snapshot of server RSS. No external benchmark tool is required.

**Measured result** (development machine: macOS arm64, curl-based loop,
idle laptop, 2026-10-13, release build — numbers are only meaningful on
this machine; re-run on the Linux VM for VM numbers):

```
workers  conc   req/s   median_ms  p95_ms  mean_ms  errors  rss_mb
1        1         49        0.7      1.2      0.8       0     1.6
1        10       286        0.6      1.4      0.7       0     1.6
4        1         50        0.7      1.1      0.7       0     1.8
4        10       299        0.7      1.2      0.7       0     1.8
```

Observations (this workload, this machine): concurrency 1→10 raises
throughput ~6× while latency stays sub-millisecond — the accept loop and
queue keep up and workers overlap I/O. Worker count barely matters for a
1-page localhost workload (bottleneck is curl/process overhead, not the
server). These are **measured** numbers, not claims of general
superiority; for stronger evidence run `wrk`/`ab` against the VM.

## 15. Known Limitations

- Single request per connection (no keep-alive) — throughput is dominated
  by TCP setup per request.
- Static files only; no CGI, proxying, or dynamic content.
- No query-string parsing, no directory listing, no index per directory
  beyond `/` → `index.html`.
- `400` used for oversized requests instead of `414`/`431`.
- Client I/O timeout (10 s) and server client timeout (5 s) are
  compile-time constants.
- IPv4 only (`AF_INET`, `INADDR_ANY`).
- Logging to stdout only; log lines are status/method/path (client input
  is sanitized by the parser before logging, so log injection is not
  possible, but there is no rotation or levels).

## 16. Future Improvements

- Keep-alive (`Connection: keep-alive`) with a per-connection request
  loop and idle timeout.
- Query-string stripping and `414`/`431` status codes.
- Configuration file and `--docroot` flag.
- Structured logging (timestamps, client addresses, levels).
- IPv6 (`AF_INET6`) support.
- Optional `wrk`-based benchmark comparison of pool sizes under sustained
  load, and a sequential (thread-per-request) baseline for the report.

---

## Design Decisions (OS viva talking points)

1. **Thread pool, not thread-per-connection.** Spawning a thread per
   connection is simple but unbounded; a fixed pool with a bounded queue
   gives a hard resource ceiling — an OS-level admission-control idea.
2. **Bounded queue + close-on-full.** Blocking the acceptor would stop
   accepting even though the kernel's listen backlog could still absorb
   connections; dropping the socket immediately keeps the system stable
   (fail-fast backpressure).
3. **One mutex, one condvar, drain-then-join shutdown.** The pattern
   mirrors producer–consumer slides from class: `pthread_cond_wait`
   releases the mutex while sleeping; shutdown uses `broadcast` (not
   `signal`) so every sleeper wakes; joining before destroying
   primitives prevents use-after-destroy races.
4. **`sig_atomic_t` flag + no `SA_RESTART`.** Signal handlers may only
   assign to `volatile sig_atomic_t`; the accept loop relies on `EINTR`
   to notice the flag promptly instead of waiting for the next client.
5. **Descriptor-relative file access (`openat` walk).** Absolute-path
   checks are racy; walking from a directory fd with `O_NOFOLLOW`
   guarantees containment at the filesystem level — the same technique
   used by sandboxed daemons.
6. **Single-request connections.** Simplicity and clear resource
   lifetime (fd closed by exactly one owner) were preferred over
   keep-alive performance for the project's scope.

## Build & Run Cheatsheet

```sh
make                                # build server + client
make test                           # full automated suite
make sanitize && make test          # suite under ASan+UBSan
./build/microhttp -p 8080 -t 4 -q 64
./build/client                      # interactive testing
bash bench/bench.sh 200 "1 10 40" "1 4 8"
```
# Microhhtp
