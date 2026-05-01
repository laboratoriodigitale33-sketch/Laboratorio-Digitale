#!/usr/bin/env python3
"""
Genera una piccola griglia MIST per il diagramma H-R:
- più masse
- più metallicità [Fe/H]
- opzionalmente più rotazioni v/vcrit
- un file JSON per ogni combinazione (feh, vvcrit)
- un manifest JSON con l'indice dei file disponibili

Uso tipico:
    python prepare_mist_hr_grid.py

Output:
    data/manifest_mist_hr.json
    data/mist_feh_..._vvcrit_....json

Note:
- Default prudente: 12 masse, 4 metallicità, 1 rotazione.
- Puoi aumentare metallicità/rotazioni da riga di comando.
"""

from __future__ import annotations

import argparse
import io
import json
import math
import re
import sys
import tarfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_MASSES = [0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0, 5.0, 8.0, 15.0, 20.0, 40.0]
DEFAULT_FEH = [-1.0, -0.5, 0.0, 0.25]
DEFAULT_VVCRIT = [0.0]
DEFAULT_AFE = 0.0

FALLBACK_COLUMNS = [
    "EEP", "star_age", "star_mass", "star_mdot", "he_core_mass", "c_core_mass",
    "log_L", "log_LH", "log_LHe", "log_Teff", "log_R", "log_g",
    "surface_h1", "surface_he3", "surface_he4", "surface_c12", "surface_o16",
    "log_center_T", "log_center_Rho", "center_gamma", "center_h1",
    "center_he4", "center_c12", "phase"
]
REQUIRED = {"star_age", "star_mass", "log_L", "log_Teff"}


@dataclass
class Track:
    filename: str
    initial_mass: float
    points: list[dict[str, Any]]


def feh_token(x: float) -> str:
    """MIST v1.2 usa due decimali per [Fe/H], per esempio m1.00, p0.25, p0.00."""
    return ("p" if x >= 0 else "m") + f"{abs(x):.2f}"


def afe_token(x: float) -> str:
    """MIST v1.2 usa un decimale per [alpha/Fe], per esempio p0.0."""
    return ("p" if x >= 0 else "m") + f"{abs(x):.1f}"


def vvcrit_token(v: float) -> str:
    """MIST v1.2 usa un decimale per v/vcrit, per esempio 0.0 oppure 0.4."""
    return f"{v:.1f}"


def mist_url(feh: float, afe: float, vvcrit: float, version: str = "v1.2") -> str:
    return (
        f"https://mist.science/data/tarballs_{version}/"
        f"MIST_{version}_feh_{feh_token(feh)}_afe_{afe_token(afe)}_vvcrit{vvcrit_token(vvcrit)}_EEPS.txz"
    )


def dataset_filename(feh: float, vvcrit: float) -> str:
    return f"mist_feh_{feh_token(feh)}_vvcrit_{vvcrit_token(vvcrit)}.json"


def download_bytes(url: str) -> bytes:
    print(f"Scarico: {url}", file=sys.stderr)
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Mozilla/5.0 (educational MIST HR downloader)"}
    )
    with urllib.request.urlopen(req, timeout=240) as response:
        return response.read()


def decode_bytes(raw: bytes) -> str:
    for enc in ("utf-8", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            pass
    return raw.decode("utf-8", errors="replace")


def parse_mass_from_filename(name: str) -> float | None:
    base = Path(name).name
    # e.g. 00100M.track.eep => 1.00 Msun
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)M\.track\.eep$", base)
    if not m:
        return None
    token = m.group(1)
    try:
        if "." in token:
            return float(token)
        return int(token) / 100.0
    except ValueError:
        return None


def clean_header(line: str) -> list[str]:
    line = line.strip().lstrip("#").strip()
    return [x.strip() for x in line.split() if x.strip()]


def find_columns(lines: list[str]) -> list[str]:
    best: list[str] = []
    for line in lines:
        low = line.lower()
        if "star_age" in low and "log_teff" in low and "log_l" in low:
            cols = clean_header(line)
            if REQUIRED.issubset(set(cols)):
                best = cols
    return best or FALLBACK_COLUMNS[:]


def is_data_line(line: str) -> bool:
    s = line.strip()
    if not s or s.startswith("#"):
        return False
    first = s.split()[0]
    return bool(re.match(r"^[+-]?(\d+(\.\d*)?|\.\d+)([EeDd][+-]?\d+)?$", first))


def parse_float(token: str) -> float | None:
    try:
        value = float(token.replace("D", "E").replace("d", "E"))
        if math.isfinite(value):
            return value
    except ValueError:
        return None
    return None


def parse_track_text(text: str, filename: str) -> Track | None:
    lines = text.splitlines()
    columns = find_columns(lines)
    initial_mass = parse_mass_from_filename(filename)

    points: list[dict[str, Any]] = []

    for line in lines:
        if not is_data_line(line):
            continue
        parts = line.split()
        cols = columns[:]
        if len(parts) > len(cols):
            cols += [f"COL_{i}" for i in range(len(cols), len(parts))]

        row: dict[str, Any] = {}
        for col, token in zip(cols, parts):
            value = parse_float(token)
            row[col] = value if value is not None else token

        if not REQUIRED.issubset(row.keys()):
            continue

        age = row.get("star_age")
        mass = row.get("star_mass")
        log_l = row.get("log_L")
        log_teff = row.get("log_Teff")

        if not all(isinstance(v, (int, float)) and math.isfinite(v) for v in (age, mass, log_l, log_teff)):
            continue

        point = {
            "age_yr": float(age),
            "mass": float(mass),
            "logL": float(log_l),
            "logTeff": float(log_teff),
            "phase": int(row["phase"]) if isinstance(row.get("phase"), float) and float(row["phase"]).is_integer() else row.get("phase", "")
        }
        if isinstance(row.get("log_R"), (int, float)) and math.isfinite(row["log_R"]):
            point["logR"] = float(row["log_R"])
        if isinstance(row.get("log_g"), (int, float)) and math.isfinite(row["log_g"]):
            point["logg"] = float(row["log_g"])
        points.append(point)

    if len(points) < 10:
        return None

    points.sort(key=lambda p: p["age_yr"])
    if initial_mass is None:
        initial_mass = points[0]["mass"]

    if not (0.05 <= initial_mass <= 400):
        return None

    return Track(filename=filename, initial_mass=float(initial_mass), points=points)


def load_tracks_from_txz(txz_bytes: bytes) -> list[Track]:
    tracks: list[Track] = []
    with tarfile.open(fileobj=io.BytesIO(txz_bytes), mode="r:xz") as tf:
        members = [m for m in tf.getmembers() if m.isfile() and m.name.endswith(".track.eep")]

        for i, member in enumerate(members, start=1):
            ex = tf.extractfile(member)
            if ex is None:
                continue
            raw = ex.read()
            if len(raw) < 1000:
                continue
            tr = parse_track_text(decode_bytes(raw), member.name)
            if tr is not None:
                tracks.append(tr)
            if i % 50 == 0:
                print(f"  letti {i}/{len(members)} file .track.eep", file=sys.stderr)

    tracks.sort(key=lambda t: t.initial_mass)
    return tracks


def select_nearest_tracks(tracks: list[Track], target_masses: list[float]) -> list[tuple[float, Track]]:
    selected: list[tuple[float, Track]] = []
    used: set[str] = set()
    for target in target_masses:
        candidates = [t for t in tracks if t.filename not in used]
        if not candidates:
            break
        best = min(candidates, key=lambda t: abs(t.initial_mass - target))
        used.add(best.filename)
        selected.append((target, best))
    return selected


def simplify_points(points: list[dict[str, Any]], max_points: int) -> list[dict[str, Any]]:
    if len(points) <= max_points:
        return points

    finite = [p for p in points if math.isfinite(float(p["logTeff"])) and math.isfinite(float(p["logL"]))]
    if len(finite) <= max_points:
        return finite

    xs = [float(p["logTeff"]) for p in finite]
    ys = [float(p["logL"]) for p in finite]
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    xr = max(xmax - xmin, 1e-9)
    yr = max(ymax - ymin, 1e-9)

    cumulative = [0.0]
    for i in range(1, len(finite)):
        dx = (finite[i]["logTeff"] - finite[i - 1]["logTeff"]) / xr
        dy = (finite[i]["logL"] - finite[i - 1]["logL"]) / yr
        cumulative.append(cumulative[-1] + math.hypot(dx, dy))

    total = cumulative[-1]
    if total <= 0:
        return finite[:max_points]

    keep = {0, len(finite) - 1}
    # preserva cambi di fase
    for i in range(1, len(finite)):
        if finite[i].get("phase") != finite[i - 1].get("phase"):
            for j in (i - 2, i - 1, i, i + 1):
                if 0 <= j < len(finite):
                    keep.add(j)

    # campiona per lunghezza d'arco
    j = 0
    for k in range(max_points):
        target = total * k / max(1, max_points - 1)
        while j < len(cumulative) - 1 and cumulative[j] < target:
            j += 1
        keep.add(j)

    idx = sorted(keep)
    if len(idx) > max_points:
        step = len(idx) / max_points
        idx = sorted(set(idx[min(len(idx) - 1, int(k * step))] for k in range(max_points)))

    return [finite[i] for i in idx]


def describe_phases(points: list[dict[str, Any]]) -> list[Any]:
    phases = []
    for p in points:
        ph = p.get("phase", "")
        if ph not in phases:
            phases.append(ph)
    return phases


def build_payload(selected: list[tuple[float, Track]], feh: float, afe: float, vvcrit: float, url: str, max_points: int) -> dict[str, Any]:
    tracks_json = []
    for target, tr in selected:
        pts = simplify_points(tr.points, max_points)
        tracks_json.append({
            "target_mass": target,
            "initial_mass": round(target, 4),
            "model_mass": round(tr.initial_mass, 6),
            "filename": tr.filename,
            "feh": feh,
            "afe": afe,
            "vvcrit": vvcrit,
            "phases_present": describe_phases(pts),
            "points": pts
        })
    return {
        "source": "MIST v1.2 EEP evolutionary tracks",
        "source_url": url,
        "feh": feh,
        "afe": afe,
        "vvcrit": vvcrit,
        "tracks": tracks_json
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--masses", nargs="+", type=float, default=DEFAULT_MASSES)
    parser.add_argument("--feh", nargs="+", type=float, default=DEFAULT_FEH)
    parser.add_argument("--vvcrit", nargs="+", type=float, default=DEFAULT_VVCRIT)
    parser.add_argument("--afe", type=float, default=DEFAULT_AFE)
    parser.add_argument("--max-points", type=int, default=900)
    parser.add_argument("--version", default="v1.2")
    parser.add_argument("--outdir", default="data")
    parser.add_argument("--keep-txz", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "source": "MIST v1.2 EEP evolutionary tracks",
        "version": args.version,
        "afe": args.afe,
        "masses": args.masses,
        "datasets": []
    }

    for vvcrit in args.vvcrit:
        for feh in args.feh:
            url = mist_url(feh, args.afe, vvcrit, version=args.version)
            txz_bytes = download_bytes(url)

            if args.keep_txz:
                local_txz = outdir / Path(url).name
                local_txz.write_bytes(txz_bytes)

            tracks = load_tracks_from_txz(txz_bytes)
            if not tracks:
                print(f"Nessuna traccia valida per [Fe/H]={feh}, vvcrit={vvcrit}", file=sys.stderr)
                continue

            selected = select_nearest_tracks(tracks, args.masses)
            print(f"\nDataset [Fe/H]={feh:+.2f}, vvcrit={vvcrit:.1f}", file=sys.stderr)
            for target, tr in selected:
                last = tr.points[-1]
                print(
                    f"  Target {target:6.2f} -> modello {tr.initial_mass:8.3f} Msun | "
                    f"{len(tr.points):5d} punti | età finale {last['age_yr']:.3e} yr | "
                    f"{Path(tr.filename).name}",
                    file=sys.stderr
                )

            payload = build_payload(selected, feh, args.afe, vvcrit, url, args.max_points)
            fname = dataset_filename(feh, vvcrit)
            (outdir / fname).write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
            manifest["datasets"].append({
                "feh": feh,
                "vvcrit": vvcrit,
                "file": fname,
                "masses": [t["model_mass"] for t in payload["tracks"]]
            })

    (outdir / "manifest_mist_hr.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nScritto manifest: {outdir / 'manifest_mist_hr.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
