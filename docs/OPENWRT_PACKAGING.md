# OpenWrt Packaging Kit (`bd-j4m.7`)

This document describes the OpenWrt packaging scaffold for `asx` and the
reproducible artifact flow used by package-focused e2e validation.

## Scope

- Feed recipe scaffold: `packaging/openwrt/asx/Makefile`
- Init hook for startup sanity checks: `packaging/openwrt/asx/files/asx.init`
- Deterministic package artifact builder:
  `tools/packaging/openwrt/build_package.sh`
- Package e2e lane: `tests/e2e/openwrt_package.sh`

## Artifact Builder

Build deterministic `.ipk` artifacts locally:

```bash
tools/packaging/openwrt/build_package.sh \
  --output-dir build/openwrt-package \
  --source-date-epoch 1700000000
```

Outputs:

- `build/openwrt-package/artifacts/asx_<ver>-<rel>_all.ipk`
- `build/openwrt-package/artifacts/asx_<ver>-<rel>_all.ipk.sha256`
- `build/openwrt-package/artifacts/openwrt-package-manifest.json`

Manifest includes operator commands for:

- install
- upgrade
- rollback
- startup sanity

## Reproducibility Contract

Given identical source + `SOURCE_DATE_EPOCH`, package checksum must be stable.

The e2e lane enforces this by running the builder twice:

1. full build (`make build` + package)
2. rebuild with `--skip-build`

and asserting equal `.ipk` hashes.

## Package E2E Lane

Run directly:

```bash
tests/e2e/openwrt_package.sh
```

Run as part of full e2e suite:

```bash
tests/e2e/run_all.sh
```

The suite records the package lane as `GATE-E2E-PACKAGE`.
