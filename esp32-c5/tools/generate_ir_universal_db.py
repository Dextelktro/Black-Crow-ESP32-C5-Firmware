#!/usr/bin/env python3
"""Convert Flipper universal IR libraries to the firmware's static C tables."""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


MAX_RAW_TIMINGS = 1024
MAX_RMT_SYMBOLS = 512
MAX_RMT_DURATION_US = 32767
# A space longer than this ends a frame; raw universal signals keep only their
# first clean frame so the sweep never replays captured inter-frame dead air.
# Mirrors IR_UNIV_FRAME_GAP_US in ir.c. The firmware also truncates at runtime,
# so this is forward hygiene (and smaller flash) for regenerated tables.
FRAME_GAP_US = 25000
MIN_FRAME_TIMINGS = 8


def first_frame(timings: tuple[int, ...]) -> tuple[int, ...]:
    """Leading timings up to (not including) the first inter-frame gap.

    Spaces live at odd indices; a space > FRAME_GAP_US marks the end of the
    first frame. Returns the whole signal when there is no such gap (a genuine
    single-frame code, e.g. most AC) or when the cut would be implausibly short.
    """
    for i in range(1, len(timings), 2):
        if timings[i] > FRAME_GAP_US:
            return timings[:i] if i >= MIN_FRAME_TIMINGS else timings
    return timings


@dataclass
class Record:
    model: str
    fields: dict[str, str] = field(default_factory=dict)
    line: int = 0


CATEGORIES = (
    ("ac.ir", "ac", "IR_CAT_AC", {
        "Off": "off",
        "Dh": "dry",
        "Cool_hi": "cool_hi",
        "Cool_lo": "cool_lo",
        "Heat_hi": "heat_hi",
        "Heat_lo": "heat_lo",
    }),
    ("projectors.ir", "projector", "IR_CAT_PROJECTOR", {
        "Power": "power",
        "Vol_up": "vol_up",
        "Vol_dn": "vol_dn",
        "Mute": "mute",
    }),
    ("leds.ir", "led", "IR_CAT_LED", {
        "Power_on": "power_on",
        "Power_off": "power_off",
        "Brightness_up": "bright_up",
        "Brightness_dn": "bright_dn",
        "Red": "red",
        "Green": "green",
        "Blue": "blue",
        "White": "white",
    }),
    ("fans.ir", "fan", "IR_CAT_FAN", {
        "Power": "power",
        "Speed_up": "speed_up",
        "Speed_dn": "speed_dn",
        "Mode": "mode",
        "Rotate": "swing",
        "Timer": "timer",
    }),
    ("audio.ir", "audio", "IR_CAT_AUDIO", {
        "Power": "power",
        "Vol_up": "vol_up",
        "Vol_dn": "vol_dn",
        "Mute": "mute",
        "Next": "next",
        "Prev": "prev",
        "Play": "play",
        "Pause": "pause",
    }),
)

PROTOCOLS = {
    "NEC": ("IR_PROTO_NEC", 0),
    "NECext": ("IR_PROTO_NECEXT", 0),
    "NEC42": ("IR_PROTO_NEC42", 0),
    "Samsung32": ("IR_PROTO_SAMSUNG32", 0),
    "SIRC": ("IR_PROTO_SONY", 12),
    "SIRC15": ("IR_PROTO_SONY", 15),
    "SIRC20": ("IR_PROTO_SONY", 20),
    "RC5": ("IR_PROTO_RC5", 0),
    # The RC5 encoder handles the extended command bit used by RC5X.
    "RC5X": ("IR_PROTO_RC5", 0),
    "RC6": ("IR_PROTO_RC6", 0),
    "RCA": ("IR_PROTO_RCA", 0),
    "Pioneer": ("IR_PROTO_PIONEER", 0),
    "Kaseikyo": ("IR_PROTO_KASEIKYO", 0),
}


def parse_file(path: Path) -> list[Record]:
    records: list[Record] = []
    current: Record | None = None
    model = "Unknown"

    def finish() -> None:
        nonlocal current
        if current is not None:
            records.append(current)
            current = None

    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        model_match = re.fullmatch(r"#\s*Model:\s*(.*?)\s*", line)
        if model_match:
            finish()
            model = model_match.group(1) or "Unknown"
            continue
        if line.startswith("#"):
            continue
        if line.startswith("name: "):
            finish()
            current = Record(model=model, line=line_number)
            current.fields["name"] = line[6:].strip()
            continue
        if current is None:
            if line.startswith(("Filetype:", "Version:")):
                continue
            raise ValueError(f"{path}:{line_number}: data appears before a name")
        if ": " not in line:
            raise ValueError(f"{path}:{line_number}: malformed field")
        key, value = line.split(": ", 1)
        current.fields[key.strip()] = value.strip()

    finish()
    return records


def parse_hex_le(value: str, path: Path, record: Record, key: str) -> int:
    try:
        data = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"{path}:{record.line}: invalid {key}: {value}") from error
    if not data or len(data) > 4:
        raise ValueError(f"{path}:{record.line}: {key} must contain 1 to 4 bytes")
    return int.from_bytes(data, byteorder="little")


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def parse_categories(
    source_dir: Path,
) -> tuple[dict[str, list[str]], dict[str, list[str]], dict[str, int]]:
    raw_arrays: dict[str, list[str]] = {}
    entries: dict[str, list[str]] = {}
    counts: dict[str, int] = {}

    for filename, slug, category, button_map in CATEGORIES:
        path = source_dir / filename
        if not path.is_file():
            raise FileNotFoundError(f"required source file is missing: {path}")
        category_arrays: list[str] = []
        category_entries: list[str] = []
        raw_array_ids: dict[tuple[int, ...], str] = {}
        raw_index = 0
        records = parse_file(path)
        raw_count = parsed_count = 0
        for record in records:
            fields = record.fields
            name = fields.get("name", "")
            if name not in button_map:
                raise ValueError(f"{path}:{record.line}: unsupported button name {name!r}")
            button = button_map[name]
            signal_type = fields.get("type", "")
            model = c_string(record.model)

            if signal_type == "raw":
                try:
                    timings = tuple(int(value, 10) for value in fields["data"].split())
                    carrier = int(fields["frequency"], 10)
                    duty = float(fields["duty_cycle"])
                except (KeyError, ValueError) as error:
                    raise ValueError(f"{path}:{record.line}: incomplete RAW signal") from error
                if not timings or len(timings) > MAX_RAW_TIMINGS or any(value <= 0 for value in timings):
                    raise ValueError(f"{path}:{record.line}: invalid RAW timing count/value")
                # Keep only the first clean frame (drop captured repeats / dead air).
                # AC is exempt: its whole-state commands can legitimately span
                # two parts split by a >25 ms gap, so cutting could break them
                # (matches the runtime exemption in ir.c).
                if slug != "ac":
                    timings = first_frame(timings)
                if not 1 <= carrier <= 0xFFFF:
                    raise ValueError(f"{path}:{record.line}: carrier does not fit the firmware table")
                if not 0.0 < duty < 1.0:
                    raise ValueError(f"{path}:{record.line}: invalid duty cycle {duty}")
                symbols = math.ceil(
                    sum(math.ceil(value / MAX_RMT_DURATION_US) for value in timings) / 2
                )
                if symbols > MAX_RMT_SYMBOLS:
                    raise ValueError(
                        f"{path}:{record.line}: RAW signal needs {symbols} RMT symbols, "
                        f"limit is {MAX_RMT_SYMBOLS}"
                    )
                array_name = raw_array_ids.get(timings)
                if array_name is None:
                    array_name = f"ir_{slug}_raw_{raw_index}"
                    raw_index += 1
                    raw_array_ids[timings] = array_name
                    lines = [
                        ", ".join(str(value) for value in timings[offset:offset + 12])
                        for offset in range(0, len(timings), 12)
                    ]
                    category_arrays.append(
                        f"static const uint32_t {array_name}[] = {{\n    "
                        + ",\n    ".join(lines)
                        + "\n};"
                    )
                category_entries.append(
                    f"    {{ {category}, {c_string(button)}, {model}, IR_PROTO_RAW, "
                    f"0, 0, 0, {array_name}, {len(timings)}, {carrier}, {duty:.6f}f }},"
                )
                raw_count += 1
            elif signal_type == "parsed":
                protocol_name = fields.get("protocol", "")
                try:
                    protocol, bits = PROTOCOLS[protocol_name]
                    address = parse_hex_le(fields["address"], path, record, "address")
                    command = parse_hex_le(fields["command"], path, record, "command")
                except KeyError as error:
                    raise ValueError(
                        f"{path}:{record.line}: unsupported/incomplete parsed protocol "
                        f"{fields.get('protocol', '')!r}"
                    ) from error
                category_entries.append(
                    f"    {{ {category}, {c_string(button)}, {model}, {protocol}, "
                    f"0x{address:X}, 0x{command:X}, {bits}, NULL, 0, 0, 0.0f }},"
                )
                parsed_count += 1
            else:
                raise ValueError(f"{path}:{record.line}: unsupported signal type {signal_type!r}")
        raw_arrays[slug] = category_arrays
        entries[slug] = category_entries
        counts[slug] = len(records)
        print(
            f"{filename}: {len(records)} entries "
            f"({parsed_count} parsed, {raw_count} raw)"
        )

    return raw_arrays, entries, counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source_dir",
        type=Path,
        help="directory containing ac.ir, projectors.ir, leds.ir, fans.ir, and audio.ir",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "main",
        help="directory for generated include files (default: esp32-c5/main)",
    )
    args = parser.parse_args()

    try:
        raw_arrays, entries, _ = parse_categories(args.source_dir)
    except (FileNotFoundError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    total_raw = total_entries = 0
    for slug, category_entries in entries.items():
        raw_path = args.output_dir / f"ir_db_{slug}_raw.inc"
        entries_path = args.output_dir / f"ir_db_{slug}.inc"
        raw_path.write_text(
            "/* Generated from Flipper universal category .ir files. Do not edit. */\n"
            + "\n".join(raw_arrays[slug])
            + "\n",
            encoding="utf-8",
        )
        entries_path.write_text(
            "/* Generated from Flipper universal category .ir files. Do not edit. */\n"
            + "\n".join(category_entries)
            + "\n",
            encoding="utf-8",
        )
        total_raw += len(raw_arrays[slug])
        total_entries += len(category_entries)
        print(f"{entries_path.name}: {len(category_entries)} entries")
        print(f"{raw_path.name}: {len(raw_arrays[slug])} unique RAW arrays")
    print(f"wrote {total_entries} entries and {total_raw} unique RAW arrays")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
