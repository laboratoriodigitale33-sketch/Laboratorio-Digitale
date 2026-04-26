#!/usr/bin/env python3
"""
Scarica dataset statici da NASA/JPL Horizons per il viewer del sito.

Punti chiave:
- usa solo la libreria standard;
- salva JSON statici in ``data/horizons/``;
- crea anche ``manifest.json`` per il frontend;
- su alcune reti Windows puo` fare retry senza verifica SSL se il certificato
  viene intercettato da proxy/firewall locali.
"""

from __future__ import annotations

import argparse
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, replace
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any


HORIZONS_API = "https://ssd.jpl.nasa.gov/api/horizons.api"
ERROR_PRIOR_RE = re.compile(r'No ephemeris .* prior to (A\.D\.\s+.+?) TDB', re.IGNORECASE)
ERROR_AFTER_RE = re.compile(r'No ephemeris .* after (A\.D\.\s+.+?) TDB', re.IGNORECASE)


@dataclass(frozen=True)
class TargetConfig:
    slug: str
    name: str
    command: str
    center: str
    center_name: str
    start: str
    stop: str
    step: str
    description: str


TARGETS: list[TargetConfig] = [
    TargetConfig(
        slug="artemis1_orion_geocentric_2022",
        name="Artemis I / Orion",
        command="-1023",
        center="@399",
        center_name="Earth",
        start="2022-Nov-16 08:46",
        stop="2022-Dec-11 17:20",
        step="1 h",
        description="Traiettoria geocentrica di Artemis I / Orion.",
    ),
    TargetConfig(
        slug="artemis2_orion_geocentric_2026",
        name="Artemis II / Orion Integrity",
        command="-1024",
        center="@399",
        center_name="Earth",
        start="2026-Apr-02 03:30",
        stop="2026-Apr-12 00:00",
        step="1 h",
        description="Traiettoria geocentrica di Artemis II / Orion Integrity, se disponibile in Horizons.",
    ),
    TargetConfig(
        slug="apollo8_sivb_geocentric_1968",
        name="Apollo 8 S-IVB",
        command="-399080",
        center="@399",
        center_name="Earth",
        start="1968-Dec-21 12:00",
        stop="1968-Dec-28 18:00",
        step="1 h",
        description="Stadio S-IVB associato ad Apollo 8.",
    ),
    TargetConfig(
        slug="apollo9_sivb_geocentric_1969",
        name="Apollo 9 S-IVB",
        command="-399090",
        center="@399",
        center_name="Earth",
        start="1969-Mar-03 16:00",
        stop="1969-Mar-13 18:00",
        step="1 h",
        description="Stadio S-IVB associato ad Apollo 9.",
    ),
    TargetConfig(
        slug="apollo10_sivb_geocentric_1969",
        name="Apollo 10 S-IVB",
        command="-399100",
        center="@399",
        center_name="Earth",
        start="1969-May-18 16:00",
        stop="1969-May-26 18:00",
        step="1 h",
        description="Stadio S-IVB associato ad Apollo 10.",
    ),
    TargetConfig(
        slug="apollo10_lm_snoopy_geocentric_1969",
        name="Apollo 10 LM Snoopy",
        command="-399101",
        center="@399",
        center_name="Earth",
        start="1969-May-22 00:00",
        stop="1969-May-24 12:00",
        step="30 min",
        description="Modulo lunare Snoopy di Apollo 10.",
    ),
    TargetConfig(
        slug="apollo11_sivb_geocentric_1969",
        name="Apollo 11 S-IVB",
        command="-399110",
        center="@399",
        center_name="Earth",
        start="1969-Jul-16 13:30",
        stop="1969-Jul-24 18:00",
        step="1 h",
        description="Stadio S-IVB associato ad Apollo 11.",
    ),
    TargetConfig(
        slug="apollo12_sivb_geocentric_1969",
        name="Apollo 12 S-IVB",
        command="-399120",
        center="@399",
        center_name="Earth",
        start="1969-Nov-14 16:30",
        stop="1969-Nov-24 20:00",
        step="1 h",
        description="Stadio S-IVB associato ad Apollo 12.",
    ),
    TargetConfig(
        slug="voyager1_heliocentric_1977_1979",
        name="Voyager 1",
        command="-31",
        center="@sun",
        center_name="Sun",
        start="1977-Sep-05",
        stop="1979-Mar-05",
        step="5 d",
        description="Primo tratto eliocentrico della missione Voyager 1.",
    ),
    TargetConfig(
        slug="voyager2_heliocentric_1977_1981",
        name="Voyager 2",
        command="-32",
        center="@sun",
        center_name="Sun",
        start="1977-Aug-20",
        stop="1981-Aug-25",
        step="8 d",
        description="Primo tratto eliocentrico della missione Voyager 2.",
    ),
    TargetConfig(
        slug="parker_solar_probe_heliocentric_2018_2019",
        name="Parker Solar Probe",
        command="-96",
        center="@sun",
        center_name="Sun",
        start="2018-Aug-12",
        stop="2019-Apr-01",
        step="8 h",
        description="Traiettoria eliocentrica iniziale di Parker Solar Probe.",
    ),
    TargetConfig(
        slug="moon_geocentric_2026",
        name="Moon",
        command="301",
        center="@399",
        center_name="Earth",
        start="2026-Jan-01",
        stop="2026-Mar-01",
        step="3 h",
        description="Traiettoria geocentrica della Luna, utile come riferimento.",
    ),
    TargetConfig(
        slug="mars_heliocentric_2026_2028",
        name="Mars",
        command="499",
        center="@sun",
        center_name="Sun",
        start="2026-Jan-01",
        stop="2028-Jan-01",
        step="5 d",
        description="Traiettoria eliocentrica di Marte come riferimento planetario.",
    ),
]


def log(message: str) -> None:
    print(message, flush=True)


def build_horizons_url(target: TargetConfig) -> str:
    params = {
        "format": "json",
        "MAKE_EPHEM": "YES",
        "EPHEM_TYPE": "VECTORS",
        "OBJ_DATA": "YES",
        "COMMAND": f"'{target.command}'",
        "CENTER": f"'{target.center}'",
        "START_TIME": f"'{target.start}'",
        "STOP_TIME": f"'{target.stop}'",
        "STEP_SIZE": f"'{target.step}'",
        "OUT_UNITS": "'KM-S'",
        "REF_PLANE": "'ECLIPTIC'",
        "REF_SYSTEM": "'J2000'",
        "CSV_FORMAT": "'YES'",
        "VEC_TABLE": "'2'",
    }
    return HORIZONS_API + "?" + urllib.parse.urlencode(params)


def should_retry_without_ssl_check(exc: BaseException) -> bool:
    current: BaseException | None = exc
    while current is not None:
        if isinstance(current, ssl.SSLCertVerificationError):
            return True
        if isinstance(current, ssl.SSLError) and "CERTIFICATE_VERIFY_FAILED" in str(current):
            return True
        current = current.__cause__ or current.__context__
    return False


def fetch_json(url: str, timeout: int = 90) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "simulazioni-horizons-script/1.1"},
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except urllib.error.URLError as exc:
        if not should_retry_without_ssl_check(exc):
            raise
        log("  SSL warning: certificate verification failed, retrying without SSL verification.")
        insecure_context = ssl._create_unverified_context()
        with urllib.request.urlopen(req, timeout=timeout, context=insecure_context) as response:
            raw = response.read().decode("utf-8", errors="replace")

    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        preview = raw[:320].replace("\n", " ")
        raise RuntimeError(f"Response is not valid JSON. Preview: {preview!r}") from exc


def clean_horizons_time(raw_value: str) -> str:
    return raw_value.replace("A.D. ", "").strip()


def shift_horizons_time(value: str, seconds: int) -> str:
    formats = [
        "%Y-%b-%d %H:%M:%S.%f",
        "%Y-%b-%d %H:%M:%S",
        "%Y-%b-%d %H:%M",
        "%Y-%b-%d",
    ]

    text = value.strip().title()
    for fmt in formats:
        try:
            parsed = datetime.strptime(text, fmt)
            shifted = parsed + timedelta(seconds=seconds)
            if fmt == "%Y-%b-%d":
                return shifted.strftime("%Y-%b-%d")
            if fmt == "%Y-%b-%d %H:%M":
                return shifted.strftime("%Y-%b-%d %H:%M")
            return shifted.strftime("%Y-%b-%d %H:%M:%S")
        except ValueError:
            continue
    return value


def maybe_adjust_time_window(target: TargetConfig, error_text: str) -> TargetConfig | None:
    new_start = target.start
    new_stop = target.stop
    changed = False

    prior_match = ERROR_PRIOR_RE.search(error_text)
    if prior_match:
        candidate = clean_horizons_time(prior_match.group(1))
        if candidate:
            if candidate == target.start:
                candidate = shift_horizons_time(candidate, 1)
            if candidate != target.start:
                new_start = candidate
                changed = True

    after_match = ERROR_AFTER_RE.search(error_text)
    if after_match:
        candidate = clean_horizons_time(after_match.group(1))
        if candidate:
            if candidate == target.stop:
                candidate = shift_horizons_time(candidate, -1)
            if candidate != target.stop:
                new_stop = candidate
                changed = True

    if not changed:
        return None

    return replace(target, start=new_start, stop=new_stop)


def parse_vectors_result(result_text: str) -> list[dict[str, float | str]]:
    start = result_text.find("$$SOE")
    stop = result_text.find("$$EOE")
    if start < 0 or stop < 0 or stop <= start:
        raise RuntimeError("Could not find $$SOE/$$EOE block in Horizons response.")

    block = result_text[start + len("$$SOE") : stop].strip()
    lines = [line.strip() for line in block.splitlines() if line.strip()]

    points: list[dict[str, float | str]] = []
    for line in lines:
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 8:
            continue
        try:
            points.append(
                {
                    "jd": float(parts[0]),
                    "date": parts[1],
                    "x": float(parts[2]),
                    "y": float(parts[3]),
                    "z": float(parts[4]),
                    "vx": float(parts[5]),
                    "vy": float(parts[6]),
                    "vz": float(parts[7]),
                }
            )
        except ValueError:
            continue

    if len(points) < 2:
        raise RuntimeError("Parsed fewer than two valid state vectors.")

    return points


def make_dataset(
    target: TargetConfig,
    points: list[dict[str, float | str]],
    query_url: str,
) -> dict[str, Any]:
    return {
        "schema": "horizons-vector-dataset-v1",
        "name": target.name,
        "slug": target.slug,
        "description": target.description,
        "source": {
            "name": "NASA/JPL Horizons",
            "api": HORIZONS_API,
            "queryUrl": query_url,
        },
        "target": {"command": target.command, "name": target.name},
        "center": {"code": target.center, "name": target.center_name},
        "time": {"start": target.start, "stop": target.stop, "step": target.step},
        "frame": {"referencePlane": "ECLIPTIC", "referenceSystem": "J2000"},
        "units": {"position": "km", "velocity": "km/s"},
        "columns": ["date", "jd", "x", "y", "z", "vx", "vy", "vz"],
        "pointCount": len(points),
        "points": points,
    }


def save_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def download_target(target: TargetConfig, out_dir: Path) -> dict[str, Any]:
    log("")
    log(f"-> Downloading: {target.name}")
    log(f"   COMMAND={target.command} CENTER={target.center} {target.start} -> {target.stop}, step={target.step}")

    active_target = target
    url = build_horizons_url(active_target)
    response: dict[str, Any] | None = None

    for _ in range(5):
        response = fetch_json(url)
        if "error" not in response:
            break
        error_text = str(response["error"])
        adjusted_target = maybe_adjust_time_window(active_target, error_text)
        if adjusted_target is None:
            break
        if adjusted_target.start == active_target.start and adjusted_target.stop == active_target.stop:
            break
        log(f"   Adjusting time window to {adjusted_target.start} -> {adjusted_target.stop} and retrying.")
        active_target = adjusted_target
        url = build_horizons_url(active_target)

    if response is None:
        raise RuntimeError("Empty response while requesting Horizons.")

    if "error" in response:
        raise RuntimeError(f"Horizons error: {response['error']}")

    result = response.get("result")
    if not isinstance(result, str):
        raise RuntimeError("Horizons response does not contain a textual 'result' field.")

    points = parse_vectors_result(result)
    dataset = make_dataset(active_target, points, url)
    out_path = out_dir / f"{active_target.slug}.json"
    save_json(out_path, dataset)

    log(f"   OK: {len(points)} points saved to {out_path}")

    return {
        "slug": active_target.slug,
        "name": active_target.name,
        "description": active_target.description,
        "command": active_target.command,
        "center": active_target.center,
        "centerName": active_target.center_name,
        "start": active_target.start,
        "stop": active_target.stop,
        "step": active_target.step,
        "file": f"{active_target.slug}.json",
        "pointCount": len(points),
        "units": {"position": "km", "velocity": "km/s"},
    }


def write_manifest(out_dir: Path, entries: list[dict[str, Any]], errors: list[dict[str, str]]) -> None:
    manifest = {
        "schema": "horizons-manifest-v1",
        "source": "NASA/JPL Horizons",
        "generatedBy": "scarica_horizons.py",
        "count": len(entries),
        "datasets": entries,
        "errors": errors,
    }
    save_json(out_dir / "manifest.json", manifest)
    log("")
    log(f"Manifest saved to {out_dir / 'manifest.json'}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download precomputed NASA/JPL Horizons state vectors for the static site viewer."
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("data/horizons"),
        help="Output folder. Default: data/horizons",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=1.0,
        help="Delay in seconds between requests. Default: 1.0",
    )
    parser.add_argument(
        "--only",
        nargs="*",
        default=None,
        help="Download only selected slugs.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available slugs and exit.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.list:
        log("Available datasets:")
        for target in TARGETS:
            log(f"  {target.slug:45s} COMMAND={target.command:>8s}  {target.name}")
        return 0

    selected = TARGETS
    if args.only:
        requested = set(args.only)
        selected = [target for target in TARGETS if target.slug in requested]
        missing = requested - {target.slug for target in selected}
        if missing:
            log("Unknown slugs:")
            for slug in sorted(missing):
                log(f"  - {slug}")
            return 2

    args.out.mkdir(parents=True, exist_ok=True)

    entries: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []

    for index, target in enumerate(selected, start=1):
        try:
            entries.append(download_target(target, args.out))
        except Exception as exc:  # noqa: BLE001
            log(f"   ERROR on {target.name}: {exc}")
            errors.append(
                {
                    "slug": target.slug,
                    "name": target.name,
                    "command": target.command,
                    "error": str(exc),
                }
            )
        if index < len(selected):
            time.sleep(max(0.0, args.delay))

    write_manifest(args.out, entries, errors)

    log("")
    log("Done.")
    log(f"Datasets downloaded: {len(entries)}")
    log(f"Datasets failed:     {len(errors)}")

    if errors:
        log("")
        log("Some datasets could not be downloaded. Check data/horizons/manifest.json for details.")

    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
