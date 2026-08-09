# Konnect3DS

A Nintendo 3DS homebrew app that backs up a game's save data to Dropbox.

Builds as either a `.3dsx` (run via Homebrew Launcher, no extra tools) or
a `.cia` (install with FBI, get a HOME menu icon like a real app). Reading
another title's save archive needs the same elevated FS access tools like
[JKSM](https://github.com/J-D-K/JKSM) / [Checkpoint](https://github.com/BernardoGiordano/Checkpoint)
rely on — see the CIA permission note under "Known rough edges" below.

## Status

Both `.3dsx` and `.cia` compile clean. On-hardware testing (real 3DS,
via a user, not this session -- no devkitARM/3DS available here) has
caught and fixed real bugs, most notably a scrambled QR code caused by
a wrong raw-framebuffer draw (see the fix note below and in
`source/qr_display.c`) -- treat anything not explicitly confirmed working
below as still needing a look.

Fixes applied while getting the build green and iterating on real
hardware (kept here so it's obvious what changed since the initial
scaffold):
- `ARCH` flags updated for current devkitARM/GCC (`-mtype=thumb
  -mthumb-interwork` were removed upstream; now `-mtp=soft`).
- Missing `#include <stdio.h>` / `<stddef.h>` in `include/http.h` /
  `include/dropbox.h` (used `FILE`/`size_t` without including them).
- `_3DSXDEPS` was referenced but never defined, so the `.smdh` icon never
  actually got built before `3dsxtool` tried to embed it.
- `makerom`/`bannertool` aren't in any devkitPro pacman package (despite
  `general-tools` sounding like it should have them) -- they're separate
  manual downloads, documented below.
- **QR code was scrambled on real hardware**, through several rounds of
  debugging: disabling double buffering before drawing (removed, redraw
  every frame instead), oversized stack buffers in the QR encoder (moved
  to the heap), and finally the real fix -- explicitly forcing
  `GSP_BGR8_OES` instead of trusting whatever pixel format this devkitPro
  version defaults to. Confirmed rendering correctly on real hardware
  after that.
- **Login no longer requires typing a code at all**, if `cloudflare-relay/`
  is deployed: the 3DS shows a QR code for the relay's `/start`, the
  phone does the whole Dropbox login+approval there, and the relay's
  `/callback` page ends with a "Download" button for a small file
  containing the finished tokens. Copy that file (not its contents) to
  `3ds/Konnect3DS/paste_tokens.txt` on the SD card and the 3DS picks it
  up automatically within about half a second. Manual entry still works
  as a fallback. See `cloudflare-relay/README.md` for why the exchange
  happens entirely on the relay instead of the 3DS polling it directly.
- **Every HTTPS request was failing TLS certificate verification**
  (`rc=0xD8A0A03C`), against both the Cloudflare relay and Dropbox's own
  servers, no matter which root CAs were bundled — root-caused to the
  3DS system's own TLS stack being unable to handshake with modern
  servers at all (even the native Internet Browser fails identically).
  Fixed by rewriting `source/http.c` to do TLS itself, over raw sockets
  (`soc:u`) + mbedTLS, instead of going through `httpc`/`ssl:C`. Confirmed
  working on real hardware, including TLS version/cipher negotiation and
  certificate verification.
- **The 3DS's own HTTPS requests to Cloudflare-fronted domains get a raw
  `400 Bad Request` straight from the edge**, on every domain tried
  (`*.workers.dev`, a dedicated custom domain, even an unrelated
  third-party Cloudflare-fronted site) despite byte-exact, well-formed
  requests and clean TLS negotiation — while the exact same client works
  fine against Dropbox's own servers. Root cause unresolved; the
  Cloudflare relay was redesigned around it instead (see the bullet
  above and `cloudflare-relay/README.md`) so the 3DS never needs to
  successfully reach Cloudflare at all.
- **Dropbox destination changed from a flat `/Konnect3DS/<code>_<id>.zip`
  to one folder per game**, `/Konnect3DS/<game name>/<game name>.zip`
  (`dropbox_build_game_path()` in `source/dropbox.c`), and **saves can
  now come from an existing folder on the SD card**, not just a title's
  own save archive (`source/sd_browse.c`, `saves_backup_folder()` in
  `source/saves.c`) -- in particular, a
  [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) backup
  (its on-SD layout confirmed against Checkpoint's own source, not
  guessed). Not yet confirmed on real hardware.

## What it does

1. Lists installed titles (SD + inserted cartridge) that look like regular
   games (`source/saves.c`), showing each one's actual name (read from its
   SMDH via `ARCHIVE_SAVEDATA_AND_CONTENT`, `source/title_name.c`,
   technique adapted from [selloa/3DS-Random-Game-Launcher](https://github.com/selloa/3DS-Random-Game-Launcher),
   MIT-licensed) instead of just a product code, falling back to the
   product code + title ID if a title has no readable SMDH.
2. Reads the selected title's `SAVEDATA` archive recursively and packs it
   into a `.zip` (stored, no compression) on the SD card
   (`source/saves.c`, `source/minizip_writer.c`). **Import save from
   folder...** in the menu does the same for a save that already exists as
   loose files on the SD card instead of inside a title's own save archive
   -- most usefully, a [Checkpoint](https://github.com/BernardoGiordano/Checkpoint)
   backup. The picker (`source/sd_browse.c`) starts at
   `/3ds/Checkpoint/saves` when that folder exists (Checkpoint's own save
   location, one subfolder per game named `0x<unique id> <game name>`) but
   lets you navigate anywhere else on the SD card too.
3. Logs in to Dropbox using OAuth 2.0 Authorization Code + PKCE. The
   authorize URL is shown as a QR code drawn directly to the top screen's
   framebuffer (`source/qr_display.c`, using a vendored copy of
   [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator),
   MIT-licensed) so you scan it with a phone instead of typing a long URL
   by hand; the plain-text URL still prints on the bottom screen as a
   fallback. If `cloudflare-relay/` is deployed and configured (see its
   README), the QR code points at the relay instead of Dropbox directly:
   the relay does the whole login+token exchange server-side and offers a
   downloadable token file to copy onto the SD card -- no code to type,
   and no network request from the 3DS to the relay at all. Otherwise it
   falls back to the no-redirect-URI flow, where Dropbox shows a code you
   type back in via the 3DS software keyboard (`source/auth.c`). No
   client secret is embedded in the binary either way.
4. Uploads the zip to `/Konnect3DS/<game name>/<game name>.zip` in the
   user's Dropbox via `POST /2/files/upload` (`source/dropbox.c`), one
   folder per game (falls back to the product code if a title has no
   readable name). Dropbox-unsafe characters in the name (`: / \ < > " | ? *`
   and friends -- real game titles routinely have some of these) are
   replaced with `_` first.

Networking (`source/http.c`) uses raw sockets (`soc:u`) + mbedTLS, not
libctru's `httpc`/`ssl:C` service. The 3DS's own system TLS stack can't
complete a handshake with any modern server at all — confirmed against
both Dropbox's and Cloudflare's servers, and even the console's native
Internet Browser fails the same way against google.com/dropbox.com with a
generic "update your browser" error. This is a known, documented
limitation (the same reason FBI ships its own TLS stack), not something
fixable by adjusting which root CAs get trusted. The same bundled root CA
set in `romfs/certs/` is still used, just parsed by mbedTLS instead of
handed to httpc — see `romfs/certs/README.md` for how each one was
verified.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`3ds-dev` package group (devkitARM + libctru) plus mbedTLS:

```sh
(dkp-)pacman -S 3ds-mbedtls   # pulls in 3ds-zlib as a dependency
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make
```

Produces `Konnect3DS.3dsx` (+ `.smdh`). Copy it to `/3ds/Konnect3DS/` on the SD
card and launch via Homebrew Launcher — **`.3dsx` is not something FBI
installs**, it just runs directly from that launcher.

### Building a `.cia` instead (installs with FBI)

Needs two extra tools that aren't part of the base `3ds-dev` group:

```sh
(dkp-)pacman -S general-tools   # provides makerom + bannertool
make cia
```

Produces `Konnect3DS.cia`. Copy it to the SD card and install it with FBI
like any other CIA — it'll get a real HOME menu icon (a plain generated
placeholder icon/banner for now, see `resources/`).

## One-time setup: register a Dropbox app

1. Go to the [Dropbox App Console](https://www.dropbox.com/developers/apps)
   and create an app:
   - API: **Scoped access**
   - Access type: **App folder** (simplest — keeps it to `/Apps/Konnect3DS`)
     or **Full Dropbox** if you'd rather control the path yourself.
   - Under **Permissions**, enable `files.content.write` and
     `files.content.read`.
2. Copy the app's **App key**.
3. Put it in `include/auth.h`, replacing `PUT_YOUR_DROPBOX_APP_KEY_HERE`
   in `DROPBOX_CLIENT_ID` — or pass it at build time instead:
   ```sh
   CFLAGS+=' -DDROPBOX_CLIENT_ID=\"your_app_key\"' make
   ```
   No app secret is needed (PKCE flow).

## Optional: no-typing login via the Cloudflare relay

By default, logging in means typing a Dropbox-generated code on the 3DS's
software keyboard once. `cloudflare-relay/` removes that step entirely --
see its README for what it does and why, and for full deploy steps. Once
deployed:

1. Add `<your-worker-url>/callback` as a Redirect URI in the Dropbox App
   Console (**Settings** -> **OAuth 2** -> **Redirect URIs**) -- required,
   Dropbox rejects unregistered ones.
2. Set `RELAY_BASE_URL` in `include/auth.h` (or via `-DRELAY_BASE_URL=...`
   at build time, same as `DROPBOX_CLIENT_ID`) to your Worker's URL.
3. Rebuild.

Skip this and leave `RELAY_BASE_URL` as the placeholder to keep the
original manual-code-entry flow -- it still works fine, just needs that
one bit of typing.

## Using it

- On first run, select **Log in to Dropbox**, scan the QR code on the top
  screen with your phone (or type the URL printed on the bottom screen
  manually), and approve access. If the Cloudflare relay is set up, finish
  by tapping **Download** on the page that follows and copying that file
  (via FTP or similar) to `3ds/Konnect3DS/paste_tokens.txt` on the SD
  card -- the 3DS picks it up on its own within about half a second.
  Otherwise, press A on the 3DS and paste the code Dropbox shows you.
- Pick a title from the list and it backs up + uploads immediately.
  Titles show their real name when available, falling back to product
  code + title ID otherwise.
- **Import save from folder...** backs up a save that's already loose
  files on the SD card instead of packed inside a title's own save
  archive -- opens straight into `/3ds/Checkpoint/saves` if it exists.
  Navigate with Up/Down/A, **[Select this folder]** confirms whatever
  folder you're currently browsing, **[.. Parent folder]** goes up a
  level, B cancels.

## Known rough edges / follow-ups

- **CIA permission list is the least-verified part.** `resources/template.rsf`
  declares `CategorySystemApplication` in `FileSystemAccess` specifically so
  a properly-installed CIA (not run through the Homebrew Launcher exploit,
  which already has broad FS access regardless of any RSF) can open other
  titles' `SAVEDATA` and `ARCHIVE_SAVEDATA_AND_CONTENT` (used for title
  names) archives. Whether this is sufficient depends partly on what your
  CFW (e.g. Luma3DS) patches system-wide — if `.cia` save backup or title
  names fail with a permission error on real hardware, this list is the
  first place to check.
- **`.3dsx` end-to-end backup+upload confirmed on real hardware** (login,
  pick a title, zip, upload, verified in Dropbox).
- **Cartridge saves are listed but not specifically verified.**
  `saves_list_titles()` already scans `MEDIATYPE_GAME_CARD` alongside SD
  the same way as any installed title, but that path hasn't been
  exercised with an actual cartridge inserted -- deprioritized for now in
  favor of **Import save from folder...**, which covers a cart's save via
  whatever already-extracted-to-SD-card tool a user has (e.g. Checkpoint)
  instead of this app needing its own cartridge-specific read path.
- **`EXTDATA`-only titles aren't handled**, only `SAVEDATA`
  (`ARCHIVE_USER_SAVEDATA`). Some titles keep save data in extdata
  instead/as well; adding that archive type is a moderate amount of
  additional, less-common code.
- **Large saves are fully buffered in RAM** for both zipping and upload
  (`httpc`'s raw POST API wants the whole body up front). Fine for
  typical save sizes; would need chunked upload handling for very large
  extdata.
- **ZIP entries use STORE (no compression).** Simple and easy to verify
  by hand; swapping in DEFLATE (devkitPro's `zlib` portlib) would shrink
  uploads if that ever matters.
- **One provider (Dropbox).** Google Drive and OneDrive were the other
  candidates discussed; if a second provider gets added, give
  `auth.c`/`dropbox.c` a shared interface first.
- **No restore path yet** — this only backs up. Restoring would mean the
  reverse: download from Dropbox, unzip, and write files back into the
  `SAVEDATA` archive, then `FSUSER_ControlArchive` commit if the archive
  needs it (double-check this against Checkpoint's restore code).

## Project layout

```
source/     application code (.c), plus a vendored copy of nayuki's
            qrcodegen (MIT license, embedded in the file header) and an
            SMDH-reading technique adapted from 3DS-Random-Game-Launcher
            (MIT, credited in title_name.c)
include/    headers (.h)
romfs/      bundled read-only assets (root CA certs), embedded in both
            the .3dsx (via 3dsxtool --romfs) and the .cia (via the RSF's
            RomFs/RootPath)
resources/  CIA-only: AppInfo (title/author/ids), template.rsf
            (permissions), icon.png/banner.png/audio.wav (placeholder art)
Makefile    devkitARM build rules (`make` -> .3dsx, `make cia` -> .cia)
cloudflare-relay/
            optional Cloudflare Worker that lets login finish without
            typing a code -- see its own README
```
