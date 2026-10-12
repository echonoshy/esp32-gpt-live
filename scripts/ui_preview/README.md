# Device UI preview

Builds the actual `firmware/main/ui.c`, `menu.c`, and embedded fonts with the same LVGL dependency as the firmware. Uses a 240×320 RGB565 display and the firmware's 24 KiB LVGL pool; no device or voice service is contacted.

After ESP-IDF has resolved `firmware/managed_components`, run:

```sh
cmake -S scripts/ui_preview -B /tmp/mic-on-ui-build
cmake --build /tmp/mic-on-ui-build -j8
mkdir -p artifacts/ui-preview
cd artifacts/ui-preview
/tmp/mic-on-ui-build/preview
```

`-DLVGL_DIR=/path/to/lvgl` can select the matching dependency from another existing build. The executable writes PPM images for all three home selections, voice states, settings and QR pages. It checks every visible label for clipping, confirms network events cannot overwrite home, checks home sleep/wake, and cycles between home/chat/QR 100 times to detect allocation leaks. These images are firmware renderings, not photographs or real-device interaction verification.

Home selection commits final geometry immediately. The preview captures subsequent timer ticks to check that font sizes and layout do not change a second time.

`preview --update-only` renders the four firmware-update states without running the full navigation loop. Full preview also covers Wi-Fi connection results and both setup QR steps. Output directories are ignored artifacts; no online gallery/mock server is required.

On macOS, use a matching Xcode SDK if the Command Line Tools SDK is newer than the linker (set `SDKROOT` before configure/build). These are local host dependencies, unrelated to the ESP-IDF toolchain.
