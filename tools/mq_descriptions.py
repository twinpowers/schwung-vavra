"""Read gearmulator's parameterDescriptions_mq.json.

That file is the authority for the microQ's ~380 single parameters: index,
range, step, discrete/bipolar, and the name of the value list an enum draws
its option text from. It is JSON with // comments, so it needs stripping
first -- and the stripping must be line-based and comment-only, because a
naive regex over the whole file eats the "//" inside strings.
"""
import json, pathlib, re

DEFAULT_PATH = (pathlib.Path(__file__).resolve().parent.parent /
                "libs/gearmulator/source/waldi/microq/mqJucePlugin/parameterDescriptions_mq.json")


def load(path=None):
    """Return (descriptions, valuelists, defaults, section_of_index)."""
    raw = pathlib.Path(path or DEFAULT_PATH).read_text()
    cleaned, sections, section = [], [], None
    for line in raw.split("\n"):
        heading = re.match(r"\s*//\s*(.+?)\s*$", line)
        if heading:
            section = heading.group(1)
            cleaned.append("")
            continue
        body = re.sub(r"//.*$", "", line)
        if '"index"' in body:
            sections.append(section)
        cleaned.append(body)
    text = re.sub(r",(\s*[}\]])", r"\1", "\n".join(cleaned))
    data = json.loads(text)
    descriptions = data["parameterdescriptions"]
    if len(sections) != len(descriptions):
        raise SystemExit(f"section walk desynced: {len(sections)} vs {len(descriptions)}")
    for description, name in zip(descriptions, sections):
        description["section"] = name
    return descriptions, data["valuelists"], data["parameterdescriptiondefaults"], data.get("regions", [])


def with_defaults(description, defaults):
    merged = dict(defaults)
    merged.update(description)
    return merged
