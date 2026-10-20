# Settings page redesign (W-1079)

`W-1079 Settings page: organise by port, like macOS System Settings`. Mockups: the agreed one
is mockup 4 plus Wells's 2026-10-20 notes below; the first is kept for comparison.

## Why

The page was built as the printer's own front panel (W-1072): engraved keys with their
legends above them, everything on tractor paper, one italic condensed face. That worked with
two settings. With AirPrint, four AppleTalk links, updates and recovery it became a wall of
technical text that is hard to scan and, in that italic, hard to read.

## What it is now

A settings app on the macOS 27 pattern: a sidebar docked to the window's edges, a pane per
item, plain language, progressive disclosure, rows that repeat one shape so the eye learns
them once.

The sidebar is organised by what is on the outside of the box and what it does over Wi-Fi:

| Pane | Holds |
|---|---|
| **Printer** | Status (connected, ready or what's wrong, printing), ribbon; model, then name (blank means the model's name); halftone with its printed sample; Test Cable…, Print Test Page |
| **AirPrint** | On/off; the printer's IPP address with Copy; help link |
| **AppleTalk** | One card per way in: AppleTalk Port, Networked Macs, Basilisk II, Infinite Mac; "SheepShaver and Mini vMac are not supported. Learn More…"; help link |
| **Wi-Fi** | The network, connected, lock and signal glyphs, Change…; IP address; host name |
| **About** | The printer icon and name; version, up to date, Check Now, update automatically, install from a file; on for, last started; Erase All Settings…, Restart; DIP switches; links out; the Wells Workshop mark |

Each setting has one home. Power and Software Update are not panes: there is nothing to set
on power, and updates are about the box, so both live in About.

Each sidebar row shows its pane's state (Ready, On, 2 Macs, the network's name, Up to date),
so the whole box can be read without opening anything.

### Rows

- A pane opens with a hero: its icon, its name in Apple Garamond, one sentence and Learn
  More… (the wiki). Nothing else explains; a row says what it is by its label.
- Rows are label left, value or control right, in rounded groups.
- Status is a lamp: a short lit bar, like the ImageWriter's and the Snow White panel lights,
  green for fine, amber for attention, red for failed, unlit for off.
- AirPrint and AppleTalk end with a round help button to the wiki's troubleshooting page.
- On a big screen the page sits in a window on a darker desk; on a phone it fills the screen.
- The cable test shows the cover's port edge (serial, appletalk, power) with a cable from one
  port to the other: a pulse runs along it while it tests, then it turns green or red.
- The DIP switches are drawn as the part on the board: a red body, white sliders, white
  numbers. Each row names what a switch does, then its setting.

### AppleTalk cards

Wells's layout, from macOS's Wi-Fi network row:

```
[icon] Interface                          (switch)
       Short description            [Set Up…]
--------------------------------------------------
1 Mac connected
[mac] Name or address                 detail
```

- AppleTalk Port: mini-DIN 8 icon. Networked Macs (not "Ethernet": AirWriter has none):
  Ethernet socket icon. Basilisk II: its own icon. Infinite Mac: its rainbow infinity.
- Basilisk II and Infinite Mac have Set Up… sheets with the steps. Infinite Mac's sheet holds
  the zone name (Copy, Open Infinite Mac, New Name).
- Infinite Mac is off until someone turns it on. Its zone name is made up (adjective, noun,
  four digits) until someone types their own: anyone who opens Infinite Mac with the name can
  print, and Infinite Mac lists no zones, so the name is the only lock. Infinite Mac itself has
  no default; its zone field starts blank.
- "Connected" means heard from in the last 10 minutes. AppleTalk has no connection to watch,
  and an idle Mac says little, so a Mac drops off the list after a quiet spell and comes back
  when it prints or opens the Chooser.

### Look

Platinum neutrals like the ImageWriter's case, with the Platinum theme's dark gray as the
accent (links carry an underline since they are no longer blue). Upright system type for
everything people read; Apple Garamond Light, the face on the printer's badge, for pane titles
and the name. Icon tiles in the colour ribbon's inks and the colours the printer mixes from
them; red is left for failures, as on the panel, and Wi-Fi and updates keep the panel's sky
blue and pink. Buttons are low, platinum keys with a moulded edge that travel a pixel when
pressed: a nod to the old keys, not the whole mechanism. The halftone sample stays on tractor
paper, one screen pixel per printer dot, with its sprocket holes. Light and dark.

## What the firmware gives the page

Existing routes stay as they were; W-1079 adds:

- `/api/status`: `pmodel`, `pname`, `phost` (what the page last saved; a name or host name
  reaches mDNS and the Chooser only after a restart, so the page says so), `airprint`,
  `lt_port` (1 on, 0 off, -1 not set: DIP switches 1-2 decide, mode 2 on), `lt` (the port's
  node running and not quiet), `dip` (switches 1-8 as "0"/"1", "" before the first read).
  User strings in it (network, printer and job names) are now escaped.
- `POST /api/general` takes `model`, `name` (present and empty: back to the model's name),
  `airprint` and `localtalk` (true/false) beside `host`, `queue`, `dither`, `gain`. The model
  and name are stored apart (NVS `model`, `name`); `queue` keeps the name in effect for older
  firmware. A board from before reads a stored queue that is one of the three models as that
  model, and anything else as a name on an ImageWriter II.
- `POST /api/restart` (lets a printing page finish first), `/api/erase` (as holding the test
  button 8 s), `/api/test` (the test page; 409 while the printer is busy).
- `GET /api/lt` and each `/api/et` link gain `macs`: the nodes heard in the last 10 minutes,
  newest first. The cable's carry their node; an EtherTalk link's carry their address and MAC,
  and a Basilisk II peer's its computer's IP (its MAC is `B2` and that IP).

**AirPrint off** takes `_ipp` (with `_universal`) and `_printer` out of Bonjour, keeps `_http`
for the page, and answers Print-Job and Validate-Job with not-accepting-jobs; on puts them
back under the same printer-uuid. Live, no restart.

**The AppleTalk port's switch** replaces DIP switches 1-2 once used: the node cannot stop once
started (it keeps UART2 and its task), so off is quiet, with TashTalk answering for no node,
frames dropped and a job cut short, and on acquires a node again. Until the switch is used, DIP
mode 2 starts the port as before.

## Not in this change

- Mac names from NBP. AirWriter shows each Mac's AppleTalk address; System 7 and later should
  answer a lookup of `=:Workstation@*` with the Macintosh name, which needs a bench check.
- The captive setup page keeps its own short form.
