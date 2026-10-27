<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# StyleWriter tools

Two tools for AirWriter's StyleWriter support (W-1103). `swprint` prints a page to a real
StyleWriter from a computer and logs the conversation. `swfake` is a StyleWriter in software,
for trying the support without a printer.

## swprint

`swprint` prints a page to a StyleWriter with no AirWriter in between and logs everything
sent and received. Run it on your StyleWriter and send the log back. The log shows whether
your model works with AirWriter's StyleWriter support.

### You need

- A StyleWriter (the original, II, 1200, or Color 1500, 2200, 2400 or 2500) with paper and a
  cartridge, switched on.
- A USB serial adapter with RS-422, and the cable that connects the printer to a Mac's printer
  port. The line is 57600 baud, 8N1, no flow control.
- A Mac or Linux computer with a C compiler (on a Mac, the Xcode command line tools) and zlib.
- A page, as a PNG or a URF file. Make it the size of the sheet at 360 dots an inch: 3060 x 3960
  pixels for Letter, 2976 x 4209 for A4, 3060 x 5040 for Legal. The printer cannot print to
  the edges, so keep anything that matters clear of the outer 0.3 inch. A PDF turns into a
  page with `pdftoppm -r 360 -png -singlefile page.pdf page`. A PNG with no resolution in it
  prints one pixel to a dot, and `--dpi` sets another.

### Print

```sh
make -C tools/rip build/swprint
ls /dev/cu.usbserial-*                       # Linux: /dev/ttyUSB0
tools/rip/build/swprint --dev /dev/cu.usbserial-XXXX --log sw.log page.png
```

Several pages in a row print as one job. The progress shows on screen:

```
swprint: page.png: 3060 x 3960, 360 dpi, gray
swprint: found: Color StyleWriter 2200
swprint: printer: printing
swprint: cartridge: colour
swprint: page 1 of 1 (2916 x 3960)
swprint: printer: ready
swprint: printed 1 page
```

If the printer needs attention, such as paper or a closed door, the screen shows it
(`printer: out of paper`) and the job carries on once it is put right, for up to 10 minutes.
**Ctrl-C** cancels. The block being sent finishes, and the printer is reset.

| Option | Use |
|---|---|
| `--ask` | The model is read from the printer's reply to the plain question, the one byte `?`. The default. |
| `--model M` | Name the model: `original`, `ii`, `1200`, `1500`, `2200`, `2400` or `2500`. The job begins by resetting the printer. |
| `--quality 3\|4\|5` | Draft, normal (the default) or best. |
| `--log FILE` | The log. |
| `--dither NAME` | The screen: `atkinson` (the default), `floyd`, `bayer`, `bluenoise`, `threshold` or `apple`. |
| `--dpi N` | The resolution of a PNG that has none (360). |

A colour page prints in colour on a colour model with a colour cartridge, and as gray on
any other.

### If it does not print

| Screen | Fix |
|---|---|
| `no answer to the plain question` | Check the power, the cable and the adapter. The original StyleWriter does not answer the question: use `--model original`. A printer reset in the last 2 seconds needs a moment: run it again. |
| `printer stopped answering` | Nothing came back for 30 seconds. Check the power and the cable. |
| `the printer answers as another model than the one named` | Run again with `--ask`, or name the right model. |
| `... is neither a PNG nor a URF page` | The file is not a page `swprint` reads. A PNG must not be interlaced. |

### Send back the log

Open an issue at github.com/wr/airwriter and attach `sw.log`. Say which model it is, whether
the cartridge is black or colour, what the adapter and cable are, and what came out on
paper (a photo of the page helps). The log has the bytes sent and received, with long page
data cut after 32 bytes, and the name of your computer's operating system.

## swfake

A StyleWriter in software (W-1103), for trying AirWriter's StyleWriter support
without a printer. It answers on a serial line the way Apple's seven StyleWriters
do, takes pages, checks them, and draws what would have come out. Nothing prints.

The seven are `original`, `ii`, `1200`, `1500`, `2200`, `2400` and `2500`. What it
knows comes from the design
(`docs/superpowers/specs/2026-10-25-stylewriters-design.md`, "What the printer and
the board allow"), which is the drivers' reading and other people's notes, not a
real printer. Its page decoder is written from the wire format's rules and shares
no code with the firmware's, so a page that comes back right is a page two readings
agree on.

### Run it

```sh
python3.14 tools/stylewriter/swfake.py serve 2500 --pty --cart colour --out /tmp/sw
```

The first line it prints is the path of a pseudo-terminal, like `/dev/ttys004`. Open
that as a serial port at 57600, 8N1, no flow control, and talk to it. Pages go to
`/tmp/sw/page-N.png` (ink on paper at 360 dots an inch, the whole sheet with its
margins) and `/tmp/sw/page-N-{c,m,y,k}.pbm` (the printable area, 1 is ink).

Other ways to reach it: `--serial DEV` opens a real serial device, `--tcp HOST:PORT`
connects out to something listening, `--listen PORT` waits on 127.0.0.1 (0 picks a
port and says which). `--media letter|a4|legal` sets the sheet, `--drain BYTES_PER_S`
how fast the printer eats its buffer (32768), `--log FILE` writes every byte both
ways with milliseconds, `--no-png` skips the pictures.

It ends on Ctrl-C, or when its TCP or serial peer goes away (`--listen` keeps waiting for the next one).

Type on its stdin to break it: `paper`, `jam`, `ink`, `door`, `waste`, `cart`
(missing; `cart black` or `cart colour` puts one in), `busy 20`, `silent` (as if
unplugged), `overflow`, and `ok` to put everything right. A fault it does not mean
to forgive, such as a bad run code, a rect past the printable width or more bytes
than its buffer holds, makes it eject the page and go deaf until the reset
(`00 FF FF FF 49`, then 2 seconds), as the design says a real one does; it says why
on stdout and in `--log`.

### Against swhost

`tools/rip/build/swhost` (`make -C tools/rip build/swhost`) is the firmware's own
session, page stage and rip built for this computer. `swprint` above is the same program
under another name (`build/swprint` is a link to it). It prints pages to a serial device,
so the fake's pseudo-terminal does:

```sh
python3.14 tools/stylewriter/swfake.py serve 2200 --pty --out /tmp/sw    # prints /dev/ttysNNN
tools/rip/build/swhost --dev /dev/ttysNNN --ask fixtures/checker.urf
```

`make -C tools/rip swinterop` (`test_swinterop.py`) does this for all seven models, a gray
page and a colour one each, and then breaks the printer: paper out and back, silence, and a
cancel in the middle of a band. It checks that the pages swfake drew are the dots
`rip --printer stylewriter` made of the same pages, with PNG pages and bad ones as well.
The silence case waits out the real 30 seconds, so the run takes about 35 seconds. swfake is
the printer's side only; it never asks anything of the host.

### Against a board

Wire the board's J2 to a USB serial adapter that speaks RS-422 at 57600 8N1, with
the cable a StyleWriter would have, and give its device to `--serial`:

```sh
python3.14 tools/stylewriter/swfake.py serve 2500 --serial /dev/cu.usbserial-XXXX --cart colour
```

Set the board's port to a StyleWriter and print from a phone. This has not been
tried yet.

### Against Apple's drivers

`capture.md` is the plan for running Apple's own StyleWriter drivers in an emulated Mac whose
printer port is this fake's pseudo-terminal, and `make -C tools/rip swcaptures` holds what
they send to what the firmware sends. Not run yet: the rig needs a 68k Mac ROM.

### Decode a stream

```sh
python3.14 tools/stylewriter/swfake.py decode 2200 stream.bin --out /tmp/sw [--media a4] [--no-png]
```

Reads the bytes a printer would be sent (a ripped page, or the `>` side of a
capture), with no waiting and no buffer limit, and writes the same files. Exit 1 and
the reason if the stream is bad. It is how the firmware's pages are compared with an
independent reading of the format: `rip --printer stylewriter --model 2200 --out-sw
stream.bin` writes a page as the printer would be sent it, and `make -C tools/rip
swcheck` decodes it here, and the same dots coded with lpstyl's coder, and holds both
to the dots the rip made.

### Tests

```sh
python3.14 -m unittest tools/stylewriter/test_swfake.py
python3.14 -m unittest tools/stylewriter/test_plain_question.py
```

The first runs on a clock of its own, so the 2 s settle costs nothing. The second is
a model of a 9600 baud receiver (16 samples a bit, start bit checked again at its
middle) hearing the single `?` a StyleWriter is asked at 57600: it hears no byte at
any of 64 phases. It also prints what the same receiver makes of the full sequence
`00 FF FF FF 49`, which the design sends only to a printer someone named.

### What it guesses

The design fixes the queries, replies, buffer numbers and geometry. It leaves a few
things open, and swfake picks:

- `D` and `N` take no answer (the session sends them as bare letters, as Apple's drivers
  do, and swfake takes them after `FF FF FF` too), `R` answers `00`, and `H` answers
  without a `D` before it.
- Status is busy while a cleaning, `busy N` or the end of a page is going on, and
  `08` once the page has been eaten; it is not busy while a band is being eaten.
- A fault while a page is in the printer holds the page until `S`, not just until
  the paper is back.
- Rects must be inside a band (400 or 800 rows) and get every row before the next.
- The 2200 reads `A2` when empty (it may read `A3`), so its buffer is 39424 bytes;
  the 2500's is 30720, a little under the 0x79FA its driver sends in one block.
