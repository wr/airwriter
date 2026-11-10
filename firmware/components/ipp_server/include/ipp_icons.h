// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sets of printer icons ipp_server serves, tools/icon/mkicons.py's PNGs: X is
// expanded once for each set's name. ipp_server's CMakeLists.txt names the same list,
// to embed them; printers_core.c's pr_icon says which a printer shows.
#pragma once

#define ICON_SETS(X) X(icon) X(stylewriter) X(iwlq) X(sworiginal) X(sw2) X(sw1200) \
                     X(csw1500) X(csw2200) X(lw) X(lw2) X(plw) X(lw4600) X(lwselect) \
                     X(lwpro400) X(lwpro600) X(lwpro810) X(lw12640) X(lw8500) X(clw)
