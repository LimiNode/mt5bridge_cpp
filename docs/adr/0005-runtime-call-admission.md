# ADR-0005: Runtime-call admission and shutdown barrier

## Status

Accepted. Implemented in the embedded runtime before Stage 2 trade dispatch.

## Context

`mt5_bridge.dll` owns one CPython interpreter, while the realtime poller and
host callers may invoke the bridge concurrently. `g_mutex` protects lifecycle
state and subscription registries; `g_python_mutex` serializes calls into the
interpreter. Holding both mutexes over a MetaTrader Python call makes a slow
terminal/IPC operation block state-only work such as diagnostics,
unsubscribe, and event processing. It also makes it difficult to reason about
shutdown when a call is already in flight.

Stage 2 will add a side-effecting `order_send`, so this lifecycle boundary must
be explicit before durable dispatch is introduced.

## Decision

All Python/MT5 operations enter through `RuntimeCallAdmission`:

1. Lock `g_mutex` and require `RuntimeState::running`.
2. Increment the in-flight call count while still holding `g_mutex`.
3. Release `g_mutex`.
4. Acquire `g_python_mutex`, then acquire the GIL and perform the Python/MT5
   work.
5. Release the GIL and `g_python_mutex`, then decrement the in-flight count
   before touching runtime state again.

The in-flight count is a lifecycle safety barrier, not a state mutex. It
allows state-only operations to proceed while Python/MT5 is slow, but prevents
interpreter finalization from racing an admitted call.

Shutdown marks the state `shutting_down`, stops and joins the realtime poller,
then waits on the in-flight count before calling `MetaTrader5.shutdown()` or
`Py_FinalizeEx()`. The condition-variable wait releases `g_mutex` while an
admitted call completes, so its destructor can decrement the count. New calls
are rejected after the state transition; already admitted calls finish before
finalization.

Initialization remains a lifecycle transition serialized by `g_mutex` and
`g_python_mutex`; it cannot race shutdown because the state mutex is held from
the initial interpreter setup through the transition to `running`.

## Invariants

- No Python/MT5 call holds `g_mutex` while waiting on terminal IPC or executing
  Python code.
- At most one thread executes Python/MT5 code at a time.
- Finalization starts only after the in-flight admission count reaches zero.
- Calls arriving after `shutting_down` receive a deterministic lifecycle
  error and do not enter the interpreter.
- Native callbacks and host state-only operations remain outside Python and
  lifecycle critical sections.

## Acceptance

The fake-runtime test suite blocks an `order_check` call in Python and checks
that a concurrent state-only `unsubscribe_all` completes before the blocked
call is released, while a concurrent shutdown remains pending until the call
finishes. The full CTest suite must remain green, including owned
interpreter/realtime shutdown tests and the client-only build with
`MT5BRIDGE_BUILD_RUNTIME=OFF`.
