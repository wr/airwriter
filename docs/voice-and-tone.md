# Voice and tone

How AirWriter writes, wherever a person reads it: the settings page, the wiki, the README, the
shop and forum posts. Read this before writing any of them.

## Readers

People with a vintage Apple printer, and sometimes a vintage Mac. A few know LocalTalk from
EtherTalk. Most just want the ImageWriter from the swap meet to print a photo from their phone.
Write for them. The rest goes in the engineering docs.

## Four voices

| Where | Voice |
|---|---|
| Settings page, setup, notices, status names | Product |
| Wiki | Docs |
| README, shop, forums, Hackaday | Wells |
| AGENTS.md, specs, plans, commits, Linear | Engineering |

**Product.** As short as it can be. Labels over sentences, and one line of help at most.

- Buttons: Title Case, starting with a verb. An ellipsis when the button opens a sheet or asks
  for more (**Add a Printer…**, **Remove Printer…**). None when it acts at once (**Print Test
  Page**, **Check Now**).
- Notices only for something that needs a decision. A title that names the problem, one line
  on what to do, and buttons for the choice: "Unrecognized printer on the printer port" / "If
  it's an ImageWriter, choose its model." / **Choose Model…** **Dismiss**.
- Status names stay the same on the page, the phone and the wiki: Ready, Printing, Needs
  attention, Offline, The last job failed.

**Docs.** What the reader does and what they see. Numbered steps for procedures, tables for
lookups, problem then fix in troubleshooting. Anything on screen goes in bold, spelled exactly as
it appears. Addresses, file names and commands go in code.

**Wells.** First person and casual, like the shop and the TinkerDifferent thread. Mixed sentence
lengths, the odd "(me!)" or ":)", and a request for testers. Few headings and no bold lead-ins.
Only the technical details you'd tell a friend.

**Engineering.** Precise and complete. Internals, timings, test status and known issues all
belong here, and only here. Name Linear issues by ID and title. AGENTS.md is current state
only. A commit message gives the reason for the change.

## Principles

These apply to the product, docs and Wells voices.

1. **The reader's side.** Describe what people do and see, not what AirWriter does inside.
   "Connect the printer and switch it on. It shows up in **Printers**." Not "AirWriter
   identifies the printer on its port at startup."
2. **Objects don't think or talk.** AirWriter, the page and the printer don't speak, ask, tell,
   say, report, hold, decide, know, find or search.
3. **Cut what the reader can't use.** Mechanics, timings, internal names, edge cases, and scope
   negatives the reader didn't ask about ("StyleWriters aren't supported" under a table of
   supported printers).
4. **Confident.** No "untested", "should work", "never tried" or "this part is new". Test
   status belongs in Linear and the engineering docs. One exception: the wiki's Supported
   Printers table marks a printer not yet confirmed on paper **Yes\***, with a footnote asking
   owners to report it. Drop the star once one prints.
5. **Labels where a label will do.** Headings, table headers and list leads: "Port", not "Where
   it is".
6. **Plain and short.** One idea per sentence, with no lists packed into a sentence behind a
   colon.

## Words

| Use | Not |
|---|---|
| AirWriter | the AirWriter, the board, the dongle |
| phones and computers | iPhone, iPad and Mac (except in per-platform steps) |
| vintage Macs | old Macs |
| connection, or the card's name | way in |
| printer port, AppleTalk port | J2, J4, LocalTalk port, serial port |
| test button | test dot |
| settings page | web UI, dashboard, the page |
| switch on, switch off | power on, power up |
| ImageWriter, LaserWriter | IW, LW |

## Mechanics

- **American English:** color, gray, canceled, center.
- **No em dashes.** Use commas, periods or parentheses.
- **A real ellipsis** (…), not three periods. Curly quotes and apostrophes in the page.
- **Numbers:** digits for counts and measurements (4 printers, 300 dpi, 8 seconds), and "about
  a minute" for waits.
- **Case:** buttons and pane names in Title Case. Help text and notice lines in sentence case.

## Tells to cut

- A clause ending on a stranded word: "what it is", "print to", "send them".
- Personified objects (see principle 2), and figures like "drawn as" and "never as".
- Colon reveals, triplets, and "not X, but Y".
- Hedges and fillers: should, may, just, simply, basically.
- An aside that makes a general action sound narrow: "set the model of one AirWriter couldn't
  identify" for "set a printer's model".
- Saying the obvious: "One page prints at a time."

## Before and after

| Before | After |
|---|---|
| AirWriter identifies the printer on its port at startup, whenever the printer comes back… | Connect the printer and switch it on. It shows up in **Printers**. |
| The Printers section of the settings page holds the list | Your printers are listed under **Printers** on the settings page. |
| Set the model of one AirWriter couldn't identify | Set a printer's model. |
| Pages print on the paper size the printer reports. | Pages print on the paper in the LaserWriter's tray. |
| Something on the printer port didn't say what it is. | Unrecognized printer on the printer port |
| ImageWriter LQ: should work, untested | ImageWriter LQ: Yes |
| Where it is | Port |
| One page prints at a time. | (cut) |

## Before publishing

Read it as someone with an ImageWriter and an iPhone who has never heard of AppleTalk. Then search
the text for: `speaks`, `asks`, `tells`, `says`, `reports`, `untested`, `old Mac`, `iPhone, iPad`,
`way in`, `—`, and lines ending in ` is.`, ` to.` or ` them.`
