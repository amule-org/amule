# Menu icons and country flags

Prefer SVG for new artwork and use icons wherever they help users recognize an
action. Keep text labels, reuse the same symbol for the same action, and respect
native menu behavior. Country flags use embedded PNGs at 1x/2x/3x.

Menu artwork is from [Bootstrap Icons](https://github.com/twbs/icons), version
1.13.1, commit `ce0e49dd063243118a115f17ad1fe1fe7576d552` (MIT).
Country flags are from [flag-icons](https://github.com/lipis/flag-icons), version
7.5.0, commit `7aa5b2bdddd570ece62c812c0cb588ccdc099e2e` (MIT), the `flags/4x3`
directory. The manifest records both the annotated `v7.5.0` tag object and its
peeled commit; use the commit for raw-file URLs and source verification.
The individual copyright notices and complete licenses are retained
beside the original SVGs in `vendor/` and in the installed
[`docs/THIRDPARTY.md`](../../docs/THIRDPARTY.md).

`vendor/manifest.json` records upstream revisions, source directories and SHA-256
checksums of the unmodified originals. The editable originals must accompany
source distributions, along with the conversion script. No assets were copied
or traced from eMule or a screenshot.

The country selection retains every existing country code, adds missing ISO
country/territory flags and `xk`, and retains the existing `eu` flag. Extra
upstream symbols and non-country codes `cp`, `dg`, `ic`, `pc`, `un` and `xx` are
not imported. This is an artwork lookup, not a source of geolocation or country
names. An IP's estimated location does not identify a person's nationality.

`flags/an.png` and `flags/unknown.png` retain the existing FamFamFam public-domain
artwork by Mark James. They have no upstream SVG equivalent and remain raster artwork. Other existing aMule icons are unchanged by this import.

## Regeneration

Normal builds use checked-in artwork and require no downloads or conversion
tools. For maintenance, install `picosvg==0.23.0` in a Python virtual environment
and install `rsvg-convert` (librsvg), then run:

```sh
python src/icons/regenerate_artwork.py
python src/icons/embed_icons.py src/icons src/icons/icon_data.c
python src/icons/test_regenerate_artwork.py
```

Menu icons use the picosvg compatibility pass and retain a normalized 16×16 SVG
plus PNG fallback. Flag PNGs are rendered directly from the vendored 4:3 SVGs by
librsvg at 16×12, 32×24 and 48×36. The generated `flags/` directory contains
`<cc>.png`, `<cc>@2x.png` and `<cc>@3x.png`; it contains no SVGs. Vendored SVG
originals and license notices remain unchanged and accompany source distributions.
Legacy `an` and `unknown` have no vector original; their padded public-domain
16×12 PNGs are retained and used to render the larger densities.

Regeneration losslessly recompresses PNG image data without altering pixels or
ancillary chunks. Embedding copies checked-in bytes verbatim, including optional
2x and 3x data fields, so a normal build never recompresses artwork or depends on
which zlib implementation Python uses. All SVG bytes remain uncompressed; there
is no artwork decompressor. A maintenance test limits the complete flag PNG
payload to 500 KB and checks every density, provenance and table regeneration.

## Rendering

Country flags are embedded in `amule`, `amulegui` and `amuleapi`. No resource
search, separate artwork install component, or runtime artwork files are needed.
For a requested desktop pixel size, `CCountryFlags` chooses the smallest PNG
covering both dimensions, or the largest available PNG above 3x. Exact sizes
are used directly; other sizes use `wxIMAGE_QUALITY_HIGH`, including fractional
scales (1.5x shrinks the 2x source). Flags bypass wxBitmapBundle's nearest-neighbour
resampling. Decoded images and a bounded cache of completed bitmaps retain each
logical size/backing scale; missing codes share the unknown bitmap. Windows
icon/text cells draw HICON directly; GTK and macOS use logical bitmap sizes.

Menu SVGs use `#212529` as a replacement token. At popup construction,
`GetMenuBitmapBundle` substitutes the current menu text colour outside the global
wxArtProvider cache and recolours PNG fallbacks while preserving alpha. On Cocoa,
menu images use black alpha masks and AppKit templates so native menus provide
highlighted and disabled appearance. Text, shortcuts, checkmarks and native
platform image preferences remain unchanged.

The API serves embedded PNGs at `/flags/{code}.png`, `/flags/{code}@2x.png` and
`/flags/{code}@3x.png`, with public GET/HEAD, ETags and conditional requests. SVG
flag routes return 404. The WebUI uses `srcset` at 2x/3x and removes it on a missing
density so older APIs fall back to the base PNG. A missing base image is hidden.

## Checks

Build `IconArtworkTest` with testing and a GUI enabled. Under a native display
(or Xvfb on Linux), run:

```sh
ctest --test-dir build -R 'IconArtwork.*Test' --output-on-failure
node unittests/browser-tests/country-flags.cjs
```

The native test renders all 253 flags and menu icons at 1x/1.5x/2x/3x/4x,
checks 1.5x high-quality shrinking from 2x, exact source selection, transparent
legacy padding, bitmap reuse, unknown fallback, simulated scale transitions,
menu commands/checks/disabled states, and repeated black/white colour changes.
On Cocoa it verifies the AppKit template flag. Headless runs skip unless
`AMULE_ICON_TEST_REQUIRE_GUI` is set; CI sets it. Linux also runs dark and
high-contrast themes. `AMULE_ICON_TEST_OUTPUT` exports native renders.

The Playwright test exercises the actual CountryCell at device pixel ratios
1, 2 and 3, all country flags including legacy artwork, density selection,
1x-only API fallback and unavailable images. `AMULE_BROWSER_OUTPUT` exports
screenshots. The HTTP script separately verifies the real API's density routes,
validation, caching and HEAD behavior.

Before merge, visually inspect Windows menus/flags at 100%, 150% and 200%,
highlighted/disabled rows and high contrast, and physical mixed-DPI transitions.
Inspect macOS menus in light/dark mode and flags on Retina/non-Retina displays.
Check live OS appearance changes while the application runs. Simulated sizes
and Linux tests do not replace those native desktop sessions.

## Tray artwork

The three `tray_*.svg` files use adem4ik's v1 artwork contributed in
[aMule issue #1665](https://github.com/amule-org/amule/issues/1665),
from `amule_tray_fixed.zip` (SHA-256
`702dc1513a2bf88f7523d39aa77cc24e81767be55c70be805fb77ae375e7435f`).
The `try1_fixed/amule_22_norm_fixed.svg`, `amule_22_yellow_fixed.svg` and
`amule_22_gray_fixed.svg` files map to `tray_high`, `tray_low` and
`tray_disconnected`, respectively. Arc arguments are separated for wx 3.2's
NanoSVG parser. CSS class declarations are inlined into each element because
NanoSVG ignores stylesheet rules; fills and gradients contain the state colours
without SVG filters. The matching 22×22 PNG fallbacks are rendered with wx/NanoSVG.
The last two columns are reserved for the download-speed bar. PNG fallback
bundles resample the original directly at each requested size and clear the
reserved bar columns after filtering, so blurred edge pixels cannot occupy them.

The wxTaskBarIcon backend draws the speed bar at the pixel size requested by
`SetIcon()`. The native backend flattens the bundle; `wxEVT_DPI_CHANGED` invalidates
the tray state so the next statistics tick submits a fresh bundle even while idle.
Native tooltip updates also resubmit the retained bundle on each statistics tick:
Cocoa and GTK with wxWidgets 3.2 do not emit this DPI event, and the tray's display
can change independently of the main window. wx's AppIndicator backend skips
tooltip-only submissions to avoid repeated icon-file writes and registrations.
On Windows, preferred sizes use the system small-icon metrics at the taskbar's DPI
(typically 16/24 pixels at 100/150%), with system metrics as the older-OS fallback.
wx passes the scale of a hidden window that keeps its startup DPI, so that scale is
only the fallback when the taskbar's DPI is unavailable.
Linux AppIndicator continues to resolve the existing
scalable `org.amule.aMule` icon from the desktop theme. macOS template styling
and Linux per-state named icons remain separate work.

`IconArtworkTest` also checks tray composition at 16, 17, 22, 24, 33, 39, 44, 66, 88 and 100 pixels,
including a return from 4x to 1x, empty/half/full bars, percentages above 100,
exact percentage boundaries, distinct brown/yellow/gray state colours, white
eyes and muzzle, non-black gray planks, transparent pixels beneath the bar, and
preservation of artwork and transparency, for both SVG and PNG fallback bundles.
The transfer-rate conversion also handles zero capacity and non-finite or
out-of-range percentages before converting to an integer.
`AMULE_ICON_TEST_OUTPUT` exports tray
renders. Native Windows/macOS tray sizing and physical display transitions still
need desktop verification.

## Template tray artwork

`tray_{high,low,disconnected}_template.svg` are original aMule artwork (GPL-2.0-or-later).
A shared mule silhouette uses a check, padlock or cross to distinguish connection
states after AppKit applies its template tint. The final two columns remain clear
for the speed bar. Windows and Linux continue using the coloured tray artwork.
Render the 22-pixel PNG twins with librsvg and regenerate `icon_data.c` using
`embed_icons.py`; both vector and raster fallbacks are checked in.

## Installed tray state variants

CMake generates the installed SNI variants from the existing coloured tray artwork
at configure time and regenerates them when the originals change. They shift its
20-pixel-wide content right by one pixel on the 22-pixel canvas, replacing the
asymmetric speed-bar reserve with equal margins. SVGs use a `-1 0 22 22` viewBox;
only scalable SVGs are installed for AppIndicator theme lookup. No duplicate
sources or shifted PNGs are retained, and these variants are not separately embedded.
