# 3dsaves

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
  is deployed: Dropbox redirects there instead of displaying the code, and
  the 3DS polls it automatically. See that project's README for why a
  relay is needed (short version: the browser completing login runs on a
  phone, a different device than the 3DS, so a direct OAuth redirect to
  the 3DS isn't possible). Manual entry still works as a fallback.

## What it does

1. Lists installed titles (SD + inserted cartridge) that look like regular
   games (`source/saves.c`), showing each one's actual name (read from its
   SMDH via `ARCHIVE_SAVEDATA_AND_CONTENT`, `source/title_name.c`,
   technique adapted from [selloa/3DS-Random-Game-Launcher](https://github.com/selloa/3DS-Random-Game-Launcher),
   MIT-licensed) instead of just a product code, falling back to the
   product code + title ID if a title has no readable SMDH.
2. Reads the selected title's `SAVEDATA` archive recursively and packs it
   into a `.zip` (stored, no compression) on the SD card
   (`source/saves.c`, `source/minizip_writer.c`).
3. Logs in to Dropbox using OAuth 2.0 Authorization Code + PKCE. The
   authorize URL is shown as a QR code drawn directly to the top screen's
   framebuffer (`source/qr_display.c`, using a vendored copy of
   [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator),
   MIT-licensed) so you scan it with a phone instead of typing a long URL
   by hand; the plain-text URL still prints on the bottom screen as a
   fallback. If `cloudflare-relay/` is deployed and configured (see its
   README), login finishes automatically once you approve on Dropbox --
   no code to type at all. Otherwise it falls back to the no-redirect-URI
   flow, where Dropbox shows a code you type back in via the 3DS software
   keyboard (`source/auth.c`). No client secret is embedded in the binary
   either way.
4. Uploads the zip to `/3dsaves/<PRODUCT-CODE>_<TITLEID>.zip` in the
   user's Dropbox via `POST /2/files/upload` (`source/dropbox.c`).

Networking goes through libctru's `httpc` service (`source/http.c`), which
handles TLS itself — no raw sockets / `soc:u` needed. A handful of modern
root CA certs are bundled in `romfs/certs/` and trusted at runtime,
because the console's built-in cert list is dated. See
`romfs/certs/README.md` for why and how to refresh them.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`3ds-dev` package group (devkitARM + libctru):

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make
```

Produces `3dsaves.3dsx` (+ `.smdh`). Copy it to `/3ds/3dsaves/` on the SD
card and launch via Homebrew Launcher — **`.3dsx` is not something FBI
installs**, it just runs directly from that launcher.

### Building a `.cia` instead (installs with FBI)

Needs two extra tools that aren't part of the base `3ds-dev` group:

```sh
(dkp-)pacman -S general-tools   # provides makerom + bannertool
make cia
```

Produces `3dsaves.cia`. Copy it to the SD card and install it with FBI
like any other CIA — it'll get a real HOME menu icon (a plain generated
placeholder icon/banner for now, see `resources/`).

## One-time setup: register a Dropbox app

1. Go to the [Dropbox App Console](https://www.dropbox.com/developers/apps)
   and create an app:
   - API: **Scoped access**
   - Access type: **App folder** (simplest — keeps it to `/Apps/3dsaves`)
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
  manually), and approve access. If the Cloudflare relay is set up, that's
  it -- the 3DS picks up the login on its own within a couple seconds.
  Otherwise, press A on the 3DS and paste the code Dropbox shows you.
- Pick a title from the list and it backs up + uploads immediately.
  Titles show their real name when available, falling back to product
  code + title ID otherwise.

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
- **`.3dsx` end-to-end still needs a real backup+upload test.** Title
  listing, the QR login flow, and the UI have been confirmed on real
  hardware; a full "pick a title, upload, check Dropbox" pass hasn't yet.
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
