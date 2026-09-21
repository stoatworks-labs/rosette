#!/usr/bin/env python3
"""Every preset row must carry a full set of values, of the right kind.

`Rosette.h` maps each preset column to a ParamID with a table whose size is
`presets::kParamCount`, so a column added to one and not the other fails to
compile. What no compiler catches is a preset **row** with too few values: C++
aggregate initialisation fills the rest with zero, without a word, so the row
is accepted and silently means something else -- a Riso with no paper colour
rather than a Riso with the paper left out.

A boolean or option parameter holds the **real value**, not a 0..1 fraction.
The SDK's clamp into 0..1 is guarded by `if( pType == FF_TYPE_STANDARD )`.
Write 0.5 into the Dot Shape column and it does not mean "halfway down the
list": it rounds to 0 or 1 depending on who reads it.
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
SOURCE = HERE / "source" / "Presets.h"


def main() -> int:
    text = SOURCE.read_text()

    start = text.index("enum Param\n{")
    end = text.index("kParamCount", start)
    names = []
    for line in text[start:end].splitlines():
        stripped = line.strip()
        if not stripped.startswith("k"):
            continue
        # A line may declare several columns: `kInkCR, kInkCG, kInkCB,`
        for name in stripped.rstrip(",").split(","):
            name = name.strip()
            if name.startswith("k"):
                names.append(name)
    expected = len(names)
    if expected == 0:
        print("could not parse the Param enum out of Presets.h", file=sys.stderr)
        return 1

    # Columns whose host parameter is discrete, with the range declared for it
    # in Rosette.cpp. A value here must be a whole number inside that range.
    discrete_columns = {
        "kDotShape": (0, 3),
        "kPlateC": (0, 1),
        "kPlateM": (0, 1),
        "kPlateY": (0, 1),
        "kPlateK": (0, 1),
    }
    for column in discrete_columns:
        if column not in names:
            print(f"the Param enum has no {column}; this check needs updating", file=sys.stderr)
            return 1

    column_of = {name: i for i, name in enumerate(names)}
    failures = 0
    rows = 0

    for match in re.finditer(r'\{\s*"([^"]+)",\s*\{(.*?)\}\s*\}', text, re.S):
        name, body = match.group(1), match.group(2)
        rows += 1
        cleaned = "\n".join(line.split("//")[0] for line in body.splitlines())
        # Strip the /*Label*/ comments the table uses to keep itself readable.
        cleaned = re.sub(r"/\*.*?\*/", "", cleaned, flags=re.S)
        values = [v for v in cleaned.split(",") if v.strip()]

        if len(values) != expected:
            print(f"FAIL  {name}: {len(values)} values, expected {expected}")
            failures += 1
            continue

        if len(name) > 16:
            print(f"FAIL  {name}: {len(name)} characters; FFGL shows 16")
            failures += 1

        for column, (low, high) in discrete_columns.items():
            raw = values[column_of[column]].strip().rstrip("f")
            try:
                number = float(raw)
            except ValueError:
                print(f"FAIL  {name}: {column} is {raw!r}, which is not a number")
                failures += 1
                continue
            if number != int(number):
                print(f"FAIL  {name}: {column} = {raw} is a fraction; that parameter is discrete")
                failures += 1
            elif not low <= number <= high:
                print(f"FAIL  {name}: {column} = {raw} is outside its range {low}..{high}")
                failures += 1

        # Everything else is a standard 0..1 parameter.
        for column, index in column_of.items():
            if column in discrete_columns:
                continue
            raw = values[index].strip().rstrip("f")
            try:
                number = float(raw)
            except ValueError:
                print(f"FAIL  {name}: {column} is {raw!r}, which is not a number")
                failures += 1
                continue
            if not 0.0 <= number <= 1.0:
                print(f"FAIL  {name}: {column} = {raw} is outside 0..1")
                failures += 1

    if rows == 0:
        print("no preset rows found -- has the table's shape changed?", file=sys.stderr)
        return 1
    if failures:
        print(f"\n{failures} problem(s) across {rows} preset rows")
        return 1
    print(f"ok    {rows} preset rows, {expected} values each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
