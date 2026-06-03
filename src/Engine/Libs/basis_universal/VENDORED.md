# Vendored: Basis Universal Transcoder + Zstd

- **Upstream**: https://github.com/BinomialLLC/basis_universal
- **Tag**: v2_1_0
- **Commit**: 45d5f41015eecd9570d5a3f89ab9cc0037a25063
- **Date vendored**: 2026-05-22
- **Contents**: `transcoder/` and `zstd/` only — the encoder was NOT included.
- **License**: Apache-2.0 (see `transcoder/` and upstream LICENSE file)

## Notes

Only the decode-side (transcoder) is vendored here. The Basis encoder runs offline
as a separate tool and is not part of the engine.

The `zstd/` directory contains Meta's Zstandard single-file amalgamation, required
by the KTX2 transcoder path for Zstd-supercompressed UASTC payloads
(`BASISD_SUPPORT_KTX2_ZSTD=1`, the default).
