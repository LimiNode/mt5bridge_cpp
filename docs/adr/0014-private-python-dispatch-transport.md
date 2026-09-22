# ADR-0014: Private embedded-Python dispatch transport

## Status

Accepted. This slice binds the one-shot backend to the embedded MetaTrader5
Python module without adding a public side-effecting ABI method.

## Context

The durable admission and one-shot execution layers deliberately know nothing
about Python objects or the terminal package. A terminal adapter must perform
one native `order_send` call, preserve the complete `MqlTradeResult`, and keep
transport failures separate from broker decisions. The final account check must
be as close to the native call as the adapter can make it.

`order_check()` and symbol capabilities remain advisory. In particular,
`TRADE_RETCODE_MARKET_CLOSED` (`10018`) returned by `order_send` is authoritative
broker rejection evidence even when preflight appeared successful.

## Decision

`runtime::Mt5PythonDispatchTransport` is a private implementation of
`DispatchTransport`. It is compiled only into the CPython-backed
`mt5_bridge.dll`, receives a borrowed `MetaTrader5` module and a live
`CurrentAccountProbe`, and requires the caller to hold runtime admission and
the CPython GIL.

For one `OperationRecord` it performs:

1. strict UTF-8 JSON decoding of the retained `MqlTradeRequest` payload;
2. resolving the `order_send` callable;
3. a live `account_info()` check against the immutable operation `AccountKey`;
4. exactly one `MetaTrader5.order_send(request)` call;
5. strict validation of all durable `MqlTradeResult` fields, including the
   signed 32-bit `retcode_external` field;
6. JSON serialization of the complete result as opaque `raw_result` bytes.

The adapter never retries. A valid result is returned as broker evidence:

```text
10018 and deterministic rejection codes → rejected
DONE / DONE_PARTIAL / PLACED             → reconciling seed
```

`TRADE_RETCODE_LOCKED` (`10028`) is kept as a reconciliation seed: the server
reports that the request is locked for processing, not that it was definitively
rejected.

The successful-result path is not proof that a final fill occurred; later
observation/reconciliation remains authoritative. Python exceptions, `None`,
missing fields, malformed types, and serialization failures return
`transport_failure` with no retcode or raw result. An account mismatch returns
`account_mismatch` and does not call `order_send`.

Namedtuple results are recursively normalized through `_asdict()`, including
the nested `MqlTradeRequest` echo, so durable JSON retains semantic field names
rather than reducing the nested request to an array.

The test-only `test_dispatch_transport` JSON method is compiled only when
`BUILD_TESTING` is enabled. It exercises the private adapter through the
existing runtime admission and GIL path; it is not a production API.

## Consequences

- Python and MT5 objects remain private to the runtime DLL.
- Raw broker evidence survives the existing durable journal boundary without
  exposing a Python result type to C++ consumers or ctypes callers.
- `10018 MARKET_CLOSED` is a terminal broker rejection with no implicit retry.
- An exception or malformed result remains unresolved and therefore follows
  the may-have-been-sent recovery path.
- The tiny interval between the adapter's final account probe and Python's
  `order_send` call remains terminal-runtime TOCTOU; a future adapter may
  tighten it further under terminal-specific ownership/GIL rules.

## Verification

`tests/test_fake_mt5_runtime.py` drives valid `10018`, `10009`, `10008`, and
`10028` results, a signed external retcode, a nested namedtuple request,
Python exceptions, `None`, malformed results, and account mismatch through the
test-only method. The fake module records the exact number of `order_send`
calls and the complete JSON result returned by the adapter.
