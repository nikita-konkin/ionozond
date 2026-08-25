# chirpsounder2

The current acquisition path: Python 3, `digital_rf`, no GNU Radio out-of-tree
module.

Maintained separately; this directory is where it is vendored or referenced as
a submodule.

## What it needs from this project

- **an `.lfs` writer**, so its captures are readable by the console and by the
  existing archive tooling. See `docs/lfs-format.md` — and resolve the header
  version split before making it the standard producer.
- optionally an **`.lfp` writer**, so derived products are available without a
  separate pass over the archive. See `docs/lfp-format.md`.
