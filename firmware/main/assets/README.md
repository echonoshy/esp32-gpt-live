# mic-on wordmark

`ui_wordmark.c` is the approved heavy italic mic-on wordmark, with the notch
removed from the final `n` and the magenta dot retained above `i`.

The 124 × 31 RGB565 image occupies 7,688 bytes of flash-resident pixel data.
It is placed at (10, 8) in the shared device header. Its background and ink
match the current UI palette; regenerate if the header palette changes.

Source: `assets/brand/mic-on-wordmark-italic-solid-n.png` in the repository.
Regenerate with `python scripts/build_ui_wordmark.py` (requires Pillow).
The script selects the large approved wordmark and encodes its ink coverage
at the device size; firmware builds do not need Pillow or the source PNG.

Both firmware and the desktop LVGL preview compile this same resource.
