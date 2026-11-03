"""Where the top case's front copper opens. Pure geometry; emit_board.py draws it.

The back flood (emit_board.back_copper_flood) lets light into the FR4 behind
each lit dot, and FR4 spreads it sideways; black mask passes some of it, so
without copper on the front a halo leaks out round each dot, and a teal dot
under mask has no edge at all. A front flood under the mask, open only where
light should leave, puts the dot's edge in the copper.
"""

FLOOD_INSET = 0.55          # from the outline: the board's copper-to-edge rule, as the back flood
P_GROW = 0.10               # bare-FR4 dots: copper wider than the mask opening, so
                            # misregistration never uncovers a sliver of copper
SLASH_GROW = 0.05           # the power light's slash, likewise (its mask is open)


def front_windows(dots, grid, fr4):
    """The front flood's holes: a circle per lit dot (teal B/L exactly the dot,
    bare-FR4 P the dot plus P_GROW), and each FR4 slash grown by SLASH_GROW.
    Gold and test dots stay copper."""
    dia = grid["dot_dia"]
    windows = []
    for d in dots:
        if d["mat"] in ("B", "L"):
            windows.append(dict(kind="circle", x=d["x"], y=d["y"], dia=dia))
        elif d["mat"] == "P":
            windows.append(dict(kind="circle", x=d["x"], y=d["y"], dia=dia + P_GROW))
    for slash in fr4:
        windows.append(dict(kind="poly", outline=slash["outline"], grow=SLASH_GROW))
    return windows
