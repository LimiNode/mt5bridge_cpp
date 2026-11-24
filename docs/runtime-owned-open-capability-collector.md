# Runtime-owned managed OPEN capability collector

## Scope

Private follow-up to merged PR #85. This collector supplies source facts, not
canonical request bytes, a journal binding, managed ownership, a factory value
or dispatch permission. No manager integration, additional execution policy,
public ABI, production CLOSE/INOUT or live/demo trading is added.

`collect_managed_open_capabilities(expected_account, symbol, concrete_lease)`
accepts only the desired identity and an actual `WindowsSingleWriterLease`.
There is no caller-supplied POD, module, normalization, clock, ready-made token
or data-source callback. The bridge enters its existing trade-critical lane,
owns runtime admission/GIL and reads its own `MetaTrader5` module. The account
capability is acquired before the runtime lane, never while already holding it.

## Source and completeness contract

- Strictly decode required account server/login, margin/trade modes and API
  permissions. Require live account equality to the requested lease scope at
  both ends of collection. Missing, malformed, embedded-NUL, truncated or
  foreign identities fail closed; bool truthiness and lossy text truncation
  are not authority.
- Read required symbol identity, trade/execution/order/filling modes and all
  volume bounds from the runtime. Retain every known execution mode, including
  INSTANT, REQUEST and EXCHANGE; do not run the Market-only policy validator.
  An observed zero filling mask is different from an absent field.
- Derive the least-scale decimal normalization (scale <=9) from the observed
  binary64 step using B2's exact rounding-interval primitives. Validate min/max,
  nonzero directional limit and relevant exposure quantities in that domain.
  No caller-provided normalization, epsilon or rounded division is accepted.
- Use unfiltered account-wide `positions_get` and `orders_get`, successful
  concrete tuple/list results, required row fields and unique positive tickets.
  Bracket each complete enumeration with `positions_total` / `orders_total`;
  None/error, truncation/count mismatch, duplicate/unknown rows fail closed.
  Repeat the observations and compare identity/type/volume/symbol records, not
  just counts, to reject equal-count replacement or changing quantities.
  More than 100000 positions or orders is unsupported and fails closed before
  allocating row storage.
- Read symbol/account policy again and compare relevant fields; a changed
  account, permission, mode, bound, domain or book invalidates collection.
  Connected terminal and enabled terminal/API trading are required.

Directional exposure sums requested-symbol positions and active-order remaining
volume using checked canonical integer addition, separately for BUY and SELL.
Pending BUY/SELL limit/stop/stop-limit rows count in their respective direction;
unknown order types and malformed remaining volume fail closed. Zero reported
volume limit means no numeric cap, not unknown data or permission to ignore books.

## Conservative OPEN guard and freshness

The snapshot's `blocks_unencumbered_open()` is true for **any** position or
active order in the requested symbol. It does not infer ownership from magic,
netting direction, margin mode or a hedge flag. Thus an opposite netting
position, existing same-side position, external residue or pending order cannot
be treated as a new unencumbered OPEN. Collection itself can succeed with a
nonempty book so facts and directional exposure remain inspectable; successful
collection must not be interpreted as an OPEN admission decision.

The only snapshot constructor belongs to the runtime collection function. Plain
decoded data, copies of PODs and the low-level Python decoder cannot construct
one. There is no restore/deserialization seam. Data getters are const.

The held account capability covers the whole capture. The immutable result
retains its actual fencing epoch, runtime lifecycle generation and monotonic
start time. Collection and retained local age are bounded to 1000 ms.
`managed_open_capabilities_current(snapshot, concrete_lease)` rejects age expiry,
shutdown/reinitialize and account/epoch mismatch. It is a local freshness/scope
check, **not** a new broker query. Do not call collection while already owning
the runtime lane or GIL, or acquire another account capability inside it.

MT5's calls are not an atomic broker snapshot. Count checks and two matching
enumerations detect observable incompleteness/change, not an ABA change or
external trade between calls. The bridge lease excludes other bridge writers,
not manual/other-terminal trading. A future wired factory must recollect before
the durable barrier and retain normal environment admission, final account and
fencing checks, and fail-closed reconciliation. Empty observed sets are not a
permanent pure-OPEN proof or an exclusion lock.

## Verification boundary

Use the real bridge admission/GIL and concrete Windows leases against a
deterministic fake MT5 module. Cover identity/permission/known-field failures,
all four execution modes, zero versus missing filling, exact domain derivation,
non-finite/near-grid/overflow cases, complete empty and nonempty books, count
mismatch, duplicate/malformed rows and same-count replacement. Check directional
pending-order remainder and the conservative guard on both netting and hedging
accounts; check age expiry, runtime restart and new lease generation. Require
zero `order_send` calls and no operation journal records.

Repository-test-only private C++ exports permit native tests to invoke this
otherwise internal entry point. They are absent with `BUILD_TESTING=OFF`; the
installed/public headers and stable C exports do not change.

After independent review, the order remains INSTANT -> REQUEST -> EXCHANGE
policy expansion, then canonical serialization + immutable journal binding,
then manager integration and end-to-end restart/no-resend coverage.
