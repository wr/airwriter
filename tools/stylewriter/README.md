<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# swfake

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

## Run it

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

## Against swhost

`tools/rip/build/swhost` (`make -C tools/rip build/swhost`) is the firmware's own
StyleWriter code built for this computer. It prints a page to a serial device, so the
fake's pseudo-terminal does:

```sh
python3.14 tools/stylewriter/swfake.py serve 2200 --pty --out /tmp/sw    # prints /dev/ttysNNN
tools/rip/build/swhost --dev /dev/ttysNNN --ask fixtures/checker.urf
```

`make -C tools/rip swinterop` does this for all seven models. swfake is the
printer's side only; it never asks anything of the host.

## Against a board

Wire the board's J2 to a USB serial adapter that speaks RS-422 at 57600 8N1, with
the cable a StyleWriter would have, and give its device to `--serial`:

```sh
python3.14 tools/stylewriter/swfake.py serve 2500 --serial /dev/cu.usbserial-XXXX --cart colour
```

Set the board's port to a StyleWriter and print from a phone. This has not been
tried yet.

## Decode a stream

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

## Tests

```sh
python3.14 -m unittest tools/stylewriter/test_swfake.py
python3.14 -m unittest tools/stylewriter/test_plain_question.py
```

The first runs on a clock of its own, so the 2 s settle costs nothing. The second is
a model of a 9600 baud receiver (16 samples a bit, start bit checked again at its
middle) hearing the single `?` a StyleWriter is asked at 57600: it hears no byte at
any of 64 phases. It also prints what the same receiver makes of the full sequence
`00 FF FF FF 49`, which the design sends only to a printer someone named.

## What it guesses

The design fixes the queries, replies, buffer numbers and geometry. It leaves a few
things open, and swfake picks:

- `D` and `N` after `FF FF FF` take no answer, `R` answers `00`, and `H` answers
  without a `D` before it.
- Status is busy while a cleaning, `busy N` or the end of a page is going on, and
  `08` once the page has been eaten; it is not busy while a band is being eaten.
- A fault while a page is in the printer holds the page until `S`, not just until
  the paper is back.
- Rects must be inside a band (400 or 800 rows) and get every row before the next.
- The 2200 reads `A2` when empty (it may read `A3`), so its buffer is 39424 bytes;
  the 2500's is 30720, a little under the 0x79FA its driver sends in one block.
