# Dispatch implementation guide

This directory owns durable journal storage and one-shot dispatch orchestration.
It must not expose a public side-effect API or call `order_send` directly.
Private headers stay beside their implementation files. Dispatch code may use
public reconciliation contracts and narrow trade transport seams, but must not
own CPython interpreter lifecycle.

Private manager preparation and dispatch hold the account lease capability
across the complete eligibility scan and durable admission. The production
Windows capability is serialized in-process; same-thread nesting is required
by B3D -> B3B verification. In the manager path acquire the account capability
before journal file locks, release file locks before transport, and never
re-enter the manager from a transport callback. Each manager/journal instance
still belongs to one caller lane; sharing a lease is not permission to share
mutable owner state concurrently. Do not hold one account capability while
acquiring another account's capability.
