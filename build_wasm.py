#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DEFAULT_EMCC = Path.home() / "emsdk" / "upstream" / "emscripten" / "emcc.py"


MODULES = {
    "collasso": {
        "label": "Collasso stellare",
        "cwd": ROOT / "WIP" / "collasso_stellare",
        "temp": ROOT / "emscripten_tmp" / "collasso_stellare",
        "outputs": ["sim.js", "sim.wasm"],
        "args": [
            "sim.c",
            "-o",
            "sim.js",
            "-s",
            'EXPORTED_FUNCTIONS=["_sim_set_params","_sim_init","_sim_step","_sim_get_state","_sim_get_diagnostics","_sim_get_phase","_sim_get_N"]',
            "-s",
            'EXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPF32","HEAPU8","HEAP8"]',
            "-s",
            "MODULARIZE=1",
            "-s",
            "EXPORT_NAME=SimModule",
            "-s",
            "ALLOW_MEMORY_GROWTH=1",
            "-s",
            "ASSERTIONS=1",
            "-O2",
            "-lm",
        ],
    },
    "kerr": {
        "label": "Curvatura spazio-tempo",
        "cwd": ROOT / "WIP" / "curvatura_spazio-tempo",
        "temp": ROOT / "emscripten_tmp" / "kerr",
        "outputs": ["kerr.js", "kerr.wasm"],
        "args": [
            "kerr.c",
            "-o",
            "kerr.js",
            "-O3",
            "-msimd128",
            "-s",
            "WASM=1",
            "-s",
            'EXPORTED_FUNCTIONS=["_main","_render_frame","_set_spin","_set_zoom","_set_pitch","_set_mode","_set_nrays","_set_remit","_step_anim","_get_rp","_get_rergo","_get_risco","_get_rph","_get_omH","_get_width","_get_height","_get_pixel_buffer"]',
            "-s",
            'EXPORTED_RUNTIME_METHODS=["ccall","cwrap"]',
            "-s",
            "ALLOW_MEMORY_GROWTH=1",
            "-lm",
        ],
    },
    "transport": {
        "label": "Trasporto particelle",
        "cwd": ROOT / "WIP" / "trasporto-particelle",
        "temp": ROOT / "emscripten_tmp" / "transport",
        "outputs": ["transport.js"],
        "args": [
            "transport.c",
            "-O3",
            "-o",
            "transport.js",
            "-s",
                'EXPORTED_FUNCTIONS=["_sim_init","_sim_step","_sim_reset","_reset_tallies","_set_source","_get_fluence_n","_get_fluence_p","_get_fluence_e","_get_dose","_get_tracks_buffer","_get_track_count","_get_track_stride","_get_total_histories","_get_total_fissions","_get_total_captures","_get_total_scatterings","_set_geometry","_get_mat_grid","_get_spec_neut","_get_spec_phot","_query_material_response"]',
            "-s",
            'EXPORTED_RUNTIME_METHODS=["ccall","cwrap"]',
            "-s",
            "ALLOW_MEMORY_GROWTH=1",
            "-s",
            "INITIAL_MEMORY=67108864",
            "-s",
            "ENVIRONMENT=web",
            "-s",
            "SINGLE_FILE=1",
        ],
    },
    "fluid": {
        "label": "Turbolenza e instabilita",
        "cwd": ROOT / "WIP" / "turbolenza-e-instabilita",
        "temp": ROOT / "emscripten_tmp" / "fluid",
        "outputs": ["fluid.js", "fluid.wasm"],
        "args": [
            "fluid.c",
            "-O3",
            "-o",
            "fluid.js",
            "-s",
            'EXPORTED_FUNCTIONS=["_step","_compute_vorticity","_add_force","_add_turbulence","_set_params","_reset_fields","_build_obstacle","_init_kelvin_helmholtz","_refresh_inlet_dye","_get_ux","_get_uy","_get_vort","_get_pres","_get_dye","_get_wall"]',
            "-s",
            'EXPORTED_RUNTIME_METHODS=["cwrap","HEAPF32","HEAPU8"]',
            "-s",
            "ALLOW_MEMORY_GROWTH=1",
            "-s",
            "MODULARIZE=1",
            "-s",
            "EXPORT_NAME=FluidModule",
            "-lm",
        ],
    },
}

ALIASES = {
    "all": list(MODULES),
    "collasso_stellare": ["collasso"],
    "curvatura": ["kerr"],
    "curvatura_spazio_tempo": ["kerr"],
    "trasporto": ["transport"],
    "trasporto_particelle": ["transport"],
    "turbolenza": ["fluid"],
    "fluidodinamica": ["fluid"],
}


def resolve_emcc() -> Path:
    emcc_override = os.environ.get("EMCC_PY")
    emcc = Path(emcc_override) if emcc_override else DEFAULT_EMCC
    if not emcc.exists():
        raise FileNotFoundError(
            f"emcc.py non trovato: {emcc}\n"
            "Imposta la variabile d'ambiente EMCC_PY oppure installa/attiva emsdk in "
            f"{DEFAULT_EMCC.parent}"
        )
    return emcc


def resolve_targets(argv: list[str]) -> list[str]:
    if not argv:
        return list(MODULES)

    resolved: list[str] = []
    for raw in argv:
        key = raw.strip().casefold().replace("-", "_")
        if key in MODULES:
            resolved.append(key)
            continue
        if key in ALIASES:
            resolved.extend(ALIASES[key])
            continue
        raise SystemExit(
            f"Target sconosciuto: {raw}\n"
            f"Valori ammessi: all, {', '.join(MODULES)}"
        )

    seen: set[str] = set()
    ordered: list[str] = []
    for key in resolved:
        if key not in seen:
            seen.add(key)
            ordered.append(key)
    return ordered


def run_build(name: str, emcc: Path) -> None:
    spec = MODULES[name]
    temp_dir = spec["temp"]
    temp_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["TEMP"] = str(temp_dir)
    env["TMP"] = str(temp_dir)
    env["EMCC_TEMP_DIR"] = str(temp_dir)

    cmd = [sys.executable, "-E", str(emcc), *spec["args"]]
    print(f"\n==> {spec['label']}")
    print(f"cwd: {spec['cwd']}")
    print("cmd:", " ".join(cmd))

    subprocess.run(cmd, cwd=spec["cwd"], env=env, check=True)

    missing = [name for name in spec["outputs"] if not (spec["cwd"] / name).exists()]
    if missing:
        raise FileNotFoundError(
            f"Build completata ma mancano gli output attesi in {spec['cwd']}: {', '.join(missing)}"
        )

    print("ok:", ", ".join(spec["outputs"]))


def main(argv: list[str]) -> int:
    emcc = resolve_emcc()
    targets = resolve_targets(argv)

    for name in targets:
        run_build(name, emcc)

    print("\nBuild completata.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode)
