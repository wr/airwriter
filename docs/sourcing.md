# Sourcing

Where parts and boards come from, and decisions already made so nobody re-does them. The
current BOMs are `hardware/combined/fab/combined-assembly-bom.csv` (AirWriter) and
`hardware/combined/fab-lc/airwriter-lc-assembly-bom.csv` (AirWriter LC); order notes for PCBWay are in
`docs/ORDER-NOTES.md`. Edit in place.

## Standing rules

- **Fab is PCBWay** (assembly too). Quote to Wells before any order; nothing is paid until he has
  seen the quote. Pay only through the PCBWay site, never to bank details in an email signature.
- **Price against LCSC, not Digi-Key or Mouser.** The gap changes design decisions (2026-08-05:
  MAX3232 $9.98 at Mouser vs $0.91 at LCSC; MAX3490 $6.61 at Digi-Key vs $0.687 at LCSC).
- **Order spec:** 2-layer, 1.6 mm FR4, 1 oz, matte black mask, ENIG. Finish and mask colour are
  in the board stackup, so renders and STEP show what arrives and the cart can't forget them.
- **ENIG is not vanity.** J1 is 0.5 mm pitch, which HASL's uneven surface doesn't reliably
  serve; the gold dots are the art. Reckon about +$25 an order.
- **Keep the standard tier:** drill ≥ 0.30 mm (0.20 is an upgrade; one documented case went from
  $5 to $65 for ten boards) and clearance ≥ 0.15 mm (6 mil). See `docs/traps.md`.
- **Passives are generic 0603/0805**; the BOM carries an LCSC number on every line.

## Not at LCSC

- **J2, J4: Kycon KMDGX-8S-BS, from Mouser** (Digi-Key is the backordered channel). **Wells
  buys and solders them after delivery** (Wells, 2026-10-29): J2 on every board, J4 on the
  full AirWriter only. Buy a few spares; it's a through-hole part you can damage. PCBWay
  could fit them, but its instant quote (50 boards) put fitting J2, J4 and the PIC at $0.73 a
  board on AirWriter and J2 at $0.26 on the LC, and the ~25% import duty on those parts'
  value at about $1.63 and $0.63 more. Alternatives checked and rejected:
  - Kycon KMDGX-8S-AS: "Board Guide, Board Lock" push-pull variant, different mounting; Digi-Key
    doesn't list it as a substitute.
  - Adam Tech MDJ-008-FS: a genuine drop-in (all eight signals and the ±6.75 posts match), out
    of stock at Digi-Key. **The fallback if Kycon dries up.**
  - Same Sky (ex-CUI) MD-80SM: rectangular shield slots and a 2.5 × 1.0 mm earth blade where the
    footprint has a Ø2.3 hole; won't fit. MD-80S is worse (unshielded, no posts).
  - HOOYA / Legion DIN-802 (LCSC C20611694 / C41409494): pads 0.15–0.20 mm out and an oval
    centre slot; unusable without a new footprint.
- **Standoffs: Würth 9774030151R (WA-SMSI M2.5 × 3)**, four per panel (AirWriter only). Not on LCSC; PCBWay
  sourced them for rev A ($0.48 each). Consign from Digi-Key only as the fallback.
- **Status LEDs: OPSCO SK6812MINI-012 (LCSC C2886570)** from rev 2026.11. Rev A's
  SK6812MINI-C (C7423117) is discontinued at LCSC (15 left on 2026-11-19). The -012 is the
  same package and pinout; the SK6812MINI-HS is not (`docs/hardware-rev-2026-11.md`).

## The TashTalk PICs and the port transceivers (W-1176)

- **U7 and U10: PIC12F1840-I/SN (SOIC-8, LCSC C39594)**, each socket's TashTalk on rev 2026.11,
  placed blank by PCBWay on AirWriter and left off the LC. LCSC stocks only the SOIC; rev A's
  U7 was the DIP in a socket, which Wells bought from Digi-Key and pressed in. Two a board:
  LCSC had only 173 on 2026-11-20 (the reel part, C512104, none), about 86 boards' worth.
  Check stock before the order, and buy the batch's worth early if it is short.
- **U2, U6: TI THVD1452DGSR (VSSOP-10, LCSC C2879202)**, one per socket, $1.47 at 50 and 2,248
  in stock on 2026-11-20. It replaced the THVD1451 ($3.09) on J2 and two SP3485s on J4.

## Programming the PIC

The ESP32 flashes TashTalk into U7 over LVP (`pic_prog`,
`W-1028 Firmware: flash TashTalk into the PIC from the ESP32 over LVP`), so no external
programmer is needed. U10 on rev 2026.11 is wired the same way (IO42 to MCLR, IO11 and IO12
through 100 Ω to ICSPCLK and ICSPDAT); flashing it waits on the firmware that lets J2 run
LocalTalk. An erased PIC12F1840 has LVP on, so a blank part straight from the
distributor takes TashTalk on the board: buy them blank, never pre-programmed. Proved on board 1
(2026-10-29): a new blank part took TashTalk in its socket. Upstream TashTalk's config clears LVP, so a stock-programmed part needs
high-voltage programming, never on the board (`docs/traps.md`): on rev A that means out of its
socket, and on rev 2026.11, where both are soldered, a new part. For rev A: a
PICkit 3 clone driven by PICkitminus (kair.us, macOS CLI, lists the 12F1840), PICkit 5 as
fallback. The MPLAB Snap is LVP-only, and the TL866II does not support the 12F1840 at all.

## Notes

- Full-duplex RS-422 transceivers keep going out of stock at LCSC (ISL3177EIBZ, MAX3491ESD+
  were both zero); half-duplex RS-485 is commodity. That is why rev A's LocalTalk uses two
  SP3485s. Rev 2026.11's THVD1452 is full duplex too: check its stock before each order, and
  the fallback is rev A's pair of SP3485s per socket, which means a new layout.
- The printer cable is Apple's 590-0552-A / M0197 crossover; see SPEC §2.4 before buying any other.
