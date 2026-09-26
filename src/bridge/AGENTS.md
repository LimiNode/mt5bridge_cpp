# Bridge adapter guide

This directory contains the exported C ABI adapter and legacy control-plane
entry points. Keep boundary validation, error translation, and delegation here;
place substantive market, trade, dispatch, or runtime behavior in its owning
directory.

The bridge target may depend on private source headers through target-local
include paths, but no private header belongs under the public `include/` tree.
