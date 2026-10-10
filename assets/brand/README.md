# Brand sources

- `mic-on-wordmark-italic-solid-n.png`: approved wordmark specimen. The final
  `n` has no notch; `scripts/build_ui_wordmark.py` uses this source to generate
  the device header resource in `firmware/main/assets/`.
- `mic-on-product-icon-v2.png`: retained product icon artwork.
- `mic-on-product-icon-v2.prompt.txt`: generation brief for the icon.

These are design sources, not disposable rendering outputs. Generated previews,
firmware binaries, device logs and backups belong in the ignored `artifacts/`
directory. Local toolchains belong in `.tools/`, and Rust builds in `target/`.
