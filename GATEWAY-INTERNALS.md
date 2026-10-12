# Gateway internals: one request lifecycle, two backends

How a request actually moves through `llmbridge`, on both event-loop backends.
`DESIGN.md` is the architecture summary and `LATENCY.md` defines every timing
number; this document is the mechanism: which buffer holds what, who owns each
piece of mutable state, when a connection may be freed, and where a credential
is scrubbed.

The lifecycle is written once, in `gateway/src/core/`, as methods of
`detail::Engine`, and reaches a socket only through `IoOps`. Each backend,
`EpollIo` (`gateway/src/io/epoll_io.cpp`) and `UringIo` (`gateway/src/io/uring_io.cpp`),
owns its loop and its syscalls and calls back into the shared methods as completions
arrive. `Gateway` is a facade over the `Engine`. What still differs between the
backends is how I/O is issued and how completion is discovered (sections 4 and 5),
and the threshold at which a slow streaming client pauses its upstream (6c).

## 1. The cast

Two connection kinds, and the vocabulary is used consistently everywhere:

| | |
|---|---|
| **client conn** | a customer's connection to us: `ClientConn` |
| **upstream conn** | our connection to the provider: `UpstreamConn` |

Both derive from `ConnBase` (`gateway/src/core/conn.hpp`), what a socket needs on
either leg: the fd, the buffers, the TLS Session and the backend's flags. The request
and its stream live only on the client; the venue, the response framer and the pool
links only on the upstream, which keeps a pooled upstream under 512 bytes (a
`static_assert` holds it). A backend learns the kind of an event from its tag, the
low bits of epoll's event data or of io_uring's `user_data`, and calls a typed
handler; shared code holding a `ConnBase` reaches the concrete type through `visit()`.

They are linked by `ClientConn::up` and `UpstreamConn::client` for the duration of one
request, and unlinked when it completes. An upstream with no client is either idle in
the pool or dying.

## 2. The buffers

Both are named from the **gateway's** point of view on that socket: `rbuf` is what
we read from the peer, `out` is what we write to it. The same field name
therefore holds the request on one connection and the response on the other, and
which is which depends on the kind. That is the most confusing thing about the
data model, so here is one request crossing all four:

```
   client                        gateway                        provider
     |                                                             |
     |  request bytes                                              |
     |------------------> c->rbuf ---[frame, translate]---> u->out |
     |                                                     |-------|--->
     |                                                             |
     |                                                    response |
     |  c->out  <---[translate back]--- u->rbuf <-------------------|
     |<------------------|                                         |
```

| buffer | on a client conn | on an upstream conn |
|---|---|---|
| `rbuf` | the request the customer sent | the response the provider sent |
| `out` | the response we send the customer | the request we send the provider |
| `tls_out` | ciphertext heading for the socket (TLS conns only) | same |

`out` and `tls_out` are `OutBuf`s (`gateway/src/core/outbuf.hpp`, 2b). Both `rbuf`
and `out` are **plaintext, always**, whether or not TLS is in use.
TLS interposes at the socket edge only, so HTTP framing, the SSE pump and the
stale-connection resend never know it exists.

### Sent is not always "on the wire"

What `out` counts as sent shifts with the transport, and this is a real trap:

- **plaintext**: bytes actually written to the socket
- **TLS**: bytes fed into the `Session`, which have not necessarily left the
  machine. Wire progress is `tls_out`'s

So on a TLS connection `out.idle()` means "fully encrypted", never "fully sent".
Ask `tls_out_flushed()` for the second question.

### 2b. `OutBuf`: stage, wire, pin

| | |
|---|---|
| `stage()` | where new bytes go. Never behind bytes already on the wire: a drained front is reset first, keeping its capacity. Every builder appends here |
| `wire()` | what the transport writes next, out of the front |
| `pin()` / `sent(n)` | an io_uring send holds the front from submit (`UringIo::submit_send`) to completion (`UringIo::on_send`); epoll never pins, so there it is one buffer |
| `take()` | everything staged, for a resend: moved, or copied while a send still reads it |

While the front is pinned, `stage()` is the back half, and `sent()` folds it in when
the send completes, so streaming output never moves memory the kernel is reading and
neither half loses its capacity.

The front keeps what was sent until it is staged over, cleared or taken. That is
what keeps an upstream request available for a resend when a pooled connection
turns out to be dead: `retry_upstream` `take()`s it, and
resend only while the request is `Dispatched` (3c).

## 3. Request lifecycle, common to both backends

```
 accept
   |
   v
 read client bytes ---> c->rbuf
   |
   v
 frame HTTP/1.1 (net/http.hpp)      <- refuses malformed input, never repairs it
   |
   v
 translate OpenAI -> provider dialect (provider/src/translate_*.cpp)
   |
   v
 acquire upstream: pooled keep-alive, or a fresh connect
   |
   v
 write u->out   (credential mapped: Bearer -> x-api-key / x-goog-api-key)
   |
   v
 read provider bytes ---> u->rbuf
   |
   +--- text/event-stream 200 ---> STREAMING PUMP (section 6)
   |
   v
 frame response, translate back, write c->out
   |
   v
 finish: release the upstream to the pool, or close it
```

Everything above is shared. What differs is **how the I/O is issued and how
completion is discovered**, which is section 4 and section 5.

### 3b. One reset per request

A request's state is `Connection::req` (`gateway/src/core/req.hpp`): `ReqState`, the
trivially copyable part (sequence number, t0-t4 and the streaming stamps, usage,
the policy's overrides and every bounded copy for the sink), and the decoders and
buffers kept across requests. `RequestCtx::begin()` is the only reset. It runs at
framing, as soon as the parser answers, and before any reply, a 400 included, so
nothing a request reports can belong to the one before it on the connection. It
constructs a fresh `ReqState` in place rather than assigning one, which writes only
the scalars and leaves the string buffers' bytes alone. Strings the request keeps
are `FixedStr` copies, never views: the policy's `model` and `service_tier` are
copied at the decision (a longer one is refused with a 500, never cut), and a
failover takes `Retry`'s.

### 3c. Phases

`ReqState::phase` says where a request stands, and the gates read it, not the buffers:

| phase | entered | what it allows |
|---|---|---|
| `Idle` | at framing (`begin()`), and when a reply finishes | a new request may be framed |
| `Dispatched` | `*_forward`, before the upstream is acquired | retry on a fresh connection, failover to another venue |
| `Responding` | the first response byte in the upstream's `rbuf` | neither: the venue may have acted on the request |
| `Streaming` | the SSE head is staged | the stream finishes when it ends |
| `Replying` | a whole reply is staged | the reply finishes once it is on the wire |

A write event with no reply staged, such as a drained interim `100 Continue`, finishes
nothing. An error once a reply has begun, or with bytes still owed to the client,
closes the client instead of staging a second reply into the first.

### 3d. The seam: `IoOps`

The shared lifecycle reaches a socket only through `detail::IoOps`
(`gateway/src/core/io.hpp`), which each backend implements (`EpollIo`, `UringIo`).
A backend's `run()` attaches itself before the first event, so a shared method such as `error_respond`,
`acquire_upstream`, `retry_upstream`, `release_upstream`, `upstream_failed`,
`abort_pair`, `begin_stream`, `stream_on_upstream_eof` or `finalize_stream` is
written once and runs on both.

| operation | epoll | io_uring |
|---|---|---|
| `send` | write now; `Queued` arms `EPOLLOUT` | one send SQE; always `Queued` unless nothing is owed |
| `connect` | non-blocking `connect()`, `EPOLLIN` and `EPOLLOUT` | socket, resolve, a connect SQE |
| `set_read` | `EPOLLIN` off or on | cancel the multishot recv; re-arm it on resume |
| `close` | `Engine::forget`, then `::close` | `Engine::forget`, then cancel; freed at `inflight == 0` |

A send that completes at once is a return value (`Sent::Done`), never a callback, so
no handler runs re-entered. `IoCaps` says how the backend bounds a stream for a slow
client: both pause its upstream, each at its own threshold, and io_uring keeps a cap
as the ceiling (6c). Nothing else of the lifecycle is written per backend: each loop reads or
learns of a completion and calls a shared handler.

The request side is shared outright. Bytes a backend reads go to `on_client_bytes`:
`try_frame` stamps t0, frames one request from `rbuf` and hands it to `dispatch`, and
a finished request re-frames whatever was pipelined from the same buffer, never
with another read. An `Expect: 100-continue` is answered by staging the interim line
in `out` and sending it like any reply; it finishes nothing, because no request is
framed yet. Reading continues while a request is in flight, so a client may pipeline
behind it, up to `kMaxClientBuffered` (no legal request is larger); past that its
reads are paused through `set_read` (counted in `client_read_pauses`) until the
requests drain the buffer. On io_uring a pause cancels the multishot recv, whose
`-ECANCELED` completion re-arms it if the client was resumed meanwhile; bytes the
kernel had already placed still arrive, bounded by the shared buffer ring. A client
that disconnects with a request, reply or stream still owed counts as an error on
either backend (`client_gone`). `gateway/tests/core_test.cpp` drives the shared methods with
a `FakeIo`, so a send that does not complete happens exactly when a test says so.

## 4. The epoll backend (`gateway/src/io/epoll_io.cpp`)

`EpollIo` owns the epoll instance, its loop and its syscalls, and nothing else.
Level-triggered, with optimistic writes.

```
  epoll_wait
      |
      +-- listener readable -------> on_accept()             accept4() in a loop
      +-- client fd readable ------> on_client_readable()    read() -> on_client_bytes()
      +-- client fd writable ------> on_client_writable()    respond(), or stream_flush()
      +-- upstream fd readable ----> on_upstream_readable()  read() -> on_upstream_bytes()
      +-- upstream fd writable ----> on_upstream_writable()  connect completed, or drain out
      |
      v
  sweep_idle()          reap idle pooled upstreams, every timeout
      |
      v
  free everything on _doomed        <- see section 7
```

**Optimistic write.** A reply is written at once and `EPOLLOUT` is armed only on
`EAGAIN`, so the common case costs one syscall; `send()` disarms it again whenever
everything went. A fresh upstream is registered once, `EPOLLIN | EPOLLOUT` together:
its connect completing is its first writable event.

**One writer of the interest mask.** `sync_interest()` computes the mask from
`read_paused` and `write_armed`, so arming a write cannot undo a pause.

**Back-pressure.** `set_read` drops `EPOLLIN`; `EPOLLHUP` and `EPOLLERR` still fire, so a
close is never missed. Reads are bounded per event (`kMaxReadPerEvent`, 1 MiB): a drain
to `EAGAIN` once staged 33 MB for a client that never read.

## 5. The io_uring backend (`gateway/src/io/uring_io.cpp`)

`UringIo` owns the ring, the provided-buffer ring and its loop. Completion-driven,
`SINGLE_ISSUER | DEFER_TASKRUN`, with multishot accept and recv into the shared buffers.

```
  submit SQEs                          reap CQEs
      |                                    |
      +-- IORING_OP_ACCEPT (multishot) ----+--> on_accept()
      +-- IORING_OP_RECV   (multishot) ----+--> on_recv()     -> on_client_bytes() / on_upstream_bytes()
      +-- IORING_OP_SEND               ----+--> on_send()     -> finish() / stream_flush()
      +-- IORING_OP_CONNECT            ----+--> on_connect()
      +-- IORING_OP_TIMEOUT            ----+--> periodic tick: sweep_idle()
      +-- IORING_OP_ASYNC_CANCEL       ----+--> (control, not counted)
```

4096 buffers of 4096 bytes. Multishot recv lands data in the ring instead of a
per-connection buffer, so the kernel picks the buffer and it is recycled once copied
out. If the setup fails, `Engine::run` runs `EpollIo` instead.

Three constraints shape everything else here:

1. **An SQE is immutable once submitted.** The kernel reads the buffer the SQE
   points at, on its own schedule. Anything that could reallocate that buffer
   while an operation is in flight is a use-after-free.
2. **Two concurrent SENDs on one fd would interleave**, so sends are serialised.
3. **Completions arrive after the code has moved on**, so an object can be
   logically dead while the kernel still holds a reference to it.

A full submission queue is flushed once and retried. If it is still full, a send
or recv closes the pair (`drop_sq_full`), a connect fails the acquire, and a lost
timer is re-armed on the next loop.

### The pin: one owner

`send_inflight()` means exactly "an SQE referencing this connection's wire buffer
is outstanding": the `OutBuf` the socket reads (`tls_out` on TLS, else `out`) is
pinned.

| | |
|---|---|
| **pinned** | `UringIo::submit_send()` only, the sole place a send SQE is submitted |
| **unpinned** | `UringIo::on_send()` on completion, and `on_cqe()` for a doomed connection's |
| **read** | by anyone deciding whether to submit now or let the completion do it |

Callers must never pin. Two of them used to set the old flag, under different
rules, and the stream flush setting it before calling into the TLS flush, whose
first line refuses to run when it is already set, meant the first SSE flush on a
TLS connection did nothing and the stream hung forever, on io_uring only.

New stream output goes to `stage()`, the back half while a send reads the front,
and goes out with the next send.

### 5b. `inflight` and `recv_armed`

**`int inflight`**: submitted-but-uncompleted SQEs referencing this connection.
One protocol with `doomed` (section 7): doomed says we want the object gone,
inflight says whether the kernel agrees yet.

| | where |
|---|---|
| incremented | `arm_recv`, `submit_send` and `connect`, each only once its SQE is filled, so an early return cannot strand the object |
| decremented | exactly one place, `on_cqe()`, and only when the completion is not armed: a multishot op carrying `F_MORE` is still outstanding |
| read | `maybe_free()`, the only thing allowed to conclude that freeing is safe |

**`recv_armed`**: a multishot recv is live. A pause (`set_read(c, false)`) cancels it
by its `user_data`; its last completion, `-ECANCELED`, re-arms it if reads resumed
meanwhile, and a resume with no recv live arms one at once.

## 6. Streaming (SSE)

Entered when the provider answers `200` with `content-type: text/event-stream`.
The gateway then pumps instead of buffering a whole body: decode chunked,
translate Anthropic events to OpenAI chunks, write to the client.

```
  provider ---chunked SSE---> u->rbuf
                                |
                                v
                        stream_step()            shared by both backends
                       decode chunk -> translate
                                |
                     -> c->out.stage() -> stream_flush() -> IoOps::send
             epoll:  write now; Queued pauses the upstream
             uring:  SEND (pins the front); past kStreamPauseAbove owed, pause;
                     past kStreamBufCap the stream is dropped
                                |
                                v
                          client sees chunks
```

Three flags carry the state, and their meanings are precise:

- **`streaming`** latches in `begin_stream` and is cleared only when a stream
  finalizes on a connection that is kept; the next request's `begin()` resets the
  rest.
- **`stream_ended`** means no further output will be produced. It becomes true
  two ways: cleanly, when the translator emitted its terminal `[DONE]`; or by
  truncation via `stream_truncate()`, which emits **no** `[DONE]` on purpose so a
  client sees a cut-off stream instead of a fabricated clean finish. So
  `stream_ended && close_after_resp` is the truncated case and
  `stream_ended && !close_after_resp` the clean one.
- **`read_paused`** is written only by a backend's `set_read`: a stream's upstream,
  and a client past the pipelining cap, on either backend.

`stream_ended` also gates `finalize_stream`. Without it the gateway resumes
reading from an upstream that will never speak again, and the client socket is
held until the idle sweep.


### 6b. `stream_ended`, and the difference a client can see

It becomes true two ways, and only one of them is a finished answer:

| | what happened | `close_after_resp` |
|---|---|---|
| **clean** | `stream_step()` saw the end of the body and the SSE translator emitted its terminal `[DONE]` | false |
| **truncated** | the stream is being aborted, so no `[DONE]` is emitted, deliberately: a client must see a cut-off stream instead of a fabricated clean finish. Always via `stream_truncate()` | true |

So `stream_ended && close_after_resp` is the truncated case. That pairing is the
difference between a client believing it received the whole answer and knowing
it did not, so read it, never infer it.

`stream_truncate()` makes the three mutations together (`stream_ended`,
`close_after_resp`, `++errors`) because five call sites once wrote them by hand,
which is how a sixth ends up setting two of the three. The `stream_ended` latch is
load-bearing: `stream_flush` finalizes only when it is set, and without it
it resumes reading from an upstream that will never speak again, holding the client
until the idle sweep. `CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient`
is the test that fails without it. The `TRUNCATED` warning is logged there too, in
the one shared place: the abort paths bypass the finalize, and a truncated stream is
the one outcome a client cannot tell from a clean finish, so if it is not recorded
on our side nobody can answer "did my stream complete?" afterwards.

### 6c. Back-pressure under a slow client

Both backends pause a stream's upstream for a slow client and resume it once the
client has drained what the stream owes it (`owed()`: plaintext, and ciphertext on
TLS). They differ in when, and `IoCaps` says which, read in `stream_pump` and
`stream_flush`:

| | pauses | ceiling |
|---|---|---|
| **epoll** | when a write is left short (`stream_pause_above` 0): the socket is full | none; a read is budgeted per event (`kMaxReadPerEvent`) |
| **io_uring** | when more than `kStreamPauseAbove` (1 MiB) is owed: every send is queued while it is in flight, so queued alone means nothing | `kStreamBufCap` (8 MiB): bytes the kernel placed before the cancel still land, and past the cap the stream is dropped |

An io_uring pause cancels the upstream's multishot recv; a resume re-arms it, or the
cancel's completion does if the resume came first. A paused upstream is not polled,
so a venue that resets meanwhile is noticed on resume. Before 0.87.0 io_uring did not
pause streams: the cap was its only bound, and a slow client's stream past 8 MiB was
dropped. The 32 MiB flood to a TLS client that never reads (`GatewayTls.
ClientThatNeverReadsCannotGrowUsWithoutBound`) peaks at 55 MB of process RSS paused,
69 MB when the cap dropped it; epoll's is 53 MB.

Either way the idle deadline (`--upstream-timeout`) measures `ts_progress`, which both
legs advance: a response byte read, a request byte sent, a byte the client took. A
paused upstream reads nothing while its client drains, and a finished stream reads
nothing while its client catches up; neither is idle. When neither leg
moves and bytes are still owed to the client, the sweep logs `client stalled` and counts
`client_idle_timeouts`, not `upstream_timeouts`.

`read_paused` is written only by a backend's `set_read`, which both guard on the
current value. A stream pauses its upstream only where `stream_backpressure` holds,
and so does the release to the pool, which resumes reads first (8).

## 7. Connection lifetime, and why the two backends free differently

This is where a mistake is a use-after-free in a process holding customer
credentials, so the rules are worth stating exactly.

A close is two halves. `Engine::forget()` is the bookkeeping, the same on both
backends: unpair, deregister, drop from the pool, mark `doomed`, move to `_doomed`.
The backend's `close()` does the rest. **Both backends defer the free**, for
different reasons:

| | why deferred | freed when |
|---|---|---|
| **epoll** | one event can close a pair, and a later event in the same batch would dereference a freed pointer | end of the current event batch, unconditionally |
| **io_uring** | a submitted SQE references the object, and the kernel does not care that we decided to close | `inflight` reaches 0, possibly several loop iterations later |

```
  EpollIo::close()                       UringIo::close()
        Engine::forget()                       Engine::forget()
        ::close(fd)                            shutdown + IORING_OP_ASYNC_CANCEL
              |                                      |
              v                                      v
   end of batch: delete every _doomed      on_cqe: --inflight
                                                     |
                                                     v
                                          maybe_free(): free iff
                                          doomed && inflight == 0
```

Every connection is in exactly one `Registry` (`gateway/src/core/registry.hpp`):
`_clients`, `_upconns` (live upstreams, pooled ones too) or `_doomed`. Each member
holds its own index, so a close moves it in O(1), and the destructor and the
io_uring drain walk the registries instead of following `up` to find upstreams.
`Engine::free_conn` deletes a doomed one as its own type, keeping an upstream's
buffers warm for the next.

**The unconditional frees are safe for two different structural reasons.** epoll
never increments `inflight`, so it is always 0. `UringIo::run()` ends with a drain
that reaps until no SQE is outstanding, and its count moves in lockstep with every
connection's. A runtime check at those sites was added and removed: unreachable,
untestable, and therefore protection in appearance only.

## 8. Connection pooling

Upstream connections are keep-alive and reused. A streaming request would
otherwise pay a fresh connect every time, which measured as the dominant term in
time to first token.

```
  acquire_upstream
        |
        +-- UpstreamPool::acquire(venue): the newest, from_pool = true, ++upstream_reused
        |
        +-- else: new socket, connect (async on both backends; IoOps::connect)

  ...request completes...

  release_upstream -> UpstreamPool::release, the one reusability check;
  any refusal closes the connection:
        |
        +-- closed already, or not keep-alive (response, or a byte-forward's client)
        +-- no message boundary: close-delimited, or a stream cut before its end
        +-- the venue sent EOF (peer_eof)
        +-- bytes past the response in rbuf: a second message nobody asked for
        +-- request not fully on the wire: ++upstream_unsent  <- see below
        +-- kMaxIdleUpstreams (8192) connections pooled across all venues
        |
        +-- otherwise: SCRUB (section 9), stamp ts_pooled, link as the venue's newest

  sweep_idle(): reap what idled past --pool-idle (30 s), oldest first
```

The pool is one intrusive list per venue: `acquire` takes the newest, removal on
close is O(1), and the reaper visits only expired connections, which sit at the
oldest end of each list. `up` and `client` are written only by `pair()` and `unpair()`,
and every close unpairs, so neither leg keeps a pointer to a freed connection.

**Why the 30 s reap.** Providers drop idle keep-alives on their own schedule, and
discovering a corpse costs a request its retry. Reaping first is cheaper than
finding out the hard way.

**A response can beat our own request out the door, and such a connection must
not be pooled.** The upstream's recv is armed before the request is sent, so a
provider that answers early, a 413 or 401 on the headers of a large body, hands
us a complete response while the request is still half-written. Pooling that
connection scrubs and later overwrites a buffer the transport has not finished
with, and leaves a truncated request on the wire for the next client to inherit.
`upstream_request_sent()` is the predicate, checked in `UpstreamPool::release`
before the pool takes it, and the connection is closed instead. It fails closed
because the provider's view of that connection is not something we can
reconstruct. Reproduced on both backends by `ProxyEarlyResponse`, where the next
client's request was silently dropped, and mutation-guarded on each backend
separately.

**Stale-connection retry.** A pooled connection that fails before any response
byte arrives is retry-eligible (`from_pool && !retried`): the request is resent
on a fresh connection, and the client never sees the blip. The write path keeps
`out`'s front for exactly this reason, so the request survives to be resent (2b);
the gate is the phase (3c), so a request whose response has begun is never resent.


### 8b. Sizing `kMaxIdleUpstreams`, and why it is 8192

The pool was unbounded until streaming reuse landed. A streaming gateway pools
roughly one upstream per concurrent stream, so without a bound it is an fd leak
in slow motion: 4k streams means 4k idle descriptors pinned indefinitely.

**Size it generously.** The cap must exceed the number of upstreams in flight at
peak, or it stops being a bound and becomes a reuse killer: once the pool is
full, every release closes its connection and the next request must reconnect.

A first cut of 256 did exactly that, and cost the non-streaming path **2.4x its
throughput**:

| pool cap | RPS at a 90k target |
|---|---|
| 256 | 32,210 |
| 8192 | 77,282 |
| no pool at all | 78,445 |

The gateway was opening more upstream connections than it served requests.
git-bisected. **Do not lower this without re-running `./bench/saturate.sh` with
`BACKENDS=4`.** The real reclaim mechanism is the idle timeout, not the cap; the
cap only has to stop pathological growth, so err high.

## 9. Scrub and clear: what a pooled connection must not carry

A pooled connection idles for up to 30 seconds and is then handed to **whichever
client asks next**. Anything left in its buffers becomes the next customer's
problem, so release does two different things for two different reasons:

```
  UpstreamPool::release
        |
        +-- u->rbuf non-empty      RESIDUE. Bytes past the framed response would
        |   refuses                otherwise be read as the head of the NEXT
        |                          client's response: a cross-client desync
        |
        +-- u->framer.reset()      the cached head and chunked-decode state
        |
        +-- u->out.scrub()         CREDENTIAL. out held the rebuilt request
        |                          including the customer's API key
        |
        +-- msg = {}, tls_out.clear()
```

**`clear()` and `secure_clear()` are not the same operation.** `std::string::clear`
sets the size to zero and leaves the bytes in the allocation; the credential would
still be in memory, readable from a core dump or a later reallocation.
`secure_clear` overwrites the bytes through a `volatile` function pointer the
compiler cannot elide. A plain `memset` there emitted **zero instructions** at
`-O2` under dead-store elimination, which is why the barrier exists.

**The TLS `Session` is kept across pool cycles.** Only the per-request ciphertext
in `tls_out` is dropped, so a pooled reuse pays no second handshake.

Both of these are mutation-verified: deleting either one fails a test.

### 9b. How `secure_clear` erases (`net/include/net/secure.hpp`)

There is no standard way to do it: `std::secure_clear` (P1315) was proposed and never
adopted. So the platform primitive is chosen at CMake configure time by feature
detection, never by guessing from `#ifdef __linux__`:

| Primitive | Where | Detected as |
|---|---|---|
| `explicit_bzero` | glibc >= 2.25, the BSDs | `LLMBRIDGE_HAVE_EXPLICIT_BZERO` |
| `memset_s` | C11 Annex K, MSVC | `LLMBRIDGE_HAVE_MEMSET_S` |
| `SecureZeroMemory` | Windows | `_WIN32` |
| `volatile` memset | everywhere else | fallback |

The fallback is always correct, because `volatile` semantics are mandated by the
standard, but it comes last because it works for an indirect reason and the named
functions do not. To see the dead-store problem for yourself:
`g++ -O2 -S ... | awk '/secure_clear/,/ret/' | grep call`.

**Scope.** It is not a hot-path function and need not be: it runs once per request
when a pooled upstream is released (2.4 ns for a ~96 B buffer, about 0.02% of one
core at 84k RPS). Do not sprinkle it over transient buffers overwritten microseconds
later; that buys nothing and costs the hot path.

## 10. TLS on both legs

Two independent TLS sessions in opposite roles, never one pipe. The gateway
terminates the client's session and originates its own to the provider, which is
what lets it translate dialects and swap credentials in the middle.

```
  client ==TLS==> [ gateway ] ==TLS==> provider
           we are the SERVER      we are the CLIENT
           (--listen-tls)         (--upstream https://...)
```

OpenSSL is driven through **memory BIOs**, never `SSL_set_fd`, because on
io_uring the loop already owns the bytes: multishot recv has read them before
OpenSSL is involved. So the `Session` is a pure byte transform:

```
  socket --recv--> feed_ciphertext() -> [rbio] -> SSL -> read_plaintext() --> rbuf
  socket <-send--  pull_ciphertext() <- [wbio] <- SSL <- write_plaintext() <-- out
```

The same four calls serve both directions and both backends. Only the handshake
role differs (`SSL_set_accept_state` against `SSL_set_connect_state`) and what is
verified: the client leg presents a certificate and verifies nobody, the upstream
leg verifies the provider's chain and its hostname, with no way to disable it.


### 10b. Two offsets, two questions

For a TLS connection, `out` (plaintext) is not what gets written; the plaintext
stays intact there so a stale pooled connection can be retried.

| field | answers |
|---|---|
| `out` sent | plaintext handed to the transport. For TLS that means fed into the Session, which has not necessarily left the machine |
| `tls_out` sent | ciphertext that actually reached the socket |

So `out.idle()` means "fully encrypted", never "fully sent". Ask
`tls_out_flushed()` for the second question.

A transport-agnostic wrapper over the two was tried and reverted: every caller
already sits inside a TLS-only branch, so it resolved to `tls_out_flushed()` at
every site and read as a safety net while changing nothing.

### 10c. Why the TLS helpers take a `ConnBase`

They are no-op-safe building blocks shared by both backends, and since inbound TLS
landed they run on client connections too. What differs by direction lives in the two
`tls_feed` overloads, chosen by type: a client's failed handshake is counted as scanner
noise, and an upstream's completed one stamps t2 and pushes the request that waited
for it.

### 10d. Two TLS checks that look removable

`tls_invariant_ok()` refuses a write on a connection the configuration says must be
encrypted but that has no Session: on the client leg those bytes would answer a peer
that dialled TLS, on the upstream leg the next thing sent is the provider credential.
It cannot fire today, and a mutation sweep confirms no test can tell it from `return
true`. It stays because what makes it unreachable is six call sites that each close
the connection when `tls_attach_*` fails, an invariant re-established by hand, two of
them added the week inbound TLS landed. A guard whose precondition is one structural
fact is noise and gets deleted; the test is how many places must stay right for it to
stay unreachable.

`tls_feed()` logs `Session::last_error()` on a failed handshake. Before it did, wrong
CA, wrong hostname and a protocol mismatch all looked like a closed socket. A failure
on the client leg during the handshake is almost always an internet scanner speaking
junk at the listener, so it is logged at DEBUG, where an info-floor build stays quiet,
and counted in `client_tls_handshake_failures`: the line is noise, the rate is a
diagnostic, and a customer who cannot handshake shows up as a step in it. An upstream
handshake failure or a mid-session failure on either leg stays WARN.

## 11. Where each rule is enforced

Documentation drifts; these do not.

| rule | enforced by |
|---|---|
| `core/` includes no backend header (`<sys/epoll.h>`, `<linux/io_uring.h>`, `net/uring.hpp`, `io/*`) | `scripts/check_conventions.py`, CI |
| `core/` makes no socket call (`::read`, `::write`, `::send`, `::accept`, `::close`, `epoll_ctl`) | same |
| `io/epoll*` never mentions io_uring, and `io/uring*` never mentions epoll | same |
| each of those rules still catches a planted violation | same, before it checks the tree |
| one backend calls the other | the compiler: they are separate `final` classes |
| namespaces mirror directories | `scripts/check_conventions.py` |
| LATENCY.md's stamp table names real functions that assign the stamp | same |
| constants are `kPascalCase`, types `PascalCase`, `ALL_CAPS` is macros only | same |
| the shared lifecycle behaves under a send that does not complete | `gateway/tests/core_test.cpp`, with a `FakeIo` |

A green suite proves nothing it
checks regressed; it does not prove it checks anything in particular. The
mutation harness breaks each invariant on purpose and reports whether any test
notices. It has found guards that were decoration, and test gaps in code that
looked covered.

## 12. Request bodies forwarded as they arrive

A request is framed when its last byte is in `c->rbuf`, so without this section a
10 MB upload reaches the venue only after the client finished sending it: one full
upload time added to the request. `gateway/src/core/body.cpp` sends such a body on
as it arrives instead, when nothing about it needs the whole body:

- the head is complete, the body is at least 16 KiB and not yet all here, and it is
  not content-encoded;
- no policy is installed (a policy reads the body before it routes);
- the plan is a byte-forward: a translation reads the whole body;
- an OpenAI client's body streams only with `--stream-openai-body on`, the default.
  Off, it is held to its last byte exactly as before.

`try_frame` opens the request at the head (`open_request`: t0, the sequence number,
the sink's capture) and dispatches it with the body so far. `dispatch` writes the
head and that prefix to the upstream; `body_sent` counts what went. Each later read
calls `pump_body`, which appends the new bytes to the upstream's `out` and sends.
The body stays in `rbuf` (`body_in_rbuf`) until the first response byte, so a stale
pooled connection's retry rebuilds the head and resends every byte sent so far.

**Checked before it goes.** An OpenAI client's JSON body (`Content-Type` JSON) is
fed to `provider::BodyScan` before any byte is forwarded: RFC 8259 syntax, nesting,
and the top-level and `stream_options` keys that a held body would have been
refused for (repeated, escaped, too many). `safe()` holds back a key whose closing
quote has not arrived, so a repeated `"model"` never leaves, even when it is split
across reads or sits in the last one. On a refusal the gateway stops sending, sets
`SO_LINGER` 0 on the upstream and closes it: the venue sees a reset and a body
short of its `Content-Length`, which it never runs, and the connection is never
pooled. The client gets the 400 a held body would have got. Other clients' bodies
are not scanned; their translators would refuse a bad one, and here none runs.

**A response before the last byte.** A venue may answer early (an error, usually).
`end_body_stream` stops forwarding, marks the upstream not poolable, drops the copy
in `rbuf` and counts the rest of the body as `discard`: those bytes are read and
thrown away as they arrive, so the client connection stays in step for its next
request. A reply that closes the connection waits for that drain
(`close_on_drain`). It takes a response byte: a TLS venue's session tickets arrive
after its handshake as records with no plaintext, and the body goes on past them.

**Stamps.** t0 is the head's framing, so the rest of the upload is not request-path
time: it lands in the upstream wait, where the venue is reading it. t3 is restamped
as each part goes.
