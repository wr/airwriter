# Setup password and sign-in

2026-11-15. `W-1156 Setup and the settings page are locked by a password`. Decided by Wells in
the brainstorm of 2026-11-15, from mocks of three ways the page could ask and of the sign-in
screen. The bottom label that carries the password is `W-1155 Bottom label: rating plate with
serial, FCC, IC and CE marks, and maker's address`.

## What changes, in one paragraph

Every AirWriter that is sold gets its own random password, burned into the chip at the bench and
printed on its bottom label. The setup network takes that password (WPA2) instead of being open.
On the home network the settings page opens on a sign-in screen and shows nothing until the
password is entered; the browser then stays signed in for 6 months. Printing never asks. The
person can change the password in About; erasing the settings brings back the label's. A board
with nothing burned in (rev A, a dev board, one someone built) starts open, as today, and its
owner can set a password in About; erasing the settings takes it off again.

## Why

Since August 2025 a Wi-Fi device that reaches the internet has to meet EN 18031 to carry CE.
Its listing in the EU's Official Journal lets a maker sign off on their own only if the user
can't skip setting a password, and an open setup network and a settings page with no login
fail that. Britain's PSTI law wants a password unique to each device or set by its owner.
Meeting both on the device is what lets the EU paperwork be self-declared instead of paid for
(`W-1158 EU and UK paperwork: technical file, declaration of conformity, box card, EU
representative`).

## Goals, non-goals, anti-goals

- **Goals.** No open network and no settings page reachable without the password, on any board
  that is sold. A password the person never has to set, printed where they'll find it. One
  password for AirWriter: the same one joins the setup network and signs in. Signing in once
  per browser, not once per visit.
- **Non-goals.** https. Accounts or more than one password. Hiding status from the printers
  themselves: AirPrint, AppleTalk and the test button are untouched. Showing the serial on the
  page (Wells: the board never shows it). Making rev A or dev boards take a password: they can
  set one, nothing makes them. Read protection of the
  eFuse block: whoever holds the board holds its label.
- **Anti-goals**, which would make this a failure even if it builds:
  - a print job, from a phone, a Mac or a vintage Mac, that asks for anything
  - a person locked out with no way back short of a USB cable (holding the test button through
    power-up must always bring back the label's password)
  - the password crossing the network readable when the page signs in
  - a burned board that comes up with an open network
  - a password list, label art with the postal address, or a test password in the public repo

## Approaches considered

| | Approach | Verdict |
|---|---|---|
| Where the password comes from | Random per board, burned into eFuse at the bench, printed on its label | **Chosen** |
| | Set by the person during setup | Rejected: the setup network is open until then |
| | A setup window opened by holding the test button | Rejected: doesn't lock the settings page |
| | No password, with an off switch | Rejected by Wells: an optional password rules out self-declaring EN 18031 |
| How the page asks | A conventional sign-in before anything shows | **Chosen** by Wells |
| | Reading open, a dialog at the first change | Rejected by Wells for C |
| | Reading open, a lock bar on every pane | Rejected by Wells for C |
| How the page sends it | Plain http, challenge-response, so the password never crosses the network | **Chosen** |
| | https with a certificate made on the board | Rejected: every browser warns on it |
| | Plain http, password posted as typed | Rejected: readable by anyone on the network |
| How labels are printed | At the bench, on the Personal LaserWriter 300, sheets of 2.5 × 1.25 in white matte polyester labels | **Chosen** by Wells |
| | Ordered pre-printed from a list | Rejected: the password list leaves the bench |

Wells chose from mocks of all three ways and of the sign-in screen; they are not in the repo.

## How it works

### The password

- **Format** (my call): 8 characters from `abcdefghjkmnpqrstuvwxyz23456789` (lowercase letters
  and digits without 0, o, 1, l and i), so it reads and types easily on a phone. Example:
  `k7m2x9qp`. That is about 40 bits; see Risks for what that buys.
- **Where it lives.** The label's password and the serial are burned together into the
  ESP32-S3's user eFuse block (`EFUSE_BLK_USER_DATA`, BLOCK3, 32 bytes, Reed-Solomon coded, so
  written once, whole):

  | Bytes | Holds |
  |---|---|
  | 0 | Layout version, `1` |
  | 1–16 | Serial, ASCII, zero-padded (`2652AWF2611-002`, 15 today) |
  | 17–24 | Password, ASCII |
  | 25–31 | Zero. On the S3 the custom-MAC field lives here; it stays unused |

  The firmware reads it at boot with `esp_efuse_read_block`. All zero means none burned: the
  board is open, as today, until its owner sets a password. Version 1 with a password of 8 characters from
  the alphabet means burned. Anything else is logged and treated as no password.
- **Test builds.** `CONFIG_AIRWRITER_TEST_PASSWORD` (string, empty by default, in
  `main/Kconfig.projbuild`) stands in for the eFuse password when the block is blank, so board 1
  is tested without being burned. A build with it set logs a warning at every boot. Release
  builds take the defaults only, so it is always empty there.
- **A changed or set password** is kept in NVS (namespace `auth`) and wins over the label's.
  It must be 8 to 63 characters, WPA2's limits, since the setup network uses it too. Erase All
  Settings, and holding the test button through power-up, erase NVS: a burned board goes back to
  the label's password, a board with none burned goes back to open.
- **Setting one** (Wells, 2026-11-15) is for a board with none burned: About offers **Set…**
  where a burned board offers **Change…**. There is no Remove; erasing the settings does that.
- **What the page sees.** `/api/status` gains `"pw"`: `"label"` (burned, as printed),
  `"changed"` (burned, changed), `"set"` (none burned, one set) or `"none"`. Never the password
  itself.

### Older firmware

A board with a burned password (or a test one) that runs the first release with sign-in or later
refuses, by `/update` and by install-by-tag, any image older than that release: on older
firmware it would come up with an open network. A dev build numbered below that release has no
floor, so a bench board moves freely between builds. The release that first ships sign-in sets
the floor to its own tag. The page
shows "This AirWriter can’t go back to a version before vX.Y." Going back to any later version
stays allowed, and a USB flash is not limited. The signed-updates section (SPEC §3.12) is
amended to say so in the same change. A board with only a set password has no floor: it is its
owner's, signed with their key.

### The setup network

`net_provision_start` takes the current password. With one, the access point is WPA2-PSK
(CCMP); without one, it stays open as today. It is whatever the board's password is now: the
label's, a set one, or a changed one if the board came up in setup after failing to join with its settings
kept. The page on the setup network never asks: joining took the password.

### Signing in on the home network

- **What is guarded.** Every route on port 80 except the page itself (`/`, the paths in
  `webui_paths.h`, its icons), `GET /api/auth/nonce` and `POST /api/auth/signin`. A guarded
  route without a good token answers 401 with `{"signin":true,"label":true|false}` (whether the
  password is the label's kind, so the sign-in screen knows what to say). Nothing is guarded in captive
  mode or on a board with no password. IPP on port 631 and AppleTalk are not touched.
- **The proof** (challenge-response, so the password never crosses the network):
  1. `GET /api/auth/nonce` answers `{"nonce":"<32 hex>","salt":"<32 hex>","iter":N}`. A nonce
     is good once, for 60 s; the board keeps up to 4.
  2. The page derives `K = PBKDF2-HMAC-SHA256(password, salt, N, 32)` and sends
     `POST /api/auth/signin {"nonce":..,"proof": hex(HMAC-SHA256(K, "signin" ‖ nonce))}`.
  3. The board holds `K` (derived at boot and on a change; the salt is 16 random bytes in NVS,
     new after an erase), checks the proof in constant time, and answers 200 with
     `Set-Cookie: aw=<32 hex>; Max-Age=15552000; Path=/; HttpOnly; SameSite=Strict`.

  `N` is 10,000, so each guess at a recorded sign-in costs more than a guess at the setup
  network's WPA2 handshake (4096 rounds). (Planned at 100,000; the first board took 7 s to derive
  that at start-up, so it came down, and the board keeps `K` in NVS rather than deriving it at
  every start.) Plain
  http has no `crypto.subtle`, so SHA-256, HMAC and PBKDF2 are a small script in the page.
- **Tokens.** Up to 8 are kept in NVS; a new sign-in pushes out the oldest. The browser drops
  its cookie after 6 months; the board needs no clock. `POST /api/auth/signout` forgets this
  browser's token. A password change forgets every token but the one that made it.
- **Wrong passwords.** After 5 wrong proofs in a row (sign-in or change), the board takes none
  for 60 s and answers 429 with `{"wait":s}`. Kept in RAM; a success clears it.
- **Changing it.** `POST /api/auth/password` with a fresh nonce, a proof made with the current
  password, and the new password encrypted with a key stream from the current one
  (`HMAC-SHA256(K, "change" ‖ nonce ‖ i)` for i = 1, 2), so neither crosses the network
  readable. The board derives the new `K`, saves both, and the setup network uses the new
  password the next time it comes up.
- **Setting the first one.** `POST /api/auth/password` with no proof, only on a board with no
  password. With nothing shared yet there is no key to hide it with, so this one time the new
  password crosses the network readable, on a network the board was already open to. The answer
  signs in the browser that set it.
- **Command-line use.** `tools/webui/signin.py <host>` asks for the password, signs in and
  prints a cookie for `curl -b`, so the bench's `curl` steps work against a burned board.
  Board 1, unburned, stays open.

### The page

The sign-in screen is laid out as setup is, without the sidebar; on a phone the button sits at
the foot. Any 401 shows it and stops the polling; signing in reloads the state.

| Where | Text |
|---|---|
| Sign-in alert | Sign in to AirWriter · Password · **Sign In** |
| Under it | Forgot password?, which opens "It’s on the bottom of AirWriter." while the label's password works, or else "Hold the test button for about 8 seconds. That erases all its settings." (the running reset, as the wiki's Test Button page has it) |
| Errors | Wrong password. · Too many tries. Try again in a minute. (Sign In dims until it may) · Try again. |
| About | Password · **Change…** (or **Set…** on a board with none) |
| About's buttons | **Sign Out** beside **Restart…**, while signed in to a board with a password |
| Change dialog | Change password · Current password · New password · **Cancel** **Change** |
| Set dialog | Set password · New password · **Cancel** **Set** |
| Set and change errors | Wrong password. · Use at least 8 characters. · Use letters, numbers and punctuation only. |

The sign-in is an alert like the page's other dialogs, alone on the page's background, and
nothing closes it but signing in (Wells, 2026-11-16, look A). The erase dialog keeps its words. No help lines in the dialogs and no confirmation after a change
(Wells, 2026-11-16: "you're adding too much UI copy"); resetting a forgotten password is the
wiki's. Changes from the mocks Wells saw: the change dialog asks for the current password, Sign
Out moved beside Restart…, and the forgot-password line and the This browser row went.

### The bench tool

`tools/bench/bench.py` (public; Python 3 in ESP-IDF's environment, for `espefuse`). Everything
private stays in `~/.config/airwriter/labels/`, never in a repo:

- `labels.csv`: one row per label: sheet, position, kind (full or LC), serial, password, date
  printed, and the chip's MAC once burned. It is backed up off the bench Mac after each sheet,
  so a lost label can be answered later.
- `art-full.png`, `art-lc.png`: each label's art at 300 dpi, black and white, with the serial and
  password cells blank, exported from Figma's label frames. They carry the postal address.
- `sheet.json`: the label sheet's layout (columns, rows, the first label's corner, pitch) and
  the printer's offset from the alignment print. `fields.json`: where the serial and password
  sit on a label, in mm, and their size.

Commands:

1. `bench.py align` prints a plain-paper page with every label's outline. Held over a label
   sheet against a window, it shows the offset to enter once.
2. `bench.py sheet --kind full|lc` makes a serial and password for every label on a new sheet,
   adds them to `labels.csv`, and prints the sheet. The serial is the sheet's year and ISO week,
   `AW`, `F` or `L`, the board revision (`2611`), and the next number in the running count of
   devices made. So a serial's week is the week its sheet was printed. The count (my calls) is
   one count for full and LC boards and every revision, starts at 001, and is given out when a
   sheet prints. A spoiled label is marked void in `labels.csv` and its number isn't reused. It
   is three digits until 999.
3. `bench.py burn --kind full|lc --port /dev/cu.usbmodemNNNN` reads the chip's user block. If
   it is burned, it says which label the board has and stops. If not, it takes the next unused
   label of that kind, shows its number, serial and password, burns them on Enter, reads them
   back, records the MAC, and says which label to stick on ("Label 7: 2652AWF2611-007").

A sheet goes through the printer once; labels left on it wait for the next boards. Flashing
firmware stays a separate step.

**The art.** The label isn't final yet (`W-1155`). Until it is, the tool is built and tested
end to end with stand-in art made from the label mock, cells blank. The real art is exported
from Figma with the serial and password layers hidden, once `W-1155` closes. The sheet's layout
comes from its maker's spec sheet.

**Printing the sheet.** The tool draws the whole sheet itself at 300 dpi in pure black and
white (the art, plus the serial and password in the label's typeface, Barlow Semi Condensed,
OFL), encodes it as URF and sends it as a Print-Job to an AirWriter sharing the Personal
LaserWriter 300 (`--printer ipp://…`). Letting the Mac draw it would smooth the edges into gray,
which the printer screens into dots; at the label's 4 pt small print that shows.

## What stays the same

Printing from phones, Macs and vintage Macs; the test button's test page; GitHub updates
checked and installed by the board; mDNS and AirPrint discovery; every board with nothing
burned in.

## Testing

- **Host.** The pure parts (eFuse block parse, token store, nonce book, lockout, proof check,
  the change's key stream) as a C core under `make -C tools/rip test`, with known-answer tests
  for PBKDF2 and HMAC shared with the page's script.
- **Page.** `preview.py` states for signed out, wrong password, too many tries, and About with
  and without `pw`, at phone and desktop width, light and dark.
- **Set Password.** A board with none burned sets one: its setup network asks for it, the page
  signs in with it, and an erase takes it off again.
- **Older firmware.** A burned board (or one with a test password) refuses an older signed image
  by `/update` and by tag, and takes a later one.
- **Bench tool.** The sheet's URF ripped with `rip --printer plw300` and decoded by
  `qdfake.py` comes back with exactly the sheet's black dots.
- **Board 1**, with a test password in its local sdkconfig: the setup network asks for it; the
  page signs in, signs out, changes it, locks for a minute after 5 wrong; an iPhone and a Mac
  print with no prompt; holding test through power-up brings back the test password; `curl`
  with `signin.py`'s cookie reaches `/api/status` and OTA.
- **A blank rev 2026.11 board**, burned once with `bench.py burn` from a test sheet printed on
  plain paper, before any label sheet.

## Docs to update in the same change

`docs/SPEC.md` (a new §3.20, and §3.2 and the webui section for the 401s), `docs/traps.md`
(eFuse burns can't be undone: never burn board 1 or a dev board; the custom-MAC bytes),
`AGENTS.md` (`signin.py` for `curl` on a burned board, the bench tool, the test password),
the wiki's setup and settings pages, the settings page text, and the shop's description, by
`docs/voice-and-tone.md`.

## Left to the builder

The C/JS split of the crypto, where the guard sits in `webui.c` (raise `max_uri_handlers` past
32), the version floor's comparison, the cookie parse, the exact NVS keys, how `signin.py` stores its cookie, `N` within the
second-on-a-phone limit, the URF encoder, and the sheet's offset handling.

## Risks and what isn't checked

- **Password strength.** 31⁸ is about 8.5 × 10¹¹. One RTX 4090 tries about 2.5 million WPA2
  passwords a second (hashcat's community benchmarks), so a recorded join of the setup network
  falls in about 4 days of that card's time; 10 characters would take about 10 years. Kept at 8
  (my call) because the setup network is up only during setup or after a failed join, and an
  attacker has to be in Wi-Fi range then. Revisit if Espressif's EN 18031 templates ask for more.
- **EN 18031.** That this design meets its access-control clauses is expected, not confirmed:
  the standard is paid, and Espressif's self-assessment templates are the way to confirm it (asked for
  only with Wells's OK).
- **The first set password** crosses the network readable once, as above.
- **Plain http.** The token travels in the clear, as on any http page: someone on the home
  network who records traffic can use a signed-in browser's token. The password itself stays
  private.
