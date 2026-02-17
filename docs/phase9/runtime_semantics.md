# Phase 9 Runtime Semantics

## Structured Concurrency
- `with task_group([timeout_ms]) as g:` opens a structured task scope.
- `as g` binding remains visible after scope exit (Python-style name lifetime).
- `deadline(ms)` returns a validated timeout token and can be used wherever timeout arguments are accepted.
- `g.spawn(fn, ...)` attaches task to group cancellation token.
- `g.join_all()` waits all tasks and returns list of results.
- Scope exit auto-joins. On failure path, runtime triggers cancel then join.

## Async
- `async def` / `async fn` returns `Task[T]` at call site.
- `await task` and `join(task [, timeout_ms])` wait for completion.
- `async for x in stream_expr:` consumes channel/stream values until drained (`None` terminator).
- Analyzer exposes pseudo state-machine lowering with `sparkc analyze file.k --dump-async-sm`.

## Parallel Primitives
- `parallel_for(start, stop, fn [, extra...])`: chunked work submission.
- `par_map(list, fn)`: parallel element mapping.
- `par_reduce(list, init, fn)`: parallel local reduce + final combine.

## Channels / Streams
- `channel(capacity)` creates bounded (`capacity > 0`) or unbounded (`capacity == 0`) queue.
- `send` blocks on bounded full queue.
- `recv` blocks until value/close; supports timeout.
- `stream(ch)` returns channel-backed stream handle.
- `anext(stream)` / `stream.anext()` consumes one item; `None` when closed and drained.
- `stream.has_next()` is true when queue has pending data or channel is still open.

## Safety Policy (Current)
- Task/parallel callable argument must be named function (diagnostic otherwise).
- Non-sendable captured args in spawn/parallel calls are diagnosed conservatively.
- Full ownership/borrow-based race proofs are deferred.
