#!/usr/bin/env python3
"""Point a Schwung patch's TapeDelay slots at Tape Echo 2.

Tape Echo 2 understands TapeDelay's saved settings, but nothing hands them
over on its own: a patch names the module it wants ("type": "tapedelay") and
the chain host loads exactly that one, so an old patch keeps opening TapeDelay.

This rewrites the name and leaves the settings alone. The module translates
them on load — delay time picks the head that can reach it, feedback, mix,
tone, division and stereo width carry across.

    python3 tools/convert_tapedelay_patch.py MyPatch.json [more.json ...]
    python3 tools/convert_tapedelay_patch.py --in-place MyPatch.json

Without --in-place it writes <name>-tape-echo2.json and leaves the original
untouched.
"""
import json
import pathlib
import sys

OLD, NEW = "tapedelay", "tape-echo2"


def convert(doc):
    """Returns how many FX slots were repointed."""
    n = 0
    for fx in doc.get("chain", {}).get("audio_fx", []) or []:
        if fx.get("type") == OLD or fx.get("module") == OLD:
            if fx.get("type") == OLD:
                fx["type"] = NEW
            if fx.get("module") == OLD:
                fx["module"] = NEW
            # Width is what made TapeDelay's two cross-fed lines alternate, so
            # a patch that used it wants Ping Pong on. Setting it here rather
            # than having the module arm itself on any width write keeps the
            # switch honest for live use.
            params = fx.get("params")
            if isinstance(params, dict):
                try:
                    if float(params.get("stereo_width", 0)) > 0:
                        params.setdefault("ping_pong", "On")
                except (TypeError, ValueError):
                    pass
            n += 1
    return n


def main(argv):
    in_place = "--in-place" in argv
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print(__doc__)
        return 2

    total = 0
    for arg in paths:
        p = pathlib.Path(arg)
        try:
            doc = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"  skip {p.name}: {e}")
            continue
        n = convert(doc)
        if not n:
            print(f"  skip {p.name}: no {OLD} slots")
            continue
        out = p if in_place else p.with_name(p.stem + "-tape-echo2.json")
        out.write_text(json.dumps(doc, indent=4) + "\n")
        print(f"  {p.name}: {n} slot(s) -> {NEW}  [{out.name}]")
        total += n
    print(f"{total} slot(s) converted")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
