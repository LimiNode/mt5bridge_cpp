# TODO

Planned enhancements:

- Additional APIs for broader MetaTrader 5 coverage.
- Structured return types for clearer data handling.
- Logging facilities for diagnostics and troubleshooting.
- Flexible configuration system (files, environment variables, or CLI).
- Continuous integration setup for automated builds and tests.
- Installer or packaging scripts for streamlined deployment.
- Runtime bundle manifest and relocation checks for `mt5_bridge.dll` plus the
  matching CPython/MetaTrader5 environment.

Before adding any item, extend the existing dispatcher and C ABI rather than
creating a parallel bridge path. See `docs/architecture.md` and
`docs/development-rules.md` for the accepted layering and anti-bloat review
gates.
