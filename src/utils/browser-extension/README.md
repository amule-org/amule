# eD2k Link Fixer

A Manifest V3 Chrome extension that makes eD2k links on web pages open in the
computer's registered eD2k application. Chromium rejects links containing literal
`|` characters; the extension replaces those characters with `%7C` before normal
link activation. Existing filename escapes and other URL schemes are preserved.

Your computer must already have a compatible application registered for `ed2k:`.
This extension does not install or register aMule. It needs no native messaging
host, background service or download-directory watcher.

## Install

1. Download and extract the extension ZIP, or use this directory from a checkout.
2. Open `chrome://extensions`, enable **Developer mode**, select **Load unpacked**
   and choose the directory containing `manifest.json`.
3. Refresh web pages that were open before installation.
4. Click an eD2k link and allow Chrome to open your application when prompted.

Keep the extracted folder in a permanent location. Remove or reload the extension
on Chrome's Extensions page. This is a local installation, not a Chrome Web Store
listing. See [Chrome's installation guide](https://developer.chrome.com/docs/extensions/get-started/tutorial/hello-world).
The toolbar button opens help and privacy information.

## Scope and privacy

Links on HTTP/HTTPS pages, matching frames and links added or changed after page
load are repaired. Normal clicks, keyboard activation and middle clicks keep the
browser's normal behavior. Open shadow-root links are also repaired on activation, including href changes
made by page click handlers. Activation support survives `document.open()` and
links moving between open shadow roots.
The extension does not synthesize navigation or bypass browser permission prompts.

Restricted browser pages and local files are outside its scope. Plain-text URLs,
closed shadow roots and controls that construct their own URLs in JavaScript may
need to be copied into aMule. Bulk queuing is not provided.

A canonical file link whose filename contains `%7C` or `%7c` is left unchanged.
Encoding its separators would make those filename escapes indistinguishable from
separators at the receiving application. Copy that original link into aMule;
preserving it avoids rejection or interpreting filename text as transfer metadata.

Site access is required to find links across websites. All processing happens in
the browser: no analytics, storage, external requests or native helper. Site access
can be restricted in Chrome's extension details.

Linux is the validated OS. Windows and macOS require their own working OS handler
and have not been tested. Other Chromium-based browsers have not been validated
with this packaged version.

## Build and test

Requires Node.js 22 or newer and Python 3. Run from this directory:

```sh
npm ci
npx playwright-core install chromium
npm test
npm run package
```

The ZIP is written to `dist/ed2k-link-fixer.zip`. The package script includes only
runtime assets, help, README and GPL license; no browser profiles or test output.
No build step is needed to load the source directory as an unpacked extension.

Set `AMULE_BROWSER_EXECUTABLE` to test an existing compatible Chromium executable
instead of Playwright's browser, for example `/usr/bin/google-chrome` on Linux.
The browser must support CDP's unpacked-extension loading for the smoke test.

On Debian/Ubuntu, install the Linux smoke-test prerequisites:

```sh
sudo apt-get install xvfb xauth dbus-x11 desktop-file-utils python3-gi gir1.2-atspi-2.0 at-spi2-core
npm run test:smoke
```

The smoke test builds and extracts the ZIP, runs a headed browser under a private
X server and D-Bus session, and registers a logging handler in private XDG
folders. It checks the blocked control, native first-use permission prompt,
handler delivery, dynamic links, frames, site cancellation and recovery, page
click handlers, filename escapes, activation methods and bundled help.
The default smoke test establishes OS handoff; it does not require aMule.
The optional application test also queries the actual EC queue and compares exact
filenames, file hashes and sizes, including the absence of cancelled transfers.

To additionally verify actual download-queue acceptance in an isolated aMule:

```sh
AMULE_BINARY=/path/to/current/amule-or-amuled npm run test:smoke
```

The optional application test also requires Python cryptography
(`sudo apt-get install python3-cryptography`). Use a current source build
compatible with the documented eD2k command line and maintained EC test peer.
Older installed versions can fail escaped-filename checks because of their own
decoding behavior; do not weaken those assertions to accept a changed filename. Autoconnection, Kad and UPnP are disabled in its private config.
The fixture seeds `lastversion` for GIT, 3.0.1 and 3.2.0 to avoid the first-run
version notice. Add the tested version there when testing another release.
Smoke tests retain their evidence in the printed temporary directory. Missing
prerequisites return exit code 77 before fixture processes start.

The repository CI runs the DOM unit tests and the Linux logging-handler smoke
test. Windows/macOS, native context-menu selection and peer file transfer are not
covered. Test failures and evidence must not be interpreted as success merely
because Chrome logged an external-handler launch.

## License

GPL-2.0-or-later, like aMule. See [LICENSE.txt](LICENSE.txt).
