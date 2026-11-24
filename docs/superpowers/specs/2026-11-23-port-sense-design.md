# Port sense on rev 2026.11

`W-1185 Rev 2026.11: sense each port's receive pair, so a switched-on printer that isn't ready
is seen`. Started with Wells on 2026-11-23, after measuring his printers' cables.

## Why

On board 1, AppleTalk frames sent into an ImageWriter II printed three sheets of junk, and a
deselected ImageWriter keeps them and prints them when SELECT is pressed. So `W-1179 Rev
2026.11: plug anything into either socket and AirWriter works out what it is` sends AppleTalk on
a socket only after it has heard LocalTalk there. That costs a LaserWriter cabled into a fresh
socket its discovery, until it's switched off and on or a Mac talks. It also still sends into a
deselected printer plugged into a socket that has already heard LocalTalk.

Each socket needs to tell a printer that has power from anything AppleTalk could safely be sent
to.

## What the cables show

Wells measured each device at the AirWriter end of its cable, unplugged from AirWriter, against
pin 4 (the tables are in W-1185):

| | Pin 8, RxD+ | Pin 5, RxD− | Pin 2, HSKi |
|---|---|---|---|
| ImageWriter II, selected or not | +4.4 V | −4.9 V | ±4.8 V with SELECT |
| Color StyleWriter, on or in standby | +4.6 V | ≈ 0 V | 0 V |
| Personal LaserWriter 300, idle | +4.7 V | −4.5 V | 0 V |
| LaserWriter NTR, LocalTalk, direct cable | +0.08 V | +0.12 V | +0.12 V |
| Any of them unpowered | 0 V | 0 V | 0 V |

A serial printer with power holds its transmitter at idle mark: pin 8 up, and on two of the
three pin 5 down. A LocalTalk device turns its transmitter off between frames. HSKi moves on
the ImageWriter alone. An RS-232 printer on a Mac cable (the original ImageWriter) has its TxD
on pin 5, at −5 to −12 V at idle, and pin 8 grounded by the cable. So the sense watches both
receive pins, not HSKi.

## The circuit

Per socket, a dual NPN (BC847BDW1, SOT-363) and five 0402 resistors, the taps on the connector
side of the 15 k receive legs, so the receive path itself is unchanged:

- **Pin 8 high.** Pin 8 → 22 k → Qa's base, 6.8 k base to GND, emitter to GND, collector to
  the sense line. Qa turns on above about +2.5 V on pin 8.
- **Pin 5 low.** Pin 5 → 15 k → Qb's emitter, 6.8 k emitter to GND, base to GND, collector to
  the sense line. Qb starts to conduct below about −1.9 V on pin 5 and holds the line low below
  about −3.5 V (−2.8 V with a weak pull-up). At −4.5 V it sinks 160 to 200 µA.
- **The sense line**, the two collectors joined, goes through 4.7 k to a spare GPIO with its
  internal pull-up on (30 to 60 kΩ, at most about 110 µA): low means a device's transmitter is
  at idle mark. Qa pulls the GPIO to +0.5 V at most, under the ESP32's 0.82 V input low. Qb's
  collector follows its emitter below ground, to about −0.6 V, so when Qb pulls alone (an RS-232
  printer, whose cable grounds pin 8) the GPIO sits between 0 and −0.35 V with its pull-up on,
  about the ESP32's −0.3 V limit. With the pull-up off (at reset, before the firmware sets the
  pin up) the pad's protection diode holds it near −0.4 V, and the 4.7 k keeps that current
  under 0.1 mA. A larger resistor would lift Qa's low past 0.82 V. J2's (Port 1) is IO39
  and J4's (Port 2) IO38, rev 2026.11's spare pads, whose test pads TP11 and TP10 stay on the
  GPIO side. IO39 leaves reset as JTAG's MTCK: the firmware sets it to a GPIO before reading.

The parts are Q1 and R47–R51 on J2, and Q2 and R52–R56 on J4. The LC has one socket and no
TashTalk, and never sends AppleTalk, so it leaves them off.

Checks on the numbers:

- **Empty socket.** The THVD1452 biases its own inputs; TI doesn't give the open-circuit level,
  and its input-current limits (1/8 unit load) allow anything from about 0 to 3.3 V through
  roughly 100 k or more, plus the 15 k leg. The divider's 28.8 k to GND holds pin 8 under
  about 0.8 V at worst, so Qa's base stays under 0.2 V. Pin 5 sits positive, so Qb is off.
- **Fail-safe.** Pin 5 carries the heavier load (21.8 k against pin 8's 28.8 k), so on an
  empty socket the receiver's B input sits a little below A: idle mark, as now, never a
  break. Pin 8 goes to A and pin 5 to B on both sockets.
- **Reverse bias.** At ±15 V on a pin (the THVD1452's range, an RS-232 device), Qa's base
  sees at most −3.5 V and Qb's emitter +4.7 V, under the transistors' 6 V emitter-base
  rating. No clamp diodes.
- **Load on the line.** 28.8 k and 21.8 k to GND, against the RS-422 and RS-232 drivers' 100 Ω
  to 3 kΩ: nothing a printer or a LocalTalk node notices.
- **AirWriter unpowered.** The sense is open-collector, so a printer that's on can't power the
  ESP32 through it.
- **LocalTalk traffic.** A frame on a direct cable swings the pair, so the sense pulses while a
  node sends. A frame lasts at most about 21 ms at 230.4 kbit/s, and the line rests between
  frames, but the frames of one exchange are only about 200 µs apart: polling every few
  milliseconds could miss the gaps and take back-to-back frames for one long low. So the
  firmware watches for the line going high (an interrupt) and takes the sense as held only
  after it has stayed low, with no high at all, for longer than a frame.

## On the board

J2's transistor and four resistors sit west of the socket, between its body and the board edge,
and J4's in the gap between the two sockets' bodies, its transistor just above. Each taps its
socket's pins 8 and 5 on the back. The 4.7 k resistors sit by the module's IO39 and IO38
vias, and each socket's collector line crosses the back to its resistor through the one gap
past J4's handshake runs, J4's on the north side and J2's on the south. That's why J2 takes
IO39 and J4 IO38: the other way round the lines would cross. `tools_pcb/r16_sense.py` draws
it all; `docs/hardware-rev-2026-11.md` has the details.

## Parts

BC847BDW1T1G (onsemi, LCSC C82368; Nexperia's BC847BS is rated VEBO 5 V, too low here); 0402
UNI-ROYAL resistors 22 k C25768, 15 k C25756, 6.8 k C25917, 4.7 k C25900. All in stock at LCSC
on 2026-11-23.

## Review

Checked against TI's THVD1452 and onsemi's BC847BDW1 datasheets on 2026-11-23: Qa switches
between about +1.8 V (hot, a low-VBE part) and +2.9 V (cold, a high-VBE part), clear of an empty
socket's 0.8 V and the printers' 4.4 V; an empty socket keeps Qa's base under 0.2 V, under 1 nA
of collector current; the loads leave the receiver's fail-safe at +25 to +120 mV, mark; reverse
stress stays inside 6 V at ±15 V. The below-ground collector is why the 4.7 k is there. No
simpler circuit turned up: one transistor across the pair misses the StyleWriter.

No Schottky diode on the sense lines. The ESP32-S3's −0.3 V is the bottom of its input-low
range, not a rating: its datasheet sets no voltage limit on an I/O pin, and its latch-up test
drives 200 mA into one, against under 0.1 mA here. The pin still reads low, which is the right
answer, and only an RS-232 printer gets it there. A diode would add two parts in the board's
most crowded corner for no gain.

## Firmware

Per socket:

- **Sense high, held** (an empty socket, a LocalTalk box or device between frames, a printer
  with no power): the socket may send AppleTalk.
- **Sense low, held:** a powered serial printer, or a Mac on a direct cable. Never send
  AppleTalk there, and treat it as a printer port.

This relaxes W-1179's listen-first rule. It's W-1179's code to change, after the board.

## Left open

- Loaded readings on a built board, and the 300's pins while it clocks for a host.
- Whether an Apple LocalTalk box joins the transmit and receive pairs, so AirWriter's own
  frames pulse the sense while it sends. The firmware should read the sense only while the
  socket is quiet.
- A Mac on a direct cable reads as a printer. AirWriter then asks it the printer question,
  which a Mac ignores.
