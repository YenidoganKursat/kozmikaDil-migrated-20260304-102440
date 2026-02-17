# Concurrency Model

Date: 2026-02-17

## Surface Primitives
- async:
  - `async fn`, `async def`
  - `await`
  - `async for`
- structured task control:
  - `with task_group(...) as g`
  - `spawn`, `join`, `cancel`
  - `deadline(ms)` timeout alias
- data parallel:
  - `parallel_for`
  - `par_map`
  - `par_reduce`
- event-driven:
  - `channel(capacity)`
  - `send`, `recv`, `close`
  - `stream`, `anext`

## Runtime
- work-stealing scheduler:
  - worker-local queues + steal path
  - fire-and-forget fast path for chunked parallel work
- join/await assist:
  - waiting threads can execute pending tasks
- channel runtime:
  - bounded/unbounded queues
  - transition-based notifications for lower wake overhead

## Safety and Diagnostics
- analyzer provides conservative sendability checks on spawn/parallel captures.
- non-sendable capture use is reported before runtime.
- async state-machine visibility:
  - `k analyze file.k --dump-async-sm`

## Performance Contracts
- chunked data parallel execution (configurable chunk size)
- measured overhead metrics tracked in phase9/phase10 benches:
  - spawn/join ns per task
  - channel latency and throughput
  - parallel scaling efficiency across thread counts
