#!/usr/bin/env bash
# The preset names exist in three places -- docs/presets-os223.tsv (the dump),
# src/dsp/presets_os223.h (compiled into the module) and src/module.json's
# enum options (parsed by the chain host) -- and only the first is authored.
# A stale copy does not fail loudly at runtime: it shows the WRONG NAME on a
# preset that still loads, or it makes the host's param table disagree with
# the plugin's chain_params, which is how a module ends up with an invented
# knob writing a float into an enum. So compare them by content.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
import json, pathlib, re, sys

root = pathlib.Path(".")
rows = []
for line in (root / "docs/presets-os223.tsv").read_text().splitlines():
    if line.startswith("#") or not line.strip():
        continue
    slot, name = line.split("\t", 1)
    rows.append(re.sub(r"\s{2,}", " ", f"{slot} {name.rstrip()}"))

failures = []
if len(rows) != 300:
    failures.append(f"docs/presets-os223.tsv has {len(rows)} presets, expected 300")

header = (root / "src/dsp/presets_os223.h").read_text()
in_header = re.findall(r'^    "(.*)",$', header, re.M)
if in_header != [r.replace('"', "'") for r in rows]:
    failures.append(f"src/dsp/presets_os223.h is stale ({len(in_header)} names); "
                    "re-run tools/gen_presets_header.py")

module = json.loads((root / "src/module.json").read_text())
options = next((p.get("options") for p in module["chain_params"] if p["key"] == "preset"), None)
if options != rows:
    failures.append(f"src/module.json preset options are stale ({options and len(options)} names); "
                    "re-run tools/gen_presets_header.py")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print(f"PASS: {len(rows)} preset names agree across the dump, the header and module.json")
PY
