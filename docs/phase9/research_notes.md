# Phase 9 Research Notes

Date: 2026-02-17

## Core References Used
- Cilk / work-stealing scheduler model (throughput-oriented deque + steal).
- Structured concurrency model (scope-bound task lifetime and cancellation propagation).
- Async/await state-machine lowering literature as target direction (Phase 9.5+ in this repo).
- CSP-style channels for backpressure and event-driven flow.

## Practical Conclusions Applied
- Keep runtime scheduler simple and measurable first (local deque + steal + counters).
- Use structured scope (`with task_group ...`) so orphan task risk is reduced.
- Keep channel semantics deterministic: bounded queue blocks producer, `recv` supports timeout.
- Add compile-time diagnostics for non-sendable captures in parallel/task spawn paths.

## Deferred Items
- Full MIR/SSA async state-machine lowering.
- Network event loop integration (`epoll`/`kqueue`) beyond timer-like waits.
- Strong ownership/borrow checker level race proofs.
