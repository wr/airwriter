#!/bin/sh
# The one supported way to build the enclosure. Everything board-derived is
# re-read from hardware/airwriter/airwriter.kicad_pcb on every run.
set -e
cd "$(dirname "$0")"
PY=.venv/bin/python
[ -x "$PY" ] || { echo "run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2; exit 1; }
"$PY" -m unittest discover -p 'test_*.py' -v
"$PY" export.py
