#!/usr/bin/env python3
"""Add the demo terminal theme and trim the asciinema framing.

asciinema starts recording before the tmux session is attached, so the cast
begins with an idle screen that only shows the cursor. asciinema also appends an
"[exited]" line and clears the alternate screen when the recorded command
finishes. Neither frame is part of the demo. The theme is the xfce4-terminal
palette because agg does not accept a custom palette on the command line.
asciicast v3 input is converted to v2 because agg expects v2.
"""

import argparse
import json
import re
from pathlib import Path

THEME = {
    "fg": "#ffffff",
    "bg": "#000000",
    "palette": (
        "#000000:#aa0000:#44aa44:#aa5500:#0039aa:#aa22aa:#1a92aa:#aaaaaa:"
        "#777777:#ff8787:#4ce64c:#ded82c:#295fcc:#cc58cc:#4ccce6:#ffffff"
    ),
}

ANSI_ESCAPE = re.compile(
    r"\x1b\[[0-9;?]*[ -/]*[@-~]"
    r"|\x1b\][^\x07]*(?:\x07|\x1b\\)"
    r"|\x1b[()][0-9A-Za-z]"
    r"|\x1b[=>]"
    r"|\x1b[@-_]"
)


def hasVisibleText(text):
    return bool(ANSI_ESCAPE.sub("", text).strip())


def trimInitialIdle(events, keep):
    """Move the first visible output to the start of the recording.

    Terminal setup such as the alternate-screen switch can precede the first
    visible text. Clamping those events to time zero preserves their order
    without dropping the setup. Input events before the first output stay as
    well, so replaying the cast still produces the same keystrokes.
    """
    first_visible = next(
        (index for index, (_, event_type, text) in enumerate(events)
         if event_type == "o" and hasVisibleText(text)),
        None,
    )
    if first_visible is None:
        return
    cut = float(events[first_visible][0]) - keep
    if cut <= 0:
        return
    for event in events:
        event[0] = max(0.0, round(float(event[0]) - cut, 6))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cast", type=Path, help="asciinema cast to finalize (v2 or v3)")
    parser.add_argument(
        "--output", "-o", type=Path, default=None,
        help="write the result here instead of replacing the input",
    )
    parser.add_argument(
        "--keep-start", type=float, default=0.0,
        help="seconds of the idle screen to keep before the first visible output (default: 0)",
    )
    args = parser.parse_args()
    if args.keep_start < 0:
        raise SystemExit("error: --keep-start must not be negative")

    lines = args.cast.read_text().splitlines()
    if not lines:
        raise SystemExit(f"error: {args.cast} is empty")
    header = json.loads(lines[0])
    events = []
    for line in lines[1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        event = json.loads(stripped)
        if isinstance(event, list) and len(event) == 3:
            events.append(event)

    if header.get("version", 2) == 3:
        term = header.get("term") or {}
        if "cols" not in term or "rows" not in term:
            raise SystemExit("error: asciicast v3 header is missing term.cols/term.rows")
        header["version"] = 2
        header["width"] = term["cols"]
        header["height"] = term["rows"]
        header.pop("term", None)
        header.pop("tags", None)
        absolute_time = 0.0
        converted = []
        for interval, event_type, text in events:
            absolute_time += float(interval)
            if event_type in ("o", "i"):
                converted.append([round(absolute_time, 6), event_type, text])
        events = converted
        print("Converted asciicast v3 to v2")

    header["theme"] = THEME

    while events:
        timestamp, event_type, text = events[-1]
        if ("[exited]" in text or "\x1b[?1049l" in text
                or (event_type == "o" and not hasVisibleText(text))):
            events.pop()
            continue
        break

    trimInitialIdle(events, args.keep_start)

    output = args.output or args.cast
    output.write_text(
        "\n".join([json.dumps(header)] + [json.dumps(event) for event in events]) + "\n"
    )
    print(f"Wrote {output} with {len(events)} events")


if __name__ == "__main__":
    main()
