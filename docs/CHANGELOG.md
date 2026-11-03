# aMule Changelog

The history of aMule 3.x releases. Versions are listed newest first.

Older releases are in [CHANGELOG-2.x.md](CHANGELOG-2.x.md) (2.x) and [CHANGELOG-1.x.md](CHANGELOG-1.x.md) (1.x).

---

## Version 3.2.0 - "The secure until proven otherwise edition"
2026-10-28

A security release first: it fixes 15 vulnerabilities reported through GitHub Security Advisories, and 11 of them are also present in aMule 2.3.3. If you still run 2.3.x or any 3.x release, please upgrade. The release is 455 merged PRs on top of 3.1.0, including the 12 advisory fixes merged at release time. Around those fixes: uploads that no longer stall behind a slow disk, new downloads that find sources sooner, an "All" search across eD2k and Kad, chat spam protection everywhere, an appearance setting, connection-state tray icons on Linux, and the Web UI in seven languages.

Upgrading is a drop-in, but a few defaults changed for safety. Read [Before you upgrade](#before-you-upgrade) if you drive aMule from a remote GUI, amuleapi, amuleweb or scripts.

### Highlights

- **Safer against other users and your network.** 15 security problems are fixed. Other eD2k and Kad users, devices on your local network and remote-control users can no longer crash aMule, read parts of its memory, or reach files and programs outside it. The [table below](#security) shows each one and whether 2.3.3 has it too.
- **No more freezes from a slow disk.** A slow, busy or sleeping disk used to freeze the GUI and every remote control for seconds, sometimes over 20. Now replies take milliseconds, and uploads from your other disks keep going at full speed: 93 MiB/s instead of 10 MiB/s in our test (#2067, #2073, #1775, #1779, #1889, #1942). [See the numbers](#performance-measured).
- **Faster uploads that use less CPU.** Up to 16% more upload speed and about half the CPU per MiB at low speeds (#2088, #1879). No CPU is wasted compressing videos and other files that are already compressed (#1816, #1932). Your upload slots are shared fairly, because one machine can no longer take most of them by posing as many users (#2043, #2092). [See the numbers](#performance-measured).
- **New downloads start sooner.** aMule asks the servers for sources as soon as you add or resume a download, within seconds, instead of waiting up to 22 minutes for the next round of server requests (#2064, #2110, #2079). With a download limit set, aMule now answers new peers at once instead of letting some time out, so you lose fewer sources (#2065). [See the numbers](#performance-measured).
- **Search both networks at once.** One "All" search covers eD2k and Kad (#1670). The Web UI can refill a past search's form with one click, ready to run again (#2062).
- **Less chat spam, whatever you use.** The spam filter and chat CAPTCHA now protect amuled and every client connected to it, not only the desktop app, and they're on by default (#2089). amulegui, amuleapi and the Web UI can also answer the CAPTCHAs other peers send (#1829).
- **You decide which folders remote clients can use.** A remote GUI, amuleapi or script can only set download and shared folders inside a list you keep on the aMule machine, so a leaked password no longer exposes the whole disk. Folders you already use keep working.
- **A nicer desktop app.** Pick Light, Dark or follow the system on Windows and macOS (#1922); set bandwidth with sliders and an Mbit/s calculator (#1920, #2004); find preferences in eMule's familiar order (#1919); see progress while aMule shuts down (#1924); use the Linux tray icon in builds without libayatana-appindicator and turn it back on without a restart, now with connection-state icons (#2036, #2045); and get tray icons and speed bars drawn sharp at any screen scale on Windows and macOS (#2026).
- **Damaged files are caught and not spread.** Verify Local Data remembers its result and shows it in amulegui and File Details (#1638, #1691). aMule stops offering parts it found damaged, so peers no longer download bad data from you (#1721). It can also recheck shared files every few days, if you turn it on (#1948).
- **The Web UI speaks seven languages.** Catalan, Italian, Dutch, Portuguese (Brazil) and Russian join English and Spanish (#1755, #1773, #2131), and its catalogues are now translated on Weblate with the rest of aMule (#1737).
- **A better server list.** aMule combines server lists from several sources you choose, and ignores a broken download instead of importing it (#1923).

### Before you upgrade

- **Remote folder changes are refused until you allow them.** amulegui, amuleapi and EC clients can only set the incoming, temporary, shared and category folders to a folder under the list in `allowed-remote-dirs.dat` (one absolute path per line, in the config folder). The list is empty by default. In the monolithic aMule, edit it under Preferences → Remote Controls; on a daemon-only install, edit the file and restart amuled. Folders already configured keep working, and removing a folder is always allowed. amuleapi answers `403 path_not_allowed` for a refused folder.
- **Some settings are local only.** The ffprobe path, the online-signature folder and "Follow symbolic links in shared folders" can no longer be changed over EC or amuleapi, and the web server template can only be set to a template name, not a path. Set them in the local GUI or in `amule.conf`.
- **amuleweb guests need guest access turned on, and are read-only.** A guest password alone no longer lets a guest log in: turn on "Enable Low rights User" in Preferences → Remote Controls, or start amuleweb with `--allow-guest`. A guest can no longer add downloads or make the core fetch URLs.
- **Downloads use http and https only.** Server lists, the IP filter, nodes.dat and GeoIP updates from any other kind of address are refused.
- **amuled now applies chat spam protection and CAPTCHA.** The monolithic aMule already did; amuled ignored both settings, although they defaulted to on. A saved setting, including an explicit opt-out, is kept, so a daemon whose amule.conf already says on now enforces it. Both can be switched off under Preferences → Filters (#2089).
- **Office documents are not shared by default** (#2098). This applies to new profiles and to profiles from 3.0.x or older; a profile whose preferences were saved by 3.1.0 keeps its saved list, even an empty one. To get the same on such a profile, append this to "Exclude files and folders matching" under Preferences → Directories (wildcard mode, not regex; add a `|` first if the list is not empty):
  `*.doc|*.docx|*.docm|*.dot|*.dotx|*.dotm|*.xls|*.xlsx|*.xlsm|*.xlsb|*.xlt|*.xltx|*.xltm|*.xlam|*.ppt|*.pptx|*.pptm|*.pot|*.potx|*.potm|*.pps|*.ppsx|*.ppsm|*.odt|*.ott|*.ods|*.ots|*.odp|*.otp|*.odg|*.otg|*.odf|*.odb|*.rtf|*.csv|*.tsv`
  Downloads stay shared while they are incomplete.
- **Share exclusion patterns now match folder names too** (#1571). A subfolder whose name matches a pattern, inside a share that includes its subfolders, is now left out with everything in it; a folder you share yourself is still shared.
- **A server on this machine or your LAN needs "Filter LAN IPs" off** (#1864), and a server can only update your server list while "Update server list when connecting to a server" is on.
- **amuleapi refuses cookie-authenticated writes from other origins.** Behind a reverse proxy that rewrites `Host`, set `X-Forwarded-Host` (see QUICKSTART-AMULEAPI.md), or older browsers get 403 on their own changes.
- **Event commands that put an event value inside interpreter code are refused**, for example `sh -c "… %NAME"`. Values are now passed as literal arguments; the refusal is logged (#1774).

### Security

All 15 problems below are fixed in 3.2.0. "Also in 2.3.3" tells you whether the old 2.3.3 release has the same problem. Every problem is also in 3.0.0, 3.0.1 and 3.1.0, except where the last column says otherwise.

"Remote control" below means the connection that amulegui, amulecmd, amuleweb and amuleapi use to control aMule (External Connections, port 4712 by default).

| Advisory | Severity | What could happen | Who could do it | Also in 2.3.3 |
|---|---|---|---|---|
| [GHSA-cp82](https://github.com/amule-org/amule/security/advisories/GHSA-cp82-q55r-936c) | High | aMule could crash, and possibly be taken over | A device on your local network, if UPnP is on | Yes |
| [GHSA-2crf](https://github.com/amule-org/amule/security/advisories/GHSA-2crf-cv3v-53w5) | High | aMule or amuled could crash, without a password | Anyone who can reach the remote-control port | Yes |
| [GHSA-52mv](https://github.com/amule-org/amule/security/advisories/GHSA-52mv-r67q-c985) | High | aMule could freeze for tens of seconds, again and again | Any Kad user | Yes |
| [GHSA-q2jr](https://github.com/amule-org/amule/security/advisories/GHSA-q2jr-xf54-v364) | High | Files could be moved anywhere on the computer that runs aMule | A remote-control user | Yes |
| [GHSA-3j74](https://github.com/amule-org/amule/security/advisories/GHSA-3j74-m5qw-75xg) | High | aMule or amuled could be made to run any program | A remote-control user | No, new in 3.1.0 |
| [GHSA-cvxg](https://github.com/amule-org/amule/security/advisories/GHSA-cvxg-rcqx-rp9p) | Medium | Parts of aMule's memory could leak | Any eD2k user | Yes |
| [GHSA-g22c](https://github.com/amule-org/amule/security/advisories/GHSA-g22c-pmwf-qmwm) | Medium | The statistics page could crash aMule, amulegui, amulecmd or amuleweb | Any eD2k user | Yes |
| [GHSA-6qxf](https://github.com/amule-org/amule/security/advisories/GHSA-6qxf-6fj4-q8fv) | Medium | Parts of aMule's memory could leak into its log, without a password | Anyone who can reach the remote-control port | Yes |
| [GHSA-hv8p](https://github.com/amule-org/amule/security/advisories/GHSA-hv8p-rp6f-rmf7) | Medium | aMule could use up all memory | Someone you download from | Yes |
| [GHSA-vh6g](https://github.com/amule-org/amule/security/advisories/GHSA-vh6g-8m3v-wfmf) | Medium | An amuleweb guest could add downloads and make aMule fetch web pages | An amuleweb guest | Yes |
| [GHSA-42j9](https://github.com/amule-org/amule/security/advisories/GHSA-42j9-4vh7-jvw6) | Medium | A bad update file could fill the disk or crash aMule | Whoever serves your IP filter, server list or GeoIP updates | Yes |
| [GHSA-7rxg](https://github.com/amule-org/amule/security/advisories/GHSA-7rxg-6w2f-3w57) | Medium | You could get a different file than the link you added, and aMule would not notice | Someone you download from | Yes |
| [GHSA-frfq](https://github.com/amule-org/amule/security/advisories/GHSA-frfq-582f-4jww) | Medium | A bad server-list address could make aMule send data to other programs on your computer or your network | Whoever gives you a server-list address | No, since 3.0.0 |
| [GHSA-gc7p](https://github.com/amule-org/amule/security/advisories/GHSA-gc7p-357m-xj2j) | Low | amulecmd, amuleweb or amuleapi could show fake data, even with encryption on | Someone on the network between them and aMule | No, new in 3.1.0 |
| [GHSA-vj9g](https://github.com/amule-org/amule/security/advisories/GHSA-vj9g-4mg6-q9hw) | Low | A web page served from the same host as amuleapi, such as another port, could change aMule through it while you are logged in | A web page on the same host or site as amuleapi | No, new in 3.1.0 |

Since 3.1.0, a new installation's remote-control port only accepts connections from the same computer, unless you change that to use amulegui or amuleweb from another one. A configuration first saved by 2.3.x or 3.0.x keeps listening on every network interface while remote connections are enabled; set the address under Preferences → Remote Controls.

**Other hardening.** Beyond the advisories, this release closes more ways that servers, peers, links and update files could crash aMule, slow it down or use too much memory or disk (#1701, #1864, #1865, #1867, #1868, #1869, #1873, #1862, #1890, #1900, #1925, #2054); #1900 also carries the TCP half of the GHSA-cvxg fix. amuleapi guests no longer see your MaxMind licence, proxy user name, or GeoIP, server-list, IP-filter and nodes.dat update addresses (#1860, #1974), and amuleapi passwords can be set without typing them on the command line (#1973).

### Performance, measured

These numbers come from the tests in each PR, comparing the code just before and just after it. They are not one 3.1.0 against 3.2.0 benchmark, and your results depend on your disks, line and peers.

**aMule stays responsive when the disk is slow.** In 3.1.0 a slow disk could freeze the GUI and every remote control for seconds.

| Test | Before | After | Change |
|---|---|---|---|
| Every upload read takes 400 ms longer: typical remote-control reply (#1775) | 297 ms | 0.9 ms | 330× faster |
| Slow reads while an upload slot changes: slowest remote-control reply (#1779) | 1.6 to 5.0 s | 8 to 62 ms | about 80× faster |
| Upload packet building stalled for 5 s: slowest remote-control reply (#2073) | 4.9 s | 8.4 ms | about 590× faster |
| Saving a download's progress takes 15 s: remote control during the save (#1889) | frozen for 15 s | slowest reply 372 ms | about 40× faster |
| The free-space check takes 3 s: remote-control replies in 3 minutes (#1942) | 25, the slowest 23.9 s | 1745, the slowest 20 ms | 70× more replies, slowest 1,200× faster |

**Uploads and downloads.**

| Test | Before | After | Change |
|---|---|---|---|
| Upload from a fast disk while another disk is slow (#2067) | 10.3 MiB/s | 93.0 MiB/s | 9× faster |
| Upload speed with a 64 MiB/s limit, from a fast SSD (#2088) | 49.1 MiB/s | 57.0 MiB/s | +16% |
| Upload speed with no limit, 3 slots (#2088) | 173 MiB/s | 184 MiB/s | +6% |
| CPU per MiB uploaded, 15 slots at 2.5 MiB/s (#2088) | 44.1 ms | 22.8 ms | −48% |
| CPU per MiB uploaded, no upload limit, test harness (#1879) | 4.00 ms | 3.09 ms | −23% (about −5% with real aMule downloaders) |
| A download added mid-session asked at other servers (#2064) | not within 5 minutes | within 1 s | at least 300× sooner |
| Second new download asked at the connected server (#2110) | after 249 s | after 2 s | about 120× sooner |
| Answering a new peer with a 20 KiB/s download limit (#2065) | up to 29 s, sometimes timed out | 3 to 6 ms | about 5,000× faster, no timeouts |
| Peak memory with repeated 2 s disk write stalls (#2078) | 320 MiB | 94 MiB | −71% |
| Peers flooding 1-byte packets (#2109) | no progress, CPU at 100%, 120 s to shut down | 0.3 MiB/s, 2 s to shut down | keeps working; shutdown 60× faster |

**Startup with a large recursive share.**

| Test | Before | After | Change |
|---|---|---|---|
| Listing 2,040 shared folders with 200,000 files, macOS / Linux (#1569) | 930 / 425 ms | 80 / 23 ms | 12× / 18× faster |
| Time to the splash screen, 20,200 shared folders (#1572) | 744 ms | 125 ms | 6× faster |

### Transfers and sources

Measured gains for the changes below are in [Performance, measured](#performance-measured).

- Upload reads on a pool of four workers, one read in flight per client and volume, and packet building outside the uploading-list lock (#2067, #2073, #2112, #2113, #1933, #2055).
- Locks no longer held across upload disk reads (#1775, #1779), and the done-block list each request walks is capped (#1801).
- Per-address upload slot caps, also when free slots remain (#2043, #2092).
- No zlib for media and other formats that are compressed by design (#1816, #1932).
- `.part.met` saves, free-space checks and part-file size checks no longer block the main thread (#1889, #1942, #1915, #1888, #1899); tiny data packets no longer stall reception (#2109); and a stalled download disk no longer lets unwritten data fill memory: aMule stops requesting new blocks until writes catch up (#2078).
- A failing disk no longer loses a download: a `.part.met` save that cannot replace the old file keeps the old one, and a failed memory-mapped write is retried instead of being counted as written (#1962, #2076).
- Sources for new downloads from the connected server and other servers, within budgets (#2064, #2110); servers added by host name get UDP requests again (stat pings, global searches and source requests to them had been dropped for many years) (#2079).
- The download limit counts only download data (#2065).
- Endgame: a failed takeover requeues the cancelled source (#1651). Block selection uses free intervals inside partly reserved blocks (#1965).
- A cancel never deletes a finished download (#1854); a download completed at startup no longer sorts as 0% (#1855); disk-space budgeting holds across automatic resumes (#1891).
- Cached read handles for uploaded complete files (#1953).

### Networking and protocol

- Live port rebinding: TCP and UDP port changes apply without a restart, also from remote clients (#1706).
- ED2K can be switched on at runtime, and the Networks page follows network changes made from other clients (#2010).
- SOCKS5 proxies that answer with 0.0.0.0, such as `ssh -D` and Tor, work again (broken in 3.1.0), and SOCKS4/4a through Tor works for the first time (#1648, #1819).
- Bind-to-interface says what it binds and no longer leaks HTTP (#1649).
- An obfuscated server connection that times out is retried without obfuscation (#2052); dead servers are counted the same way on every platform (#1650); `server.met` stays readable and deleted servers stay deleted (#1794).
- Kad: the hourly firewall recheck no longer flips Kad to firewalled (#1745); silent buddies are detected and replaced, and callbacks for another buddy are no longer relayed (#1831, #1833); persisted indexes load in the background (#1742, #1808); publication no longer slows quadratically on large libraries (#1918); string tags are UTF-8 (#1997); `TAG_BUDDYHASH` uses the eMule layout (#1993); stale UDP verification expires (#1782); callback and source publication fixes (#1781); a node firewalled on TCP with a direct UDP callback and no buddy now publishes sources, keywords and notes (#1952).
- Kad shares and uses file repair hashes (AICH, Kad protocol 0x0a), on by default and also for profiles upgraded from 3.1.0. It is a setting now, not a build option: "Share and use file repair hashes (AICH) over Kad" under Preferences → Files. The stricter filter for old publishers stays off, and file sources outrank Kad repair-hash evidence (#1663, #1725, #2044, #1713, #1738, #1739).
- UDP packets dropped before sending, for example after a failed DNS lookup, are freed (#1822); data races in sockets, uploads, part-file I/O and statistics found by ThreadSanitizer are closed (#1807); the RC4 loop is faster (#1951).
- Compressed packets that shrink by more than 10:1 are no longer dropped when they fit the size limit (#1707).
- The server connection log names the server and explains each step, using eMule's wording (#1719).
- A failed GeoIP download no longer turns country flags off, and MaxMind .tar.gz databases are unpacked instead of being saved as a tar (#1653, #1624).

### Search

- "All" search across eD2k and Kad (#1670), and an identical running search reuses its tab (#1589).
- Search parameters stored, and a Web UI search tab can refill its form from them (#2062, #2133); search state saved periodically, not only at exit (#1882).
- Corrupted UTF-8 file names are repaired (#1985); unrequested server UDP results and sources are dropped (#1856).
- One unusual tag in a server's search answer no longer makes StoredSearches.met unreadable and loses every stored search, and .part.met files no longer log "wrong tagcount" at every start (#1858).
- Completed downloads you later deleted or moved keep showing as already downloaded in search results after 30 days (#1821).
- The Codec column shows one label per format (H.264, H.265, MPEG-4…) for local files and for the codec ids peers send (#1731).

### Chat

- amuled applies the spam filter and chat CAPTCHA to incoming chats, with easier-to-read CAPTCHA images whose answers accept look-alike characters (#2089); CAPTCHAs sent by other peers can now be answered from amulegui, amuleapi and the Web UI, not only the desktop app (#1829).
- Chat peers are addressed by a stable hash over EC, amulegui and amuleapi, and older EC clients keep working with it (#1512, #1551, #1576, #1578, #1555).
- The comment filter applies to Kad notes and EC, the message-filter master switch turns every rule off, and an empty keyword entry no longer filters every message or comment (#1600, #1579, #1575).
- Add to Friends in a chat tab works in amulegui and for peers no longer connected, and new chats open in the background (#1618).

### Shared files and Verify Local Data

- Verify Local Data results persist and show in a Shared Files column, in amulegui and in File Details; every outcome is logged and a size mismatch counts as a failure; corrupt parts are no longer advertised or sent, and the file is offered to servers as incomplete (#1638, #1691, #1634, #1721).
- Optional re-verification of requested completed files every N days, off by default (#1948).
- Delete complete files from disk from the Downloads and Shared Files lists, in aMule, amulegui and the Web UI (#1692, #1950).
- Exclusion patterns match folder names (#1571); completed files that become excluded are unshared (#2108); the exclusion preview checks every file the scan saw (#1531).
- Faster startup scan: subdirectories listed without a `stat()` per file, recursive shares expanded once (#1569, #1572), known.met saved at most every 5 minutes while hashing, and the end-of-batch save no longer stalls the GUI or misses the last file (#1535, #1540).
- The share watcher no longer treats a move into a shared folder as a rename (#1789) or logs one line per folder it cannot add (#1533).
- Adding or removing one file no longer re-indexes the whole list, which stalled very large shares for seconds per hashed file and minutes per unshare (#1527).
- amulegui and amuleapi drop a completed file from the shared list when it is replaced, moved or deleted outside aMule and the shares are reloaded (#1969).

### GUI

- Appearance: Follow system, Light mode or Dark mode under Preferences → Interface, applied after a restart. Available in the Windows and macOS packages and other wxWidgets 3.3 builds, not in the AppImage or Flatpak (#1922); macOS follows live light/dark switches (#1764).
- Preferences: eMule page order (#1919), bandwidth sliders and an Mbit/s calculator (#1920, #2004), and pages that scroll on small screens (#1696, #1734, #1735).
- Lists: hidden columns and defaults recoverable, content-fitted widths, widths kept across display scales, and window placement restored (#1975, #1987, #1988, #1989, #1990); clear buttons on search and file filters (#2009); Enter and other control keys reach the lists (#1772).
- Tray: on Linux, the tray icon is offered in builds without libayatana-appindicator wherever a tray host exists, can be turned back on without a restart, and no longer re-enables Minimize to Tray on Wayland; AppIndicator shows high-ID, low-ID and disconnected icons (#2036, #2045); a monochrome menu-bar icon on macOS (#2045); icons and the speed bar drawn at native DPI on Windows and macOS (#2026).
- Kad contact distribution and bounded lookup diagnostics, in the local and remote GUI (#1743, #1741).
- Context-menu icons and multi-resolution country flags (#1694); live transfer speeds averaged over one second (#2007); source counts readable and accurate (#2016); a SecIdent warning overlay for failed signatures (#1947).
- The startup splash appears before the known-files list (known.met), client credits (clients.met) and the IP filter load, and before recursive shares are expanded, so a large library no longer starts with a long wait and nothing on screen. Its progress bar follows the real work, and closing it no longer crashes aMule (#1532, #1536, #1561, #1572). The same window shows progress while aMule shuts down (#1924).
- The Clients icon is back in the Gnome, KDE4, Papirus, Priscilla, Tango and Xfce skins (#1560, #1570, #1590, #1621, #1636, #1656).
- amulegui runs its local "download completed" command (#1785) and no longer writes a stale preferences copy back to the core (#1677).
- A key press, double-click or Enter on a folder row of a peer's shared-files browse no longer crashes aMule or amulegui (#1866).
- Cancel, Pause, Resume, Stop and Rename no longer act on a download that was removed or replaced while the confirmation or rename dialog was open (#2100, #2008); a download being hash-checked cannot be cancelled, and amuleapi answers `409 download_hashing` (#2100).
- "Reset page to defaults" works on the Advanced preferences page again (broken in 3.1.0) (#1563).
- The tooltip delay preference is applied at startup, and Cancel discards a change to it (#1564).
- The download progress window opens only when a click starts a download, not for the GeoIP and IP-filter refreshes at every start (#1538, #1552).
- Layout fixes: File Details on GTK (#1610), the Preferences dialog on macOS (#1712), the Networks log pane on GTK (#1954) and the first-run window on GTK when it cannot be maximized (#1936).
- Smaller fixes: a middle click on a tab's close button closes the tab (#1557); the "&" shows in two first-run wizard page titles (#1628); the kbps unit is translatable (#1554); the source-seeds option says which sources it keeps and for how long (#1726); the client history records the real obfuscation state (#1961).

### amuleapi and the Web UI

- Web UI in Catalan, Italian, Dutch, Portuguese (Brazil) and Russian, translated on Weblate (#1737, #1755, #1773, #2131).
- `[Server]/BasePath` for reverse proxies on a sub-path (#1558); accent-insensitive filter boxes (#1815); the External Connections settings shown read-only in the Web UI and the API (#1614, #1660).
- `/events` streams end with their session, and admin slots are reserved (#1861); an automatically launched amuleapi restarts after a failure (#1897) and reports a thread limit instead of hanging (#1983).
- Magnet links accepted, and every link in a paste kept (#1640); the Kad bootstrap address sent in Kad byte order (#1711).
- "Run amuleapi on startup" and "Run web server on startup" find the helper next to the aMule binary, and amuleapi is told the core's actual EC address and port (#1678, #1679).
- Web UI Preferences no longer drop an edit when its parent option is unticked in the same Apply (#1657).
- Web UI layout: a wider link field, coloured labels and speed units no longer clipped, and statistics graphs that follow the window height (#1916, #1917).
- The Known clients list no longer overwrites a peer's stored totals with zeros, counts a session for a connection that never said hello, or adds rows for peers without a credit record (#2030).

### Daemon and command-line tools

- amulecmd: download without a prior listing and by search id (#1633), allocating, stopped and low-disk states shown (#1976), and shutdown delivered before exit (#2096); an id-less search command acts on this connection's own search, not another client's (#1632).
- An EC category request without a path no longer crashes amuled (#1863), and packets buffered on a closed EC connection are no longer dispatched (#2093).
- amuled no longer crashes at startup when remote connections are on and no EC password is set (Debian bug #1076605) (#1766).
- Configurable log file paths, and logs kept when a backup fails (#1744); `amuled -f` says why it stopped (#1752).
- Event commands get literal arguments and are explained in remote-GUI mode (#1774, #1769).
- Autostart and URL handlers registered the same way on every platform (#1630): each binary has its own autostart entry (#1629), one switched off in the OS stays off (#1645), the Flatpak registers them on the host (#1646), and the Windows installer keeps them across upgrades (#1643); `--configure-*` options exit 0 or 1 (#1637); clicked links keep their sources and hashes, and passing one to a running amuled no longer shows "aMule cannot start" (#1639).
- `amulecmd` and `amuleweb` respect explicit configuration paths (#2006); default folders work in non-ASCII profiles (#1913).
- amuleweb uses the web UPnP port set in Preferences instead of always 50001 (#1574).
- Man pages document `--configure-protocols`, `--configure-file-assoc` and the store each `--configure-*` option writes to, and are now also built in Dutch and Catalan (#1597, #1617, #1682).

### Preferences and defaults

- Chat spam filter and CAPTCHA get switches under Preferences → Filters; both stay on by default, as in 3.1.0 (#2089).
- Kad file repair hashes (AICH) shared and used by default, also on profiles upgraded from 3.1.0 (#1725).
- Office documents excluded from sharing in new profiles (#2098).
- GeoIP refreshed weekly, keeping the working database when an update fails (#1824).
- Download list Part column hidden by default in new profiles (#1592).
- The userhash is generated from the crypto RNG, as in eMule (#1790).
- Unused `DateTimeFormat`, `ShowAllNotCats` and `SplitterbarPosition` keys removed (#1596).

### Platform and packaging

- The Windows packages no longer ship video codec DLLs that nothing uses (portable zip 48.6 → 32.7 MB) (#2139); macOS drops AVIF and Flatpak drops HEIF and AVIF from libgd (#2140, #2145).
- Third-party notices and bundled-library licence files shipped in every package; the static tarball lists its Alpine libraries without their licence texts (#2135, #2136).
- cas writes a valid signature image on Windows, and its first-run casrc points at files that exist on every platform and install layout (#2137, #2142).
- The AppImage no longer searches the current directory for libraries, Windows ignores an ffprobe.exe other users can plant, remote.conf is created owner-only, the captcha notice no longer lets a stranger's next message skip the chat filters, and file ratings no longer overflow (#1871).
- macOS: local-network use declared in Info.plist (#1765).
- Windows: Unicode command-line arguments preserved (#1971); no bare program names handed to CreateProcess (#1894).
- Flatpak: no DRI access, and a clear message when no video player is available (#1945, #1881).
- Collections read correctly on big-endian hosts (#1594).
- CMake 3.12 is the tested minimum (#1674); `compile.sh` builds incrementally by default and gains `--clean`, `--experimental` and `--strip` (#1759).

### Groundwork, not switched on

kno continued the addressing work: native IPv6 peers and sources, IPv6 in the server login and Kad, server-coordinated NAT traversal with Lugdunum and ed2k-server, uTP hole punching, and a QUIC transport (41 PRs by kno, #1604 to #2143). It sits behind `ENABLE_IPV6`, `ENABLE_NATT_SERVER_COORDINATION`, `ENABLE_UTP` and `ENABLE_QUIC`, which are off by default. With them off, a stock 3.2.0 build connects as before, apart from the two Kad fixes listed under Networking and protocol (#1993, #1952).

The core can send duration, codec, title, album, artist and minimum-bitrate search terms, but no GUI, EC or API control sets them yet; the bitrate term only takes effect on Kad (#1939, #1887).

### Known Limitations

Carried over from 3.1.0:

- The macOS `.dmg` is **not code-signed or notarized**, and there are no plans to change that. Gatekeeper blocks the first launch. On macOS 15 and later, allow it under System Settings → Privacy & Security → Open Anyway; on macOS 14 and earlier, Control-click the app and choose Open.
- **Flathub submission still pending** - install the `.flatpak` directly with `flatpak install <file>` for now.
- AppImage isn't yet in AppImageLauncher's catalogue.

### Contributors

This release is 455 merged PRs, including the 12 advisory fixes. Particular thanks to:

- **got3nks** - 198 PRs. The fixes for 13 of the 15 advisories, the upload and source-request work, Verify Local Data follow-ups, the share watcher, tray and autostart fixes, packaging and CI.
- **3togo** - 109 PRs in a first cycle with the project. The upload worker pool (#2067), the All search, chat spam protection and CAPTCHA across every client, the appearance setting and preference reorganization, list column handling, Kad buddy recovery, and server.met sources.
- **kno** - 44 PRs. The IPv6, NAT traversal and QUIC groundwork, and stable chat identity.
- **danim7** - 9 PRs. The remote-control path hardening that fixes GHSA-3j74 and GHSA-q2jr, with the allow-list for remote folders; Verify Local Data persisted, shown everywhere and acted on; and clearer connection logs.
- **ngosang** - 7 PRs. Web UI translation on Weblate and five new Web UI languages, and 35 issues filed.
- **adem4ik** - 7 PRs restoring the Clients icon across the skins and removing unused toolbar icons, the design work below, and the most active translator this cycle.
- **mrjimenez** - 6 PRs on dialog sizing, `compile.sh`, test stability and a search arithmetic fix.
- **aresta** - 3 PRs: accent-insensitive Web UI filters, stored search parameters and cached upload read handles.
- **XG-DL** - 2 PRs: live port rebinding and periodic search saves.
- **gbcox** and **aciceri** - one PR each.

#### Design

- **adem4ik** - the artwork behind the new tray icon: the SVG icons for the connected, LowID and disconnected states, drawn sharp at any screen scale (#2026), and the monochrome version that macOS tints to match the menu bar (#2045). Also the Clients icon for the Gnome, KDE4, Papirus, Priscilla, Tango and Xfce skins, and many of the reports that shaped this cycle's interface fixes.

#### Security research

- **SpankulatorX** - reported 11 of the 15 advisories, each with a clear analysis and most with a working reproduction, is credited as finder on a twelfth (GHSA-q2jr), and moved the low-risk findings to public issues.
- **itlezy** - reported the two update and inflation advisories (GHSA-hv8p, GHSA-42j9), and the short hashset answer fixed in #1701.
- **danim7** - reported the ffprobe and rename advisories (GHSA-3j74, GHSA-q2jr), then wrote their fix.
- **mrjimenez** - credited as finder on GHSA-6qxf.

#### Testing and bug reports

- **adem4ik** (17 issues, 15 fixed), **SpankulatorX** (13, all fixed), **slrslr** (6), **demanuel** (3), **ramicastro** (3) and **itlezy** (3).
- Thanks also to everyone who reported a single bug, confirmed a fix, or ran a pre-release build.

#### Translators

Translation flows through [Weblate](https://hosted.weblate.org/projects/amule/). Credited on this cycle's translation commits, with the largest contributions first:

**Watchman89**, **Kisnov**, **Coool**, **reducedradius**, **தமிழ்நேரம்**, **mslr8**, **Michal Várady**, **thorin**, **Francisco José Juan Quintanilla**, **Mondher Njehi**, **Jaume Casado Ruiz**, **Rares Andrei Acatrini**, **Mojelumi**, **Aniol**, **Andi Chandler**, **الاصيل**, **martinjurran**, **leinaduhcs**, **cedev-1**, **Victorio Nicolas de Melo Soares**, **Télio R**, **Noé Brunner**, **Michaela Kowalski**, **Marcell Balla**, **LenUser0**, **Kurt Schumacher**, **Jack Frank**, **Ihor Samko**, **Eleonor Fernandez**, and the contributors who translate anonymously.

Several developers also carried catalogues: adem4ik, ngosang, mrjimenez, LSalami, got3nks and danim7.

### Merged PRs in this release

The entries above cover the changes users will notice. CI, test and build-tooling work (about 55 PRs) and the Weblate merges are not listed one by one; [the full compare view](https://github.com/amule-org/amule/compare/3.1.0...3.2.0) lists the commits between 3.1.0 and this tag.

Fixes to features added during this same cycle are folded into the entries above rather than listed as separate bug fixes, since no released aMule ever carried those bugs.

---

## Version 3.1.0 - "Three versions in a year! Can we have some REST?"
2026-09-21

The largest release since the 3.0.0 rebirth: 893 merged PRs on top of 3.0.1. The headline is a new way to drive aMule - `amuleapi`, a REST + Server-Sent-Events daemon shipping a complete new Web UI that covers the same ground as the desktop GUI. Around it landed transfer-performance work that lifts real-world throughput, authenticated encryption for External Connect, a top-to-bottom modernization of the GUI list controls, media metadata that now travels end to end, searches that survive a restart, a dedicated Clients page, and a sweep of memory and CPU work across the daemon, the remote GUI and the API.

Upgrading is a drop-in. No config migration, and nothing an older build cannot read back: the new per-peer metadata rides in an appended `clients.met` trailer that every existing reader ignores (#902), and EC encryption is negotiated per session and not required by default, so older amulegui / amulecmd clients keep connecting untouched (#714).

### Highlights

- **amuleapi: a REST API and SSE stream for aMule.** A standalone daemon that speaks to `amuled` as an ordinary EC client and serves `/api/v1/*` plus a live event stream, with ETag-versioned reads and per-endpoint contracts in `docs/api/` (#201).
- **A complete new Web UI, shipped with amuleapi.** Not a cut-down remote panel: Networks with the log panes, Search with multiple tabs, Downloads, Shared Files, Clients, Messages, Statistics and Preferences - the same sections the desktop GUI has, driven by SSE with throttled re-renders, in light and dark themes, responsive down to a phone, in English and Spanish (#220). It is the replacement path for `amuleweb`, which is now deprecated.
- **Faster transfers.** Endgame mode requests the final blocks from several sources at once, so a download no longer parks at 99% behind one dead peer (#225). Request and upload depth are now sized to each link's bandwidth-delay product instead of a fixed queue of three, worth up to roughly 12x on high-latency paths (#598). Kad source discovery limits, inherited from eMule and long obsolete, are raised and made configurable (#308).
- **Leaner and quicker under load.** A per-connection EC cache that never released entries for departed peers is fixed, which is what drove daemon memory growth on long-running nodes (#1424); the startup share scan does one `stat` per file instead of four (#1310); and the EC update path, the remote GUI and the API all stop redoing work nothing asked for (#775, #776, #777, #772, #878, #922, #1098, #1091).
- **External Connect is encrypted.** Authenticated encryption over the EC packet layer (AES-128-GCM baseline, ChaCha20-Poly1305 where Crypto++ offers it), keyed by an ephemeral X25519 exchange so a credential leaked later cannot decrypt a recording made earlier (#714, #785). Authentication is rate-limited (#741, #742), the daemon listens on loopback by default (#737), and amule, amuled and amuleapi share one credential store (#665).
- **Crypto++ for SHA1 and MD5.** aMule's own implementations are gone in favour of the library it already linked, picking up hardware SHA instructions where the CPU has them (#238).
- **The GUI lists are modern widgets.** Every major list control moved to `wxDataViewCtrl`: preferences sidebar, servers, friends, file details, shared files, downloads, clients and search results, with live column sorting and native dark-mode painting (#671, #830, #839, #842, #848, #850, #540).
- **Remote-to-local path mappings.** An easy one to overlook and a large quality-of-life win for anyone running amulegui against a daemon on another machine: map the core's paths to the ones the GUI can actually reach, and Open / Show in Folder start working on a remote setup (#854).
- **Media metadata, end to end.** aMule extracts Length, Bitrate and Codec from shared media with ffprobe and advertises them to servers and Kad, so other clients see them too (#280); they surface in search results, shared files, the API and the Web UI, with Artist, Album and Title columns alongside (#1343, #1344, #436, #442), and can be re-extracted on demand (#1106).
- **A Clients page.** Active peers split by direction plus a known-clients history backed by the credit store, with the same context menu and details dialog as everywhere else (#904, #902, #1271).
- **Search that remembers.** History and results both persist across restarts (#643, #856), results filter as you type (#821), and searches started by other clients are visible in the GUI (#710).
- **Chat and friends everywhere.** Peer and friend messaging works over EC and REST, not just the monolithic GUI (#1053), and the friends list is exposed over REST (#986).
- **Bind to an interface, not just an address.** A real egress-interface bind that a VPN user can rely on, using `IP_UNICAST_IF` / `IP_BOUND_IF` rather than privileged `SO_BINDTODEVICE` (#281).
- **Looks right on modern desktops.** Windows follows the system light/dark appearance (#818), toolbar and preferences icons are resolution-independent SVG for hi-DPI (#487), and the toolbar artwork was redrawn (#1494).
- **Friendlier first run.** A setup wizard on first launch (#264), a startup splash for the monolithic build (#760), and an in-app new-version check with a GUI popup and an About button, gated off for OS packages (#277, #406).
- **Packaging.** A fully static musl track producing self-contained `amuled`, `amulecmd` and `amuleapi` binaries with no shared-library dependencies (#629), libupnp 22.0.4 across every bundle (#681), and `ed2k://` and `magnet:` registered with the OS on all three platforms (#324).

### Deprecations

- **`amuleweb` is deprecated.** It still ships in 3.1.0, still works, and nothing is removed this release - but it now prints a notice at startup and carries one in its manual page (#371). Its replacement is `amuleapi` with the new Web UI, which covers the same ground as the desktop GUI rather than the subset the old PHP templates reached. A future release will retire `amuleweb`; this one gives you a cycle to move.
- **The ed2k v2 VBT code path was removed** as part of the transfer rework (#598). It was dead code no peer exercised.

### amuleapi and the Web UI

New in this release, so the entries below describe where they ended up rather than each step.

- REST surface: downloads, shared files, search, servers, Kad, clients, known clients, friends, chats, categories, preferences, statistics, logs and IP filter actions, with SSE events for everything that moves.
- Locale-independent by design: typed values and stable keys rather than the GUI's translated strings, and English error text (#293, #311, #380).
- Media metadata, client details, friend status and the credit modifier are served from the same core values the desktop shows (#436, #442, #443, #1479, #1480).
- Web UI features beyond the section list: a download button that pulls a finished file straight to the browser (#1216, #1203), per-peer detail panels, a shared-files tree, category management, version alerts, country flags, and a comments tab driven by SSE rather than polling (#731).
- Security: amuleapi receives an ephemeral EC token instead of the stored password (#749, #751), and config files are restricted to 0600 (#746).

### Performance and memory

- **EC incremental-update cache.** Each connection kept the last value it sent for every field of every object, and entries for clients, servers and friends that went away were never released - unbounded growth for the life of the connection, which is what long-running daemons were seeing. Entries are now dropped where the object is removed (#1424).
- **Startup share scan.** `AddPathToShares()` asked the filesystem four separate questions per file; on 10,000 shared files almost three quarters of the walk was syscalls that did not need to happen. Now one `stat` (#1310).
- **EC update path.** The file list is no longer rebuilt on every `GET_UPDATE` poll (#775), encoders live in a sorted vector rather than a `std::map` (#776), reconciliation is skipped when neither side changed (#777), a client's tags are built only for fields that moved (#772), and unchanged search results are skipped on the multi-search union poll (#723). AES is preferred for the EC channel where the hardware backs it (#798).
- **GUI.** Lists nobody is looking at are not re-sorted (#878), off-screen rows skip change notifications (#885), the Clients page refreshes once a second instead of ten times (#922), and the remote GUI polls its active page about once a second instead of every three (#773), with remote log delivery batched (#445).
- **amuleapi.** The file map is diffed in place instead of being copied twice a tick (#1098, #1092), JSON accumulates as UTF-8 rather than UTF-32 (#1091), the SSE diff is skipped when nothing is listening (#1102), the known-clients store is maintained rather than re-read (#912), and glibc's malloc arenas are capped so a request's peak is not retained per pool thread (#1095).
- **Hashing.** The one-second network-quiescent guard is gone, so per-part verification keeps up in real time on fast links (#791).

### Transfers, sources and performance

- Endgame mode for the last blocks of a file (#225).
- BDP-adaptive request and upload depth, and the dead VBT code path removed (#598).
- Kad source-search and connection limits raised and exposed in Preferences (#308).
- Complete-source counts recomputed when uploaders leave, not only when they arrive (#1052, #1066).
- Runtime-toggleable memory-mapped file I/O (#565).
- AICH recovery for parts found corrupt during the completion re-hash (#270), and the post-hashing sync no longer prunes a file it just hashed (#526).
- A completed file deleted from disk can be downloaded again (#815); a partfile whose handle has been closed is no longer read (#954).
- Throttled reads that stalled browsing and chat now resume (#1276).

### Networking and protocol

- Interface binding for every socket aMule opens (#281).
- Hostnames resolve to IPv4 only, so a AAAA answer cannot strand a connection (#699).
- Connections arriving during startup are no longer dropped (#926).
- Servers are not marked dead when the fault is at our end, with back-off between sweeps (#893).
- UPnP: search only for the internet gateway, rate-limit ALIVE-triggered fetches (#310), raise the content-length limit for large router descriptions (#469), and classify IGD devices regardless of UPnP version (#472).
- SOCKS5 replies and relayed datagrams hardened (#1433).
- Two long-standing Asio races in the read handoff, each capable of wedging an amulegui / amuled pair until the watchdog fired (#871, #882).
- eMuleAI peer capabilities are recognised and its reserved frames demultiplexed (#1288).

**Groundwork, not switched on.** A large block of addressing work landed this cycle: an internal address type that can hold IPv6, typed peer identity, address-aware security and filtering, and a uTP transport (#1318 through #1504). A stock 3.1.0 build behaves exactly as before - uTP sits behind `ENABLE_UTP`, off by default, as do the experimental Kad switches (#1297, #1304) - and the work is here so the next cycle has somewhere to stand.

### External Connect and security

- Negotiated authenticated encryption, with forward secrecy from an ephemeral X25519 exchange (#714, #785).
- Authentication throttling, configurable (#741, #742).
- Loopback-only listening by default, with detected addresses offered in Preferences (#737).
- One credential store shared by amule, amuled and amuleapi (#665); config files restricted to 0600 (#746).
- Remote GUIs can now manage the core's shared folders (#530), browse a peer's shares (#520), follow chat (#517), and poll every search's progress in one request (#758).

### GUI

- List controls migrated to `wxDataViewCtrl` with live sorting, keyboard context menus, and lists that no longer paint blank under an open menu or lose the user's place (#540, #860, #877).
- Statistics and Kad graphs rewritten: new renderer, hover readout, and the history behind them fixed, which on a remote GUI had been nearly empty (#897).
- A browsed peer's shared files are shown as a folder tree instead of one flat run (#917).
- File details for shared files (#495), on double-click for downloading files (#628), and an export of the selection to an emulecollection (#769).
- Free disk space and total size on the Downloads and Shared Files panels (#847, #689, #929).
- Open a file or show it in the file manager from both lists (#831), and `.emulecollection` files open from the OS file manager (#647).
- Alt+letter tab shortcuts mirrored as a macOS menu (#642), server country flags (#691), per-user server file limits (#841), "Connected since" in the ED2K and Kad panes (#650).
- Byte sizes and rates are labelled in IEC units that match the arithmetic (#931), timestamps follow the user's locale (#928), and the app icon is rendered from one vector source at every size (#932).
- Verify Local Data is available from the GUI, amuled and amulegui, with progress.
- amulegui: remote-to-local path mappings (#854), reconnects instead of exiting when the link drops (#452), quietly while minimised (#817), keeps its own log apart from the daemon's (#508), and shows the connected core's version (#1224).

### Search

- History (#643) and results (#856) persist across restarts; 100 past queries instead of 30 (#822).
- Filter results as you type (#821); rating in its own sortable column (#632).
- Kad notes and community ratings retrieved and shown, with eMule parity (#462, #500).
- Searches started by other clients appear in the monolithic GUI (#710); a new eD2k search asks before stopping the running one (#1046).
- Result grouping and download-under-name over EC and REST (#447).

### Shared files and metadata

- Media metadata advertised over ed2k and Kad (#280), re-extractable from the context menu (#1124, #1106).
- Exclude shared files by name pattern (#350).
- Live upload activity per shared file over EC (#473).
- Shared-folder path index fixes: re-keyed on completion (#309), NFC-normalized on macOS (#312), cleared with the map it mirrors (#1034).
- The directory watcher handles rename and delete correctly and notifies the GUI when it re-shares a known file (#289, #313).

### Preferences and defaults

- IP filter defaults to emule-security's list (#577) with auto-update off (#582).
- Free-disk-space floor raised to 500 MB (#578).
- A GUI control for sparse part files (#662).
- Media-probe and previously hidden daemon preferences wired over EC and exposed in amuleapi (#479).
- External connections are required for the web server and amuleapi, instead of failing silently later.

### Platform and packaging

- Fully static musl build of `amuled`, `amulecmd` and `amuleapi` (#629).
- libupnp 22.0.4 everywhere, source-built on Windows where MSYS2 has no 22.x (#681).
- `ed2k://` and `magnet:` scheme registration on Windows, macOS and Linux (#324).
- AppImage: amulegui launcher and shell shortcuts (#328), host GLib and GTK modules kept out of the bundle (#903), sanitized child environment for external programs (#336).
- Windows: dark mode (#818), correct icon sizes for scaled displays (#933, #934), prevent-sleep under MinGW (#416).
- macOS: daemonize before `wxEntry` so `amuled -f` no longer aborts (#297), UTF-8 file names (#318), and `LC_NUMERIC` kept at C so a terminal launch does not crash (#1430).

### Known Limitations

Carried over from 3.0.1, unchanged this cycle:

- macOS `.dmg` is **not yet code-signed or notarized** - Gatekeeper warnings on first launch.
- **Flathub submission still pending** - install the `.flatpak` directly with `flatpak install <file>` for now.
- AppImage isn't yet in AppImageLauncher's catalogue.

### Contributors

This release is 893 merged PRs. Particular thanks to:

- **got3nks** - 580 PRs. amuleapi and its REST surface, EC encryption and forward secrecy, the transfer pipeline, media metadata, search persistence, chat and friends parity, the Clients page, packaging and the release tooling.
- **ngosang** (Diego Heras) - 118 PRs. The Web UI: its shape, its features and most of its polish (#220), plus the Spanish catalogue, the redesigned toolbar icons and the API reviews that drove much of the v1 cleanup.
- **kno** - 40 PRs. The addressing and uTP groundwork: the internal address type, typed peer identity, address-aware security and filtering, and the uTP transport.
- **LSalami** (Lorenzo Salami) - 35 PRs. The `wxDataViewCtrl` list migration - downloads, shared files, client lists - plus remote-to-local path mappings, search-result persistence and accessibility fixes.
- **mrjimenez** (Marcelo Roberto Jimenez) - 14 PRs across the build system, compile tooling, deprecation warnings and the README distro table.
- **danim7** - 9 PRs, including the Crypto++ migration for SHA1 and MD5 (#238) and Verify Local Data across the GUI, daemon and remote GUI.
- **Cflsft** - 5 PRs, including endgame mode (#225).
- **adem4ik** (Andrei Stepanov) - 5 PRs on icons and the Russian `.desktop` localization.
- **Zeyckler** (Carlos Barrero) - 3 PRs, including the hi-DPI SVG toolbar and preferences icons (#487).
- **tbo47** (Teebo) - 3 PRs, including the first-run setup wizard (#264).
- **gbcox** (Gerald Cox) - 3 PRs.
- **ZiTAL**, **ed29**, **frnjjq**, **secretmango** - one PR each.

#### Testing and bug reports

A release this size is only as good as the people who use it before it ships and say what broke.

- **ghysler** - 47 issues filed this cycle, 44 already fixed: remote-GUI performance on large queues, the search lifecycle end to end, and a long run of interface details that would otherwise have shipped. Much of the amulegui and search polish in this release exists because of those reports.
- **mifritscher2** (9), **slrslr** (5), **Send8213** (4), **irmu** (4), **quimm2003** (3), **falcogiallo** (3) - issues filed and, in almost every case, closed this cycle.
- Thanks also to everyone who reported a single bug, confirmed a fix, or tried a pre-release build.

#### Translators

Translation flows through [Weblate](https://hosted.weblate.org/projects/amule/). Credited on this cycle's translation commits, with the largest contributions first:

**Coool**, **தமிழ்நேரம்**, **mslr8**, **Watchman89**, **Michal Várady**, **Francisco José Juan Quintanilla**, **Kisnov**, **reducedradius**, **Guzleon**, **Marcell Balla**, **Samuel García Del Campo**, **Jaume Casado Ruiz**, **Alec Bourenkov**, **Nioh**, **Libre**, **Артур**, **nathanael**, **lazizkhalilov**, **alexandre.detourne**, **Steven Liu**, and the contributors who translate anonymously.

Several developers credited above also carried catalogues: Diego Heras, Marcelo Roberto Jimenez, Andrei Stepanov, Lorenzo Salami and danim7.

### Merged PRs in this release

At 893 PRs a numbered index stopped being useful, so this release links the record instead of reprinting it: [the full compare view](https://github.com/amule-org/amule/compare/3.0.1...3.1.0) lists every merged pull request between 3.0.1 and this tag.

Fixes to features added during this same cycle - most of the amuleapi and Web UI churn - are folded into the entries above rather than listed as separate bug fixes, since no released aMule ever carried those bugs.

---

## Version 3.0.1 — "bugfixes and polish"
2026-06-24

First point release after the 3.0.0 rebirth, and the first cut entirely on the new `amule-org/amule` repository. 147 merged PRs of post-launch work: a new IP2Country preferences panel, remote-GUI (amulegui) improvements over EC, smarter search, AICH request rate-limiting, a top-to-bottom WebUI / WebServer modernization and security pass, a Weblate-driven translation pipeline, and a one-click release process. No on-disk format or config changes — a drop-in upgrade from 3.0.0.

### Highlights

- **IP2Country preferences panel.** A new Preferences page to choose the GeoIP source — DB-IP, MaxMind, or a custom URL — instead of editing config by hand (#113). IP2Country is now enabled by default and hard-fails at configure if the dependency is missing (#119).
- **Remote GUI over EC.** amulegui now populates the Server Info tab from amuled (#120), keeps statistics and Kad graphs alive across tab switches (#135), and stops offering preferences it can't actually push to the daemon (#122).
- **Smarter search.** Modern file formats and corrected extension categories in the type filter (#189), new Length / Bitrate / Codec columns in results (#139), a working Related Files lookup (#154), and global search that works even with Kad disabled (#38).
- **AICH anti-abuse + speed.** `OP_AICHREQUEST` is now rate-limited via `CheckForAggressive()`, and `LoadHashSet` gained an O(1) root-hash → offset cache with a self-healing linear-scan fallback, so AICH recovery on large `known2` files no longer rescans the whole file per request (#186, #191, #193).
- **WebUI / WebServer reborn.** The default web template was dragged out of its 2005 Dreamweaver origins: HTML5 doctype, presentational markup migrated to `style.css`, the `maquette.dwt` template and dead `MM_*` rollover JavaScript removed, favicon refreshed, and the whole thing re-encoded as UTF-8. On top of the cleanup came real features — select-all checkboxes, a working priority selector on the Shared page, download/delete confirmation with file counts, auto-refresh that pauses while rows are selected, button tooltips, and unified styling and size/speed units.
- **WebUI security hardening.** XSS defences (`htmlspecialchars` + `addslashes`, sort-param whitelisting), CWebSocket request-parser hardening and memory-leak fixes, and a sweep of the embedded PHP interpreter (`php_core_lib`, `php_amule_lib`, `php_syntree`, `php_native_split`) fixing memory frees, base16/base64 handling, and long-standing parser bugs.
- **Translations via Weblate.** Translation now flows through a hosted Weblate instance feeding the repo, with a `--no-wrap` catalog policy and build-time po4a manpage rendering; sixteen catalog-update rounds landed this cycle.
- **One-click releases.** Maintainers can cut a release from the GitHub Actions UI — pick a version, and the workflow tags, builds every platform, verifies the artifact set, and assembles a draft Release (#234).

### IP2Country

- New Preferences panel to select the GeoIP source: DB-IP, MaxMind, or a custom URL (#113).
- Enabled by default; configure hard-fails when the dependency is missing rather than silently disabling (#119).

### Remote GUI & Platform

- amulegui populates the Server Info tab from amuled over EC (#120) and stops offering preferences it can't push to the daemon (#122).
- Statistics + Kad graphs stay populated across tab switches (#135); the Kad status bar refreshes after `StopKad()` (#133).
- Theme-aware colours: list-control text (#179) and statistics labels (#128) no longer hardcode `*wxBLACK/BLUE/RED`, fixing legibility on dark themes.
- Window-state persistence: Messages columns + minimized-window geometry saved (#138); macOS persists list-control settings on Dock → Quit (#141).
- macOS Preferences left-rail tab icons padded (#130).
- `wxWebSession`-cleanup `_Exit` workaround applied on all platforms to avoid a shutdown crash (#159).

### Search & Networking

- Global search works with Kad disabled (#38); Windows EC search fixed by using `wxUIntPtr` for `m_currentSearch` (#39).
- File-type filter: miscategorised extensions corrected and modern formats added (#189).
- Result columns: Length, Bitrate, Codec added (#139); Directories column moved to the end (#142).
- Related Files lookup fixed; TCP/UDP server flags shown in Release builds too (#154).
- EC search lifecycle: the prior Kad search is stopped when the EC client starts a new one (#36); late ed2k server replies dropped after the client switches to Kad (#37).
- `SO_REUSEADDR` on the UDP socket so it rebinds cleanly after suspend/resume (#121).
- Server Info log cleared at the transition source on ed2k disconnect / server switch (#163, #164).
- UPnP ASCII `tolower()` scoped under `LC_CTYPE=C` so non-C locales don't corrupt device matching (#208).
- Full path logged when an ed2k link collides with an existing file (#172).

### AICH & Hashing

- `OP_AICHREQUEST` rate-limited via `CheckForAggressive()` and `LoadHashSet` made O(1) through a root-hash → offset cache (#186).
- True linear-scan fallback when the cached offset is stale (#191); the cache self-heals on a successful rewind (#193).
- UDP file-info handler short-circuits when the requesting client is banned (#192).

### WebUI & WebServer

The biggest theme of the release by volume — driven mainly by ngosang, with the embedded-PHP work by RealGreenDragon.

Template modernization:

- HTML5 doctype adopted; layout glitches from the switch corrected (#110, #204).
- Presentational HTML attributes migrated to semantic CSS — text/element attrs (#95), table/iframe attrs (#102), inline `<style>` blocks consolidated into `style.css` (#89), JS nav rollovers replaced with CSS `:hover` (#92).
- Dead Dreamweaver scaffolding removed: the `maquette.dwt` template (#88), `MM_*` JavaScript extracted then deleted once shown to be no-ops (#90, #93), stale `onLoad` tree calls in `stats_tree.php` (#111), and small deprecated CSS/HTML leftovers (#105).
- `<script>` blocks moved into `<head>` (#84); structural HTML errors corrected (#87); stray `style.css` brace removed and formatting normalized (#78); shared-files page re-encoded as UTF-8 with English alt-text (#80); higher-resolution transparent favicon (#112).

New WebUI features:

- Priority selector wired up on the Shared page (#210).
- Select-all checkbox on file tables (#212); auto-refresh pauses while checkboxes are selected (#213) and avoids the form-resubmission prompt (#230).
- Download/delete actions confirm with a file count (#214).
- Search-page controls improved (#215); button tooltips and clickable images (#216).
- Unified button styles (#217), form controls (#206), and file size + speed units (#218); footer-bar background (#219); logo links home to Transfers (#209); clearer dropdown option labels (#207); footer "Download link" refreshes the main page (#81).

WebUI bug fixes:

- Sorting downloads by sources never worked — fixed (#75).
- Wrong MByte/GByte multipliers in the search size filters corrected (#76).
- Division-by-zero guard on the downloads-page percentages (#83).
- Login error message now actually shown, and made readable (#82, #96).
- Static `.css`/`.js` served to unauthenticated clients so the login page styles correctly (#97).
- amuleweb honours the per-user template dir when spawned by the GUI (#72); copy-pasted preferences code removed from the log page (#74).
- ed2k links can be added via the footer links form (#145); search results cleared across cycles and on new search (#33).

Embedded PHP interpreter:

- Memory freed correctly when casting array values (#108) and for `for()` header expression lists (#109).
- `OtherFunctions` base16 decode, base64 footer append, duplicate `.asf` entry, and mp3 comment handling corrected (#30).
- Interpreter sweep: `php_native_split` (#146), `php_core_lib` (#150), `php_amule_lib_standalone` (#151), `php_amule_lib` (#152), `php_syntree` (#153), with follow-ups (#155, #156).

### WebUI Security

- XSS hardening: `htmlspecialchars` + `addslashes` builtins added and remote-controlled strings escaped (#106); the search `sort` param whitelisted before being stored in the session (#107); broad WebUI/WebServer backend hardening (#91).
- CWebSocket request parsing hardened for header casing, body bounds, cookie conversion, and MinGW portability (#27, #28); CWebSocket memory leaks fixed and review findings addressed (#29).

### Translations & i18n

- Translation moved to a hosted **Weblate** workflow feeding the repo; the README points contributors there (#71). Catalog rounds: #57, #66, #100, #123, #147, #160, #165, #170, #184, #187, #199, #200, #202, #211, #229, #235; Brazilian Portuguese also refreshed by hand (#7), and the IP2Country strings translated into Italian (#129).
- `--no-wrap` policy adopted for the app catalogs (#61) and extended to the manpage `.po` files (#67).
- Non-ASCII output fixes: the logger writes stdout/stderr via `utf8_str` (#42); locale is initialised so non-ASCII strings stop mangling under the default C locale (#205, fixes #203); a real newline is used in a comment-dialog tooltip (#70).
- Windows `SPANISHINTERNATIONAL` routed to `po/es.po` (#64).

### Build System & CI

- **One-click release workflow** from the Actions UI: tag → full matrix → artifact-manifest gate → draft Release (#234). Repo bootstrap: issue/PR templates (#12). The `manpages-sync` CI check disabled where Weblate PRs broke it (#68).
- `cryptopp-modern` accepted by the build, with packager-friendly flags (#169); versioned `libatomic.so.1` accepted in `find_library` (#176).
- `BUILD_TESTING` off by default (#49); options.cmake typo/casing cleanup (#50); a `pofiles` dependency added to every NLS-aware executable (#98).
- LLP64 portability: unsafe uses of `long` cleaned up (#65).
- Toolbar ID collision with manually-added IDs resolved (#52); `SearchDlg` `FALSE` macro replaced with `false` (#48); `IDC_SPINSEARCHAVAILABILITY` spelling corrected (#51).

### Shared Files & Preferences

- `SharedDirWatcher`: the inotify race window on newly-created subdirectories closed (#227), with a follow-up so file CREATEs reliably reach `NotifyPathAdded` (#228).
- First-run no longer wipes `shareddir-*.dat` during userhash init (#79) or promotes stale `shareddir.dat` entries to explicit shares (#86).
- "Show Clients for" defaults to "All files" (#126).
- English language-picker IDs corrected, with an explicit U.S. entry (#60).

### Manpages & Docs

- Translated manpages now rendered at build time with po4a (#94), with `--no-update` so the build doesn't dirty the source tree (#101).
- README: a Translations section (#71), an amule-org column in the Development Statistics table (#62), and NetBSD added (#32).

### Packaging & Flathub

- Icons refreshed from a new 256×256 master (#15), then reverted to the established design upscaled to meet Flathub's 256px minimum (#21).
- Flathub-strict manifest staged under `packaging/flathub/` (#16), with metainfo + desktop polish for listing readiness (#17) and commit-pin bumps as the manifest tracked master (#23, #24, #26).
- Flatpak fixes: libbfd disabled in the sandboxed build (#19); the install-prefix locale dir registered for wxLocale lookup (#22); `.mo` catalogs shipped inside the main app rather than a separate `.Locale` extension (#25); all bundled languages surfaced in the Preferences Language picker on Flatpak (#175).
- macOS: webserver templates bundled into `aMule.app` so the in-app WebUI resolves them (#195).

### Known Limitations

Carried over from 3.0.0:

- macOS `.dmg` is **not yet code-signed or notarized** — Gatekeeper warnings on first launch.
- **Flathub submission still pending** — install the `.flatpak` directly with `flatpak install <file>` for now.
- AppImage isn't yet in AppImageLauncher's catalogue.

### Contributors

This release is 147 merged PRs cut entirely on the new `amule-org/amule` repository. Particular thanks to:

- **got3nks** — 59 PRs across search, amulegui / EC, AICH, IP2Country, packaging, locale, and the release tooling.
- **ngosang** — 46 PRs; the WebUI / WebServer modernization, hardening, and UX features are almost entirely his.
- **Marcelo Jimenez** (mrjimenez) — 9 PRs across the webserver, build system, and translations.
- **RealGreenDragon** — 8 PRs fixing the embedded PHP interpreter.
- **danim7** — 2 PRs (global-search-without-Kad, Related Files + server flags).
- **peyoker** — WebUI / WebServer security hardening (#91).
- **cardpuncher** — NetBSD in the README (#32).

### Merged PRs in this release

Complete list of PRs that landed for 3.0.1, in numerical order:

#7, #12, #15, #16, #17, #19, #21, #22, #23, #24, #25, #26, #27, #28, #29, #30, #32, #33, #36, #37, #38, #39, #42, #43, #44, #48, #49, #50, #51, #52, #57, #60, #61, #62, #64, #65, #66, #67, #68, #70, #71, #72, #74, #75, #76, #78, #79, #80, #81, #82, #83, #84, #86, #87, #88, #89, #90, #91, #92, #93, #94, #95, #96, #97, #98, #100, #101, #102, #105, #106, #107, #108, #109, #110, #111, #112, #113, #119, #120, #121, #122, #123, #126, #128, #129, #130, #133, #135, #138, #139, #141, #142, #145, #146, #147, #150, #151, #152, #153, #154, #155, #156, #159, #160, #163, #164, #165, #169, #170, #172, #175, #176, #179, #182, #184, #186, #187, #189, #191, #192, #193, #195, #199, #200, #202, #204, #205, #206, #207, #208, #209, #210, #211, #212, #213, #214, #215, #216, #217, #218, #219, #227, #228, #229, #230, #234, #235.

---

## Version 3.0.0 — "alive again"
2026-06-08

First major release in 5+ years (since 2.3.3, 2021-02-07). Headline changes are dramatic throughput improvements, full build-system overhaul, modernized dependency stack, native binaries for Linux / macOS / Windows, and a broad legacy-API cleanup.

### Highlights

- **Throughput rewrite.** Disk I/O moved off the main thread, ASIO/EPOLLET races fixed, throttlers replaced with proper token-bucket limiters. Peer-to-peer download on the same hardware sees **~100–380× speedups** across macOS / Linux / Windows over 2.3.3, plus aMule 3.0.0 sustains **~4.8×** the upload throughput of eMule 0.70b on Windows. See [Performance](#performance) for the full matrix and per-PR breakdown.
- Both throttlers (`MaxUpload`, `MaxDownload`) were also broken pre-fix — `MaxUpload=0` capped at "current rate + 5 KB/s", `MaxDownload` was a ratio controller rather than a literal cap. Both rewritten (#461, #491). Important user-facing bug fixes, but secondary to the headline numbers.
- **Big-library / big-shareset scaling.** Follow-up wave targeting nodes with 100 k+ shared files: per-file EC payload caches (#725, #736), skip-unchanged EC updates (#727), local-peer ZLIB bypass (#728), and a string of O(N²) → O(N log N) / O(1) algorithmic fixes across `SharedFileList`, `SharedFilesCtrl`, `KnownFileList`, `wxListCtrl`, `ExternalConn`, and amuleweb — the WebUI / amulegui stay responsive even on libraries where the previous GUI took minutes to redraw.
- **CMake replaces autotools.** Single build system, modern toolchain — minimum CMake 3.10, minimum wxWidgets 3.2.0.
- **Native binaries for every major desktop.** AppImage (x86_64 + aarch64), Flatpak (x86_64 + aarch64), macOS Universal2 .dmg (now bundling `aMuleGUI.app` alongside `aMule.app`, #529), Windows portable .zip **and NSIS installer** (x64 + ARM64, #740). First-run desktop integration prompt for AppImage; cross-platform autostart-on-login toggle (#744).
- **Auto-rescan of shared folders.** `wxFileSystemWatcher`-driven (#591), with recursive vs explicit-share intent split (#606) and coverage of Incoming + per-category Incoming dirs (#743).
- **HTTPS works again.** `CHTTPDownloadThread` rewritten on top of `wxWebRequest`; the hand-rolled stack had silently stopped working against modern TLS.
- **Kad parallel searches** with alpha-frontier widening.
- **MaxMindDB replaces deprecated GeoIP** for IP→country.

### Performance

Throughput work landed across April 2026. The bottom-line cross-platform
numbers first, then the per-PR contributions that produced them.

#### Cross-platform end-to-end (2.3.3 vs 3.0.0)

| Leecher platform | 2.3.3 (sustained) | 3.0.0 (sustained) | Speedup |
|---|---:|---:|---:|
| macOS (Apple Silicon, Mac Studio) | 0.35 MB/s | **135 MB/s** | **381×** |
| Linux ARM (UTM VM, Ubuntu 25.10) | 0.34 MB/s | **117 MB/s** | **345×** |
| Windows ARM (UTM VM, Windows 11) | 0.36 MB/s | **39 MB/s** | **107×** |

Sustained over a 90 s window, single LAN peer downloading a 30 GB file
from an x86_64 Linux seeder running the same aMule version under test.

Per-platform breakdown of the seeder-side vs leecher-side contributions:

| Platform | 2.3.3 | + seeder fix | + leecher fix (3.0.0) |
|---|---:|---:|---:|
| macOS | 0.35 MB/s | 30 MB/s | **135 MB/s** |
| Linux | 0.34 MB/s | 20 MB/s | **117 MB/s** |
| Windows | 0.36 MB/s | 6.8 MB/s | **39 MB/s** |

The seeder-side contribution (PR #451 `CUploadDiskIOThread` + ASIO fixes)
dominates everywhere; the leecher-side contribution (#454 partfile-write
offload + #484 throttler-wake fix + #491 token-bucket cap) stacks another
~4–6× on top.

#### vs eMule 0.70b (Windows · same UTM hardware)

| Direction | eMule 0.70b | aMule 3.0.0 | Speedup |
|---|---:|---:|---:|
| **Upload** (Windows seeds → Mac leecher) | 22 MB/s | **106 MB/s** | **~4.8×** |
| **Download** (Linux seeds → Windows leeches) | 20 MB/s | **39 MB/s** | **~1.9×** |

#### Upload

- **#451 — eMule `CUploadDiskIOThread` port.** Disk reads + ed2k packet construction off the main thread, RC4 stream-desync race fixed in the ASIO layer, EPOLLET spurious-wakeup fix in `HandleRead`, adaptive per-slot packet sizing (10 KiB → EMBLOCKSIZE). Dominant contributor to the upload gain.
- **#461 — `MaxUpload=0` is now literal unlimited.** The legacy code set `allowedDataRate = current_rate + 5 KB/s` per iteration — a tracking cap that pinned the uplink at a fraction of capacity on any link above a few hundred KB/s.
- **#436 — Speed-limit fields widened from `uint16` to `uint32`.** Legacy 65,534 KB/s (~524 Mbps) configuration ceiling gone; gigabit-class links can now be configured.
- **#463 — Bandwidth-cap UI.** Spin button ceiling 19,375 → 1,000,000 KB/s; field width 100 → 140 px so the `+` button no longer clips off on modern themes. Slot Allocation cap 100 → 100,000 KB/s.
- **#898 — Default `SlotAllocation` raised from 2 kB/s to 10 kB/s.** The previous default (set in 2005) fragmented uploads into so many sub-slot slices that fast peers were rate-shaped down to a trickle.

#### Download

- **#454 — `CPartFileWriteThread`.** Disk writes for downloads moved off the main thread.
- **#484 — Throttler-wake fix.** `UploadBandwidthThrottler::Entry()` adaptive backoff dozed for 5–25 ms between control-queue pumps, stalling the request→response loop. Empty→non-empty wake gate added on the control-queue path, plus IOCP-native `async_read_some` on Windows, `wxMutex` → `std::mutex`, and decoupling `m_MaxBlockRequests` from per-packet block count. The biggest win on Windows specifically.
- **#491 — `MaxDownload` is now a literal byte/sec cap.** The legacy code was a closed-loop ratio controller that nudged each peer's rate ±5%/tick relative to its own current speed, so `MaxDownload=20000` (20 MB/s) shaped traffic toward whatever the aggregate converged on — generally well below configured. Replaced with a global token bucket (`CDownloadBandwidthThrottler`) enforcing the cap to within 2.5% across platforms.
- **#498 — Per-part hash verification deferred to a dedicated worker thread** (only runs when the file isn't actively transferring). Follow-ups #499, #500.

#### Other

- **#467 — ASIO handler binding modernised.** `boost::bind` → C++11 lambda, `strand.wrap()` → `bind_executor()`, `deadline_timer` → `steady_timer`, `null_buffers` → `async_wait(wait_read)`. Fixes the long-standing `boost::bind` placeholder warning. Minimum Boost bumped 1.47 → 1.70.
- **#504 — Fast-path bare-filename comparison** skips `wxGetCwd()`.

#### Big-library / big-shareset scaling

A follow-up wave targeting nodes with 100 k+ shared files, where the daemon ↔ GUI / WebUI / amuleweb traffic and several internal data structures were quadratic in the shareset size:

- **EC payload caches.** Per-file `ed2k://` link and partmet basename cached on `CKnownFile` / `CPartFile` so EC enumeration stops rebuilding them on every tick (#725). Per-file byte cache covers the FULL `GET_SHARED_FILES` / `GET_DLOAD_QUEUE` paths so wire-marshalling cost drops by orders of magnitude on huge sharesets (#736).
- **EC skip-unchanged.** Per-file change tracking infrastructure threaded through every field-change site so `INC_UPDATE` only re-sends what actually moved, with a backward-compatible partial-update protocol opt-in for amuleweb (#727 — landed as a 6-commit series). `MarkECChanged` gaps closed for paused / user-action setters.
- **Local-peer EC bypass.** ZLIB compression skipped on local peers (#728), receiver limit raised to 256 MB, and the locality decision later moved to the client side with a user override checkbox / `--force-zlib` flag (#840 — handles the WireGuard-as-LAN case).
- **O(N²) → O(N log N) / O(1) algorithmic fixes.**
  - `SharedFilesCtrl` bulk-update API skips per-row repaints during `ClearED2KPublishInfo` (#561, #2bf9b8f34).
  - `SharedFileList` keyword indexing in `Reload` (#566).
  - `known2.met` AICH `SaveHashSet` dedup made O(1) (#581).
  - `known.met` hash-collision dedup on Init dropped to O(N log N) (#584).
  - `wxListCtrl::FindItem(data)` O(N) → O(1) via user-data hash map (#614).
  - `ExternalConn` file-list enumeration snapshotted to avoid O(N²) `GetFileByIndex` loops (#687).
  - amuleweb `array_push_back` made O(N) via cached scan hint (#689).
  - `KnownFileList` indexes its size-map by `(size, mtime)` instead of size alone to cut false-positive collision-dedup walks (#593).
- **Disk I/O.** `CFile` gains a 64 KB userspace write buffer to coalesce metadata-save syscalls (#573); the buffer was subsequently moved off the stack onto the heap so it survives musl's 128 KiB pthread stack, and an internal recursive_mutex was added to serialise buffer + fd access against the concurrent-corruption race surfaced on Alpine (#576). `SavePartFile` switched to atomic-rename (#670) and gated on an in-memory dirty flag so periodic ticks only touch disk when something actually changed (#671). Upload-stats persistence on 10-min heartbeat when only stats moved (#e8363bf42).
- **Memory bounds.** `amuleIPV4Address::operator=` no longer leaks the endpoint copy on every assign (#716) — was 7 MB / 514 k objects per ~10 h of runtime on a busy node. `CKeyEntry::m_filenames` capped so the list can't grow unbounded (#652). `known.met` growth bounded by per-hash cap + 30-day TTL (#598, #f33c56b44). `KnownFileList::Append` only stamps `lastSeen=now` on actual sightings (#fafbb8a66). Wake-from-sleep SIGSEGV in asio socket impls fixed via `shared_from_this` (#596).
- **Backpressure & yielding.** `ECSocket::OnOutput` yields when asio backpressures `Write` to zero bytes (#672). `CECServerSocket` notification-dispatch recursion capped so EC bursts no longer stall the event loop (#667). `HTTPDownload` bounds libcurl connect-phase via `GetNativeHandle()` (#565). Startup HTTP downloads deferred until after partfile + shared-file scan (#719) so cold-start UI is responsive while ipfilter.dat and friends are fetched.
- **Recursive share walks** deferred + cancellable, with confirmation prompts for sensitive home-parent paths (#594, #599).
- **macOS daemon shutdown** driven from `OnCoreTimer` to fix the long-standing hang (#563).

### Networking & Discovery

- **Kad parallel searches** with alpha-frontier widening; new "More" button to ask additional peers for results (#505).
- **MaxMindDB replaces deprecated GeoIP** for IP→country (.dat → .mmdb; free MaxMind account required for the database) (#502, #507, #509).
- HTTPS downloads work again — `CHTTPDownloadThread` rewritten on top of `wxWebRequest` (#462), with a hard-fail when `wxUSE_WEBREQUEST` is off (#481).
- Wire-parser hardening: `OP_SERVERMESSAGE` size validation (#447), buffer overflow fix in `ECSocket` (#421), uninitialized memory fix in `UInt128` (#424). EC tags whose declared length underflows the children size now rejected (#879). `Proxy::SendTo` SOCKS5 UDP relay bounded against oversized payloads (#882). Webserver/PHP `IDENT` strcpy into `YYSTYPE.str_val` bounded (#890). Memory leaks on malformed-packet exception paths plugged (#886, fixes #884 / #885).
- ed2k-link float tags endian-swapped correctly on big-endian hosts (#368).
- Friend's shared list and search-result rating exposed via EC (#430, #452).
- **Shared-folder watcher.** `wxFileSystemWatcher`-driven auto-rescan (#591), with recursive vs explicit-share intent split + cold-discover of subdirs created while offline (#606). Watcher also covers the Incoming dir + per-category Incoming dirs so completed downloads register without a manual reload (#743). Incremental rescan replaces full `Reload` per filesystem event (#751); `SharedFileList` AICH-hash scan dropped from O(N·M) to O(N+M) (#746); optional "Follow symbolic links in shared folders" preference (#809). macOS amuled now pumps `CFRunLoop` so FSEvents deliver (#975f60fb0). File rename / move-out / delete during in-flight hash handled cleanly (#858).
- **Kad index hygiene.** `CEntry::SetFileName` rejects empty filenames in the merge path (#675); `CKeyEntry::m_filenames` bounded (#652); `known2_64.met` orphan-pruned at sync startup (#605). `m_filenames` rotation now uses O(N) compare-and-skip insertion instead of sort+resize (#795); a popularity-decay experiment (#799) was introduced and then reverted (#805), keeping the `GetCommonFileName` all-zero robustness fix. Kad late UDP firewall responses dropped; Recheck assert downgraded (#afb9da010). SearchID collisions between Kad and ed2k searches no longer mix tab results (#530, #3008ada0f).
- **Server protocol.** Future ping timestamps discarded from server.met (#3f70856a0); several server-ping fixes (#715). Control-traffic bypasses the download throttler (#615); inbound server probes also bypass it (#787). Server-name updates parsed as Unicode without the obsolete `SRV_TCPFLG_UNICODE` gate (#835); explicit disconnect when the server assigns no client ID (#788). `OP_IDCHANGE` sanity checks demoted to debug log (#613). `ServerUDPSocket` "additional packet" notice demoted to non-critical debug (#548). HTTP/HTTPS auto-update for `addresses.dat` accepts https URLs (#721); default eMule-security + shortypower URLs upgraded to HTTPS (#718).
- **EC / WebUI / amuleweb.** 65535-children-per-tag wire-format ceiling lifted (#570). `FRIEND_REMOVE` made idempotent (#632); friend-slot flag persisted on `CFriend` so it survives disconnects (#633). amuleweb honours `amule_set_options()` nickname (#634); fixes POST body parsing when URL has a query string (#729); fixes `split()` byte/wide-char confusion on UTF-8 input (#619); fixes SIGSEGV rendering y-axis labels above 9999 (#618); fixes crash with >65535 shared files (#700). WebUI adds Kad + ed2k connect/disconnect controls (#660), Kad nodes-URL control, "Disconnect from current ed2k server" (#a82cd2e1a, #b5d83829a), and persistent ED2K / Kad enable checkboxes (#661, #8ce30f910); sends max upload/download rate as uint32 (#645). amulegui surfaces `EC_OP_FAILED` from `ADD_LINK` (#557), implements category-tab Stop/Pause/Resume/Cancel + Prio (#701), renders statistics graphs via `EC_OP_GET_STATSGRAPHS` (#639), and overrides `OnFatalException` with libbfd backtrace (#695). Aggregated single-popup AddLink error reporting (#577).
- **amuleweb security hardening** (ngosang triage, all in #875). XSS-safe whitelist on the `sort` query param + `htmlspecialchars` defence-in-depth (#869); `rand()`-derived session IDs replaced with a 64-bit CSPRNG token from `CryptoPP::AutoSeededRandomPool` (#870); `HttpOnly; SameSite=Strict` on the session cookie (#871); login handler refuses to consume `pass` when reachable via the pre-POST-body-merge URL (#872); read-buffer NUL-terminator allocation fix (#873); dead `SESSION_TIMEOUT_SECS` macro wired up (#874).
- **UPnP.** Filter non-WAN device announcements before fetching description (#623). Cache failed XML fetches + demote routine discovery logs from critical to debug (#653, #627).
- **ClientTCPSocket** strict "no trailing bytes" asserts demoted to debug log (#710).
- **SharedFileList** emits the honest file count in `OP_OFFERFILES` (#556) and skips empty `OP_OFFERFILES` when nothing made the LF cut (#169910998).

### Build System & Dependencies

aMule 3.0 requires **CMake ≥ 3.10** and **wxWidgets ≥ 3.2.0**. Autotools is removed entirely.

- Full CMake build replaces autotools (#449).
- Autotools, `debian/` packaging dir, and dead platform/subtool trees retired (#466).
- wxWidgets 3.x build fixes (#438, #443, #453); macOS framework / app-bundle compat (#453).
- Boost 1.87 / 1.89 build fixes (#387, #429) — modern Boost ASIO no longer breaks the build.
- CMake hard-fails when `ENABLE_X=YES` cannot be honored: UPnP (#511), NLS (#512), Boost (#513), IP2Country (#509). No more silent disable.
- CMake mins / detection: minimum 3.10 and `version.rc` path fix (#490), `MIN_WX_VERSION` 3.2 with legacy `wx.cmake` retired (#459), default build type Release (#469), broken `UPNP.cmake` workaround (#439), BFD link fixes for modern binutils (#487, #489), `find_package(Intl)` outside `YYENABLE_NLS` gate (#495).
- SVNDATE banner: stops freezing at first configure (#483); refreshes on `git reset` / commit (#493). **`SVN` macros / files renamed to `GIT` throughout the source tree (#726)** — `SVNDATE` → `GITDATE`, `__SVN__` → `__GIT__`, `config.h.cm` and Windows `version.rc.in` follow suit. No user-facing impact; reflects 5+ years of Git as the canonical SCM.
- `compile_commands.json` generated (#441); `compile.sh` and `find_uncompiled.py` build/analysis scripts (#494, #501).
- Loongarch64 architecture support (#286).
- OpenSUSE platform fixes (#440); libupnp 1.18 compatibility (#448).
- **boost::asio mandatory.** wxWidgets-sockets fallback path removed; `ENABLE_BOOST` and `DOWNLOAD_AND_BUILD_DEPS` escape hatches retired (#603).
- **CMake polish (post-#518).** `wx_NEED_NET` kept on for BUILD_WEBSERVER-only (#620) and BUILD_AMULECMD-only (#630) configurations. `libatomic` linked directly on 32-bit targets where `std::atomic<int64_t>` needs it (#648, #662) — the heuristic atomic-probe is gone. `glib-2.0` hard-fail when missing for `amule` / `amuled` / `amulegui` (#571), but the dependency itself is dropped on macOS even under wxGTK (#647). `PACKAGE` / `PACKAGE_VERSION` pre-defined for the bfd.h probe (#545).
- **license.txt** moved to project root (#730).
- **CodeQL** alerts cleared (#732, #733); CI runs CodeQL on push to master only, not on PRs (#c02d5ec04). `actions/checkout` bumped (#446 in initial pass).
- **README + docs polish.** Distro version and GitHub activity badges (#7a1b310b5); markdown / ortography sweep (#6e9a9fe5a); INSTALL.md MD012/MD060 markup warnings fixed (#31aff1499).

### Packaging

aMule 3.0 ships native binaries covering most modern desktops:

- **Linux AppImage** (x86_64, aarch64) — works across all major distros.
- **Linux Flatpak** (x86_64, aarch64) — runs in the GNOME Platform sandbox.
- **macOS Universal2 `.dmg`** — single installer containing Apple Silicon and Intel binaries (the per-arch `.app` trees are CI intermediates for the `lipo` merge job and are not published as separate downloads).
- **Windows portable `.zip`** (x64, ARM64).
- **Windows NSIS installer `.exe`** (x64, ARM64) — guided installation, alternative to the portable zip.

Recipes, manifests, and the GitHub Actions packaging matrix landed in #510. AppImage's first-run prompt installs a desktop launcher to the user's application menu (reversible, opt-in).

Other platform integration work:

- Wayland-friendly desktop integration: matching `app_id`, SNI tray icon (replaces deprecated `GtkStatusIcon`), macOS dock-restore on quit, tray opt-in (#508; fallback workaround in #474).
- macOS Carbon `FSRef` API retired in favor of `$HOME`-based paths (#468).
- macOS Dock right-click Quit triggers proper shutdown cleanup (#456).
- Windows / MinGW build support: cmake fixes, LLP64 pointer safety, ASIO on Windows (#457).
- Icons across all platforms: Linux hicolor PNG, Windows `.rc` icon, macOS `amulegui.app` (#464).
- Initial AppStream metainfo (#367).
- Localization on macOS and Windows fixed — wxLocale lookup paths repaired (#492).
- macOS spurious `wxGetCwd()` errors silenced (#480).
- Flatpak `libupnp` install-libdir pinned for pkg-config (#515).
- **macOS .dmg now bundles `aMuleGUI.app`** alongside `aMule.app` so installing the .dmg gives both the daemon-style and GUI binaries in `/Applications` (#529). Locale catalogs are bundled inside the `aMuleGUI.app` `Contents/Resources` tree so translated strings render correctly when run from the .dmg (#543).
- **Windows NSIS installer** shipped alongside the existing portable .zip (#740), with un.SecRemoveUserData no longer appearing on the install Components page (#65178406a). Installer UI now driven from `po/<lang>.po` via a `po-to-nsh.py` bridge so every supported NSIS language gets translated strings with English fallback (#899).
- **Cross-platform autostart-on-login** toggle in Preferences (#744): Linux .desktop, macOS LaunchAgent, Windows registry under one checkbox.
- All platforms ship `alc` / `alcc` / `cas` / `wxcas`; Windows additionally ships `amuleweb` (#785); macOS `.dmg` now contains `amuleweb` after build-path correction (#794).
- Release-asset filenames standardised on `<OS>-<arch>` so every package is reachable by predictable URL (#789).
- Windows non-DPI executables (`alc`, `wxcas`) get a comctl32 v6 + PerMonitorV2 manifest so they aren't bitmap-scaled on hi-DPI displays (#796); the main GUI got the same `PerMonitorV2` declaration in #780.
- Windows resource lookup paths corrected so skins and webserver templates resolve to `share/amule` (#784).

### Internals & Refactoring

- Legacy wx 2.x-era deprecated APIs removed (#470).
- 2.x-compat globals and event-table macros retired (#475).
- Dialog code migrated to `wxSizerFlags`; wxDesigner-generated code retired; GCC 15 enum-enum conversion warnings eliminated (#473).
- macOS 10.10-era deprecation cleanup (#470).
- `OnAssertFailure` ternary fixed for strict GCC / Alpine builds (#476).
- Documentation transitioned to Markdown; staleness audit; broken README icon URL fixed (#514).
- **Late-cycle docs polish.** Petar Maymounkov's email + paper URL refreshed to current (#851); IRC channel pointed to `irc.libera.chat` (#855); leftover references to the removed XAS module dropped (#862); README project-migration notice (#888); English man-page masters resynced to source (#900); `--enable-webserver` build hint refreshed in amuleweb-launch failure (#866, fixes #864); copyright headers refreshed across the source tree (#867); EC host default corrected in i18n docs (#895).
- **Code-quality follow-ups.** `FileLock` platform `#ifdef` moved out of class body (#909); redundant null guard after early return removed from `SharedFileList` (#910); unused UPnP symbols dropped from `WebSocket` and `SafeFile.h` typo fixed (#912).
- po4a config restored + optional CMake `po4a-update` target (#485).
- Wiki content migrated into the repository (#496).
- **Crash diagnostics.** `MuleDebug` PIE-translates runtime PCs back to link-time addresses for bfd (#677, #678), uses `popen()` instead of `wxExecute()` for the addr2line fallback (#682), and the `Do_not_auto_remove` marker is replaced with the modern `IWYU pragma: keep` (#679). amulegui's `OnFatalException` overridden with libbfd backtrace (#695). `wxcas` / `alc` disable wxSizerFlags consistency checks at startup (#694) — eliminates spurious assertion noise that broke the apps under wx 3.3.
- **Tray-icon overhaul.** Unified Linux/macOS/Windows tray path (#574); dock icon dropped while hidden via tray on macOS (#a5a9a5866); `HideOnClose` decoupled from confirm-exit (#f1d0e85f0); iconize-aware menu + single-left toggle + robust ShowGUI (#0640373c8); `ServerWnd` sash anchored to top, persist-on-drag only (#47c811191); 'Enable Tray Icon' master placed before its dependents (#64b83e48e). Linux-specific tray-icon limitations gated separately (#2a0b874d7). 'Hide on close' exposed cross-platform behind the tray-icon gate (#e82b4bac5).
- **Wayland.** amule-remote-gui binds `wl_app_id` via `g_set_prgname()` (#572).
- **GUI dispatch polish.** `MuleNotebook` dispatches the right-click forward synchronously so PopupMenu's grab is fresh (#683). `ClientDetailDialog` binds Escape to Close (#681). Destructive YES_NO prompts no longer treat ESC as confirm (#612).
- **amulecmd.** Search filters (type/extension/availability/size) now exposed via flags (#542) and documented (#544, #547). amulecmd.1 stops translating command names in section headers (#4296bbccf).
- **amule** `--disable-fatal` also skips the assertion dialog (#549).
- **First-run prompt** for nodes.dat alongside server.met (#558), with translations regenerated (#71542203f).
- **Logger.** Sticky stdio EOF cleared on `CLoggerAccess` so EC log polls see new lines (#567). Control characters escaped in 'Invalid Kad tag type' log lines (#550). `ExternalConnector` console binaries respect `VerboseDebug` from amule.conf (#654) and `setlocale` at init so non-interactive output keeps accented chars (#629).
- **Y2038.** Internal time-handling unified on `uint64 GetTickCount64()` to properly manage milliseconds past 2038 (#702). `CLOCK_REALTIME` → `CLOCK_MONOTONIC` for steady-state intervals (#56a369e95).
- **EC ADD_LINK** aggregates per-link result into a single response (#551).
- **Misc.** `asio` sets `FD_CLOEXEC` on listen + UDP sockets (#552). `FileFunctions` bails out when `wxZipInputStream` stalls on a bad entry (#553). `GuiEvents` NULL-guards `ECNotifier` before dereferencing on download notifies (#554). `StatTree` stops double-counting session bytes (#555) and guards total ratio div-by-zero on fresh installs (#585). `KnownFileList` takes list_mut before opening `known.met.new` (#546) and closes a TOCTOU UAF window in `Save()` / `PruneDuplicates` (#690). `SharedFileList` takes `list_mut` around `m_keywords` ops in `Reload` and `Publish` so concurrent Kad publishes don't iterate a mid-mutation map (#686). `PrefsUnifiedDlg` flags user-home parents as sensitive (#599). amuleweb URL migration to `amule-org` and dead `amule.org` refs removed (#526).
- **Tooling / type cleanup.** `fileview` CLogger stub extended with `IsEnabled()` for Debug builds (#657). `muleunit` adds missing override keywords (#664). Sockets `override` keyword sweep on `CEMSocket`-derived classes (#625). `WebServer` uses `vector::data()` in `CProgressImage::CreateSpan` (#684). Constant rename: `PR_VERYLOW` → `PR_VERY_LOW` (#97fcd3ae8); `PS_WAITINGFORHASH` → `PS_WAITING_FOR_HASH` (#2ae100b56). Webserver internal renames: `gaptag` / `parttag` / `reqtag` → `gap_tag` / `part_tag` / `req_tag` (#412f5aa27); `autoprio` → `auto_priority` (#f0bca611b). Backtrace internal rename: `s_abfd` → `s_a_bfd` (#5a576c3c1). Webserver enum typo fixes (#5a0d13718). Spelling sweeps across webserver (#fd823eff8, #0cda3b03c) and backtrace (#bb76d0ad8).
- **PartFile.** `m_lastDateChanged` stamped on data arrival, not flush (#638). `FlushBuffer` size-correction guarded against closed fd (#609). `m_hpartfile` locked in `ReadData` + `AICHRecoveryDataAvailable` (#709). `HashSinglePart` bound-checked against partfile length (#130b0f495). Async hash enqueue gated on `IsComplete` (#c660e1281). Per-part hashing hardened against gap/disk-state drift (#560).
- **Preferences.** No longer drops inaccessible shared dirs on load (#707); preserves unknown WebServer Template across GUI Save (#621); prefs/directories surfaces that the Incoming folder is shared (#631). `ExternalConnector` deletes `m_ECClient` in dtor to stop one-shot leak (#706).
- **Listctrl polish.** `RefreshLines` skipped when range is outside visible (#624). Unfocused selection visible on themes where `BTNSHADOW == LISTBOX` (#646). LTR cell paint forced to fix RTL-locale glyph mirroring (#540).
- **Search dialog.** Clipped Min/Max Size + Availability spin controls on Linux (#607, #82626f2f3). 10px-tall extension input field (#569 / #971310402).
- **Misc UI.** `amuleDlg` re-Realizes toolbar after Show() to fix wxMSW long-locale clipping (#532). Hardcoded 20px height on the FastEd2kLinks bar dropped (#534, #c710c08fe). Icons migrated from `XPM #include` to `wxArtProvider` + embedded PNG (#537, #59d718818). Gtk-CRITICAL warnings on Preferences + Networks tabs silenced (#739).
- **PartFileConvertDlg** collects ids before removing rows so `GetNextItem` stays valid (#649).
- **SafeFile** oversized-string assertion demoted to debug log (#617).
- **KnownFile.** Skip notify when `SetPublishedED2K` value didn't change (#dde5294fc). `m_duplicateFileList` growth bounded with a per-hash cap (#f33c56b44).
- **Code-quality baseline.** Baseline `.clang-tidy` configuration introduced (#770); first sweeps produced 6 safety fixes (#773) and 4 cosmetic fixes (#774); `wxASSERT` → `wxCHECK_MSG` on array-access guards in `Logger` / `UserEvents` (#772).
- **Dead-code removal.** 2011-era `/Statistics` legacy config migration dropped (#825); long-broken "Automatic server connect without proxy" checkbox removed from preferences (#806).

### Translations

- Italian: AppImage integration prompt translated (#518).
- Spanish updated and completed (#497).
- Dutch updated (#343).
- Catalan updated (#425).
- Simplified Chinese updated (#348).
- Translation pipeline: `scripts/update-po.sh` introduced.
- **AI-assisted gap-fill** for it / fr / de / es app strings (#582) and man pages (#580).
- **Spanish** further updates (cpp + docs, manpages rebuilt) (#655).
- **French + Turkish** updated (#691) and added to the AppStream info file (#698, #86a08ffc7).
- **Brazilian Portuguese (pt-BR)** added to metainfo (#86bafee35); metainfo screenshot URLs fixed (`amule/amule` → `amule-org/amule`, #cf84ec2c6); TODO comment syntax fix (`shell` → `XML`, #fc205619f).
- **po regeneration** from updated sources + msgmerge across all locales (#c2bbea0ed).
- **Late-cycle wave**: French + Turkish manual pages (#753, #754, #776); Galician update (#763); Slovenian recovered from forum.amule.org (#771); Brazilian Portuguese man page added (#768) and the `.po` completed (#775); pt-BR translations on 4 `.desktop` files (#812); French manual-page update (#811).
- **Man-page tooling**: `.TH` headers templatized for date + version so they no longer drift per release (#802).
- **Pre-release final wave** (3.0.0 cut): French + Turkish updates (#847, #856, #891, #908); Brazilian Portuguese (#860, #904); Spanish app + man pages (#859, #863); Italian completed (it / it_CH fuzzy + untranslated) (#861); German (#880, #911); Vietnamese README added (#901); regenerated po/pot for app + man pages with Italian translations applied (#902); obsolete (#~) entries swept from app + man po (#889); stale Copyright year bumped + obsolete entries pruned (#868); Petar Maymounkov's email + paper URL updated in all catalogs (#853).

### Bug Fixes & Stability

- `CFile::doSeek` no longer asserts when `IsOpened()` is false (#294).
- `RLE_Data` memory delete issue fixed (Valgrind report) (#379).
- Selected lines in Download/Shared lists no longer become illegible when losing focus (#385).
- amulegui: clean startup on Cancel + watchdog on wrong connection data (#465).
- amuleweb: missing `prefs apply` options restored (#419).
- `PartFileWriteThread` catches `CIOFailureException` so disk-full doesn't kill the process (#499).
- StatTree fix (#319); totalizers added to download/upload stats (#338, #339).
- Doc / source-comment typos cleanup (#297); missing include (#360).
- **amuleweb crash with >65535 shared files** fixed (#700) with regression test (#61276d70d).
- **CFile musl SIGSEGV + concurrent corruption** caught and fixed (#576) — musl's 128 KiB pthread stack interacted badly with the 64 KB userspace write buffer added in #573; a separate concurrent-corruption race on a shared write buffer was also fixed in the same PR.
- **macOS link error** caused by implicit AppKit framework dependency now resolved by naming it explicitly (#578).
- **Wake-from-sleep SIGSEGV** in asio socket impls — root cause `shared_from_this` lifetime gap (#596). NULL `m_socket` also guarded in UDP DispatchClose (#82b5400dc).
- **WebUI** rate-limit fields no longer truncate at 16 bits (#645).
- **PartFileConvertDlg** iteration invalidation crash (#649).
- **amulegui** EC connect-timeout shutdown crash (#717).
- **ExternalConnector** one-shot `m_ECClient` leak on dtor (#706).
- **EC notification path** raw-pointer `CECPacket` leak plugged (#797).
- **Detect dead amuled** cleanly across all clients (#758); EC `--upnp-port` fixed from switch to numeric option in `amuleweb` (#820); `amuleweb` `remote.conf` round-trip + Windows webserver template path fixed (#822).
- **amulegui improvements and fixes**: ghost-entry sources — stale EC alive-marker for unknown ID no longer creates phantom rows (#810), `INC_UPDATE` tag for unknown file ID with suppressed identifying metadata now skipped cleanly (#819); connection dialog reappears on failure instead of quitting (#841); ED2K Info pane readability + clipboard copy fixed (#824); `DirectoryTreeCtrl` `wxNullFont` crash on the Windows revert-recursive-share path (#830); modal popups deferred out of `OnPacketReceived` call stack (#760). ECIDs the client hasn't received yet now sent with full detail instead of an empty alive marker (#857).
- **PartFile**: first-share early hash so a downloading file appears in "Shared files" before it completes (#762); drop LowID-0 sources in `CanAddSource` (#790).
- **Crypto stream**: don't UB if `EncryptedStreamSocket::Negotiate()` is entered with no expected bytes (#779).
- **UAF prevention**: `Notify_KnownFileBeingDestroyed` broadcast on every `CKnownFile` destruction (#756).
- **GUI / GTK warnings**: silenced on the Advanced Preferences tab via sizer simplification (#833); slider min-height pin attempted (#826) then reverted (#836) after wxGTK 3.2.9 surfaced an invisibility regression.
- **SearchListCtrl** dropped bogus "File" title from right-click popup (#769).
- **Three GUI fixes from #800**: toolbar races and web-template placeholder loop closed (#803).
- **amuled** constructs `CPartFileWriteThread` + hash worker AFTER the InitGui fork so the daemon no longer crashes on first flush (#850, fixes #849).
- **Locale.** `LC_CTYPE=C` scoped around `wxFileConfig` ops and the country-flag lookup so non-C locales no longer break preference number parsing or flag matching (#854, fixes #852).
- **ServerUDPSocket** `Delete()` leaked the detached `CAsyncDNS` on thread Create/Run failure (#878).
- **eD2k socket** `CLibSocket::OnLost(int)` now forwards to `CEMSocket::OnClose` so server/peer disconnects actually fire (#906).

### CI

- macOS and Windows MinGW64 build jobs added (#458); ccpp.yml adjustments (#460).
- Ubuntu CI installs `binutils-dev` so the BFD path is exercised (#488).
- Assert tests gated on `wxDEBUG_LEVEL`; `ctest` runs in Release in CI (#506).
- CodeQL adjustments and code-scanning fixes (#477, #478, #482).
- `actions/checkout` bumped from v4 to v6 (#446).
- **ccache wired across all compile jobs** in both `ccpp.yml` (#892) and the packaging matrix (#903) — `actions/cache`-backed, gated off for tag builds. CodeQL workflow also gets `binutils-dev` so its BFD detection mirrors the main Ubuntu job (#907).

### Known Limitations

Known caveats users should be aware of for this release:

- macOS `.dmg` is **not yet code-signed or notarized** — users will see Gatekeeper warnings on first launch.
- **Flathub submission pending** — for now, install the `.flatpak` directly with `flatpak install <file>`.
- AppImage doesn't yet ship through AppImageLauncher's catalogue.
- Tray-icon library (`libayatana-appindicator`) is deprecated upstream; migration to `libayatana-appindicator-glib` deferred until LTS distros pick it up.

### Contributors

This release reflects 5+ years of work across 234 merged PRs (98 initial + 136 follow-ups through to the tag). Particular thanks to:

- **got3nks** — 182 PRs.
- **Marcelo Jimenez** (mrjimenez) — 17 PRs.
- **Werner Mahr** (Vollstrecker) — 38 commits.
- **Pablo Barciela** (sc0w) — 3 PRs, 17 commits.
- **Dévai Tamás** (GonoszTopi) — 16 commits.
- **danim7** — 5 PRs across the follow-up cycle (translations, server pings, y2038, cmake polish).
- **cardpuncher** — French + Turkish translations and AppStream entries (#691, #698).
- **ngosang** — UX feedback driving the late-3.0 cycle (issues #817, #818, #821, #828, #844) and ongoing work on the user-facing manual at <https://amule-org.github.io>.

Plus contributions from RealGreenDragon, frnjjq, Sergi Amoros, Stefano Picerno, Alexander Tsoy, sergiomb2, puleglot, mercu01, lggcs, comio, MPolleke, mike2718, joebonrichie, matoro, dependabot, tbo47, SevC10, topotech, minterior, luzpaz, loongson-zn, mifritscher, nguyenhoangminhhieu2004-gif.

### Merged PRs in this release

Complete list of PRs that landed for 3.0.0, in numerical order. The narrative sections above call out the most impactful changes; this index gives the full traceback.

#286, #294, #297, #319, #338, #339, #343, #348, #360, #367, #368, #379, #385, #387, #419, #421, #424, #425, #429, #430, #436, #438, #439, #440, #441, #443, #446, #447, #448, #449, #451, #452, #453, #454, #456, #457, #458, #459, #460, #461, #462, #463, #464, #465, #466, #467, #468, #469, #470, #473, #474, #475, #476, #477, #478, #480, #481, #482, #483, #484, #485, #487, #488, #489, #490, #491, #492, #493, #494, #495, #496, #497, #498, #499, #500, #501, #502, #504, #505, #506, #507, #508, #509, #510, #511, #512, #513, #514, #515, #518, #526, #529, #530, #532, #534, #537, #540, #542, #543, #544, #545, #546, #547, #548, #549, #550, #551, #552, #553, #554, #555, #556, #557, #558, #560, #561, #563, #565, #566, #567, #569, #570, #571, #572, #573, #574, #576, #577, #578, #580, #581, #582, #584, #585, #591, #593, #594, #596, #598, #599, #603, #605, #606, #607, #609, #612, #613, #614, #615, #617, #618, #619, #620, #621, #623, #624, #625, #627, #629, #630, #631, #632, #633, #634, #638, #639, #645, #646, #647, #648, #649, #652, #653, #654, #655, #657, #660, #661, #662, #664, #667, #670, #671, #672, #675, #677, #678, #679, #681, #682, #683, #684, #686, #687, #689, #690, #691, #694, #695, #698, #700, #701, #702, #706, #707, #709, #710, #715, #716, #717, #718, #719, #721, #725, #726, #727, #728, #729, #730, #732, #733, #736, #739, #740, #743, #744, #746, #747, #750, #751, #753, #754, #756, #758, #760, #762, #763, #768, #769, #770, #771, #772, #773, #774, #775, #776, #779, #780, #784, #785, #787, #788, #789, #790, #794, #795, #796, #797, #799, #801, #802, #803, #805, #806, #809, #810, #811, #812, #819, #820, #822, #824, #825, #826, #830, #833, #834, #835, #836, #839, #840, #841, #845, #847, #850, #851, #853, #854, #855, #856, #857, #858, #859, #860, #861, #862, #863, #866, #867, #868, #875, #878, #879, #880, #882, #886, #888, #889, #890, #891, #892, #895, #898, #899, #900, #901, #902, #903, #904, #906, #907, #908, #909, #910, #911, #912.
