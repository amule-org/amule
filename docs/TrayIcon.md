# Tray icon backends

Linux wxGTK3 builds with libayatana-appindicator3 (or Canonical's compatible appindicator3) install `org.amule.aMule-tray-{high,low,disconnected}` as scalable SVGs in the hicolor icon theme. CMake generates these centred variants from the original tray artwork, omitting the unused speed-bar columns. Connection changes select the corresponding coloured artwork when the icon theme contains it, otherwise the indicator uses the existing `org.amule.aMule` icon for build-tree runs and upgrades where the new icons are unavailable. AppImages also publish `$APPDIR/usr/share/icons` through AppIndicator's icon-theme path so the separate SNI host can find the bundled artwork. Visibility remains independent of connection state. SNI has no speed overlay; rates remain available in the window and tray menu.

AppIndicator3 is selected only for wxGTK3, avoiding incompatible GTK major versions and GTK calls on wxQt window handles. Other builds use wxTaskBarIcon without adding a GTK development-package dependency. GtkStatusIcon slot sizing remains the responsibility of wxWidgets.

Cocoa uses a public NSStatusItem adapter with template artwork so AppKit supplies light, dark and highlighted tinting. The adapter preserves wx menu command dispatch, scales to the menu bar and its screen, and reuses an unchanged image and tooltip between rate ticks.

## Regression checks

`IconArtworkTest` verifies SVG and PNG fallbacks, distinct template alpha masks, monochrome pixels and speed-bar composition at multiple sizes. `NativeTrayIconMacTest` checks the displayed NSImage template flag, dimensions, tooltip, repeated updates, menu dispatch and removal/restoration on Cocoa. A macOS build with a different wx toolkit skips that Cocoa-only test.

Run GUI tests with a working display. Linux CI can use `xvfb-run -a ctest --test-dir build -R IconArtwork --output-on-failure`. `scripts/check-tray-backend-build.py build --require-remote-gui` compiles the fallback production consumers even when libayatana is installed. Theme lookup should also be checked with and without the new state icons and after a staged installation. Native platform CI and desktop smoke checks remain necessary for OS-specific behavior.
