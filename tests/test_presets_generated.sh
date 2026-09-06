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

# The plugin serves its own chain_params and the chain host parses module.json's
# copy; only the module.json half is visible without a device, so pin the KEY
# LIST of the two against each other from source. A key in one and not the
# other is the invented-knob failure again, one level up from the names.
source = (root / "src/dsp/vavra_plugin.cpp").read_text()
contract = source[source.index('if(!strcmp(key,"chain_params"))'):source.index('if(!strcmp(key,"ui_hierarchy"))')]
# Only top-level params: every one is {"key":...,"name":...}, while a
# visible_if carries its own inner "key" and would double-count.
in_plugin = re.findall(r'\{"key":"([a-z_]+)","name"', contract)
in_module = [p["key"] for p in module["chain_params"]]
if in_plugin != in_module:
    failures.append(f"chain_params keys differ: plugin {in_plugin} vs module.json {in_module}")

hierarchy = source[source.index('if(!strcmp(key,"ui_hierarchy"))'):]
hierarchy = hierarchy[:hierarchy.index("\n")]
in_hierarchy = re.findall(r'"([a-z_]+)"', hierarchy[hierarchy.index('"knobs"'):])
in_hierarchy = [k for k in in_hierarchy if k not in ("knobs", "params")]
if sorted(set(in_hierarchy)) != sorted(set(in_plugin)):
    failures.append(f"ui_hierarchy names {sorted(set(in_hierarchy))} do not match chain_params {sorted(set(in_plugin))}")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print(f"PASS: {len(rows)} preset names agree across the dump, the header and module.json")
PY
