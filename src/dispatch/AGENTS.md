# Dispatch implementation guide

This directory owns durable journal storage and one-shot dispatch orchestration.
It must not expose a public side-effect API or call `order_send` directly.
Private headers stay beside their implementation files. Dispatch code may use
public reconciliation contracts and narrow trade transport seams, but must not
own CPython interpreter lifecycle.
