#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    from bs4 import BeautifulSoup
except ImportError as exc:
    raise SystemExit(
        "Errore: BeautifulSoup non disponibile. Installa con: pip install beautifulsoup4"
    ) from exc


@dataclass
class Issue:
    section: str
    severity: str
    nature: str
    title: str
    evidence: str
    suggestion: str


@dataclass
class MetadataSuggestion:
    found: bool
    valid: bool
    current_metadata: Optional[Dict[str, Any]] = None
    suggested_metadata: Optional[Dict[str, Any]] = None
    notes: List[str] = field(default_factory=list)


@dataclass
class StyleSuggestion:
    title: str
    evidence: str
    suggestion: str


@dataclass
class ReviewReport:
    simulation_path: str
    title: str
    inferred_topic: str
    physics_score: float
    didactic_score: float
    style_score: float
    summary: str
    physics_issues: List[Issue] = field(default_factory=list)
    didactic_issues: List[Issue] = field(default_factory=list)
    style_suggestions: List[StyleSuggestion] = field(default_factory=list)
    metadata: Optional[MetadataSuggestion] = None


STANDARD_METADATA_KEYS = [
    "title", "description", "category", "icon", "order", "tags", "level"
]

CATEGORY_DEFAULT_ICONS = {
    "Meccanica": "⚙️",
    "Onde": "≈",
    "Elettromagnetismo": "⚡",
    "Termodinamica": "♨️",
    "Ottica": "🔍",
    "Relativita": "⏱️",
    "Fisica moderna": "🧪",
    "Fisica nucleare": "☢️",
    "Astrofisica": "🌌",
    "Matematica per la fisica": "∇",
    "Misure e unita": "📏",
}

LEVEL_ALLOWED = {"base", "intermedio", "avanzato"}

TOPIC_KEYWORDS = {
    "uniformly_accelerated_motion": ["moto uniformemente accelerato", "accelerazione costante", "x0", "v0", "a", "v = v0 + a", "x = x0 + v0", "rettilineo"],
    "uniform_motion": ["moto rettilineo uniforme", "velocita costante", "v costante"],
    "projectile_motion": ["moto parabolico", "indipendenza dei moti", "traiettoria", "lancio"],
    "inclined_plane": ["piano inclinato", "theta", "angolo", "attrito", "normale", "mg sin", "mg cos"],
    "newton_dynamics": ["forza", "secondo principio", "terzo principio", "newton", "massa", "accelerazione"],
    "energy_conservation": ["energia cinetica", "energia potenziale", "energia meccanica", "conservazione dell'energia", "energia termica", "molla"],
    "simple_harmonic_motion": ["oscillatore", "armonico", "molla", "periodo", "frequenza", "cos(ωt", "sin(ωt"],
    "doppler_effect": ["doppler", "sorgente", "osservatore", "frequenza osservata", "boom sonico"],
    "waves": ["onda", "lunghezza d'onda", "interferenza", "diffrazione", "fronti d'onda"],
    "electric_field": ["campo elettrico", "linee di campo", "carica", "coulomb"],
    "magnetic_field": ["campo magnetico", "linee di campo", "lorentz", " tesla", " b "],
    "radioactive_decay": ["decadimento", "radioattivo", "emivita", "mezzo tempo", "attivita"],
}

TOPIC_TO_CATEGORY = {
    "uniformly_accelerated_motion": "Meccanica",
    "uniform_motion": "Meccanica",
    "projectile_motion": "Meccanica",
    "inclined_plane": "Meccanica",
    "newton_dynamics": "Meccanica",
    "energy_conservation": "Meccanica",
    "simple_harmonic_motion": "Meccanica",
    "doppler_effect": "Onde",
    "waves": "Onde",
    "electric_field": "Elettromagnetismo",
    "magnetic_field": "Elettromagnetismo",
    "radioactive_decay": "Fisica nucleare",
}

TOPIC_TO_TAGS = {
    "uniformly_accelerated_motion": ["cinematica", "accelerazione costante", "grafici orari", "moto rettilineo"],
    "uniform_motion": ["cinematica", "velocita costante", "moto rettilineo"],
    "projectile_motion": ["cinematica", "moto parabolico", "indipendenza dei moti", "gravita"],
    "inclined_plane": ["dinamica", "piano inclinato", "forze", "attrito", "scomposizione vettoriale"],
    "newton_dynamics": ["dinamica", "forza", "leggi di Newton", "massa", "accelerazione"],
    "energy_conservation": ["energia", "energia cinetica", "energia potenziale", "conservazione"],
    "simple_harmonic_motion": ["oscillazioni", "moto armonico", "periodo", "frequenza", "fase"],
    "doppler_effect": ["onde", "effetto Doppler", "frequenza", "sorgente", "osservatore"],
    "waves": ["onde", "frequenza", "lunghezza d'onda", "interferenza"],
    "electric_field": ["elettrostatica", "campo elettrico", "carica", "linee di campo"],
    "magnetic_field": ["magnetismo", "campo magnetico", "linee di campo", "forza di Lorentz"],
    "radioactive_decay": ["fisica nucleare", "decadimento radioattivo", "emivita", "attivita"],
}

TOPIC_TO_DESCRIPTION = {
    "uniformly_accelerated_motion": "Esplora il moto rettilineo ad accelerazione costante e osserva l'evoluzione di posizione e velocita nel tempo.",
    "uniform_motion": "Visualizza il moto rettilineo uniforme e il legame tra posizione, velocita costante e tempo.",
    "projectile_motion": "Esplora la composizione dei moti e la traiettoria di un corpo soggetto alla sola accelerazione gravitazionale.",
    "inclined_plane": "Analizza le forze su un corpo lungo un piano inclinato e l'effetto dell'attrito sul moto.",
    "newton_dynamics": "Visualizza il legame tra forza risultante, massa e accelerazione nei principali scenari dinamici.",
    "energy_conservation": "Osserva la trasformazione tra diverse forme di energia e verifica quando l'energia meccanica si conserva.",
    "simple_harmonic_motion": "Esplora le caratteristiche del moto armonico semplice, dalla fase al periodo, fino all'energia del sistema.",
    "doppler_effect": "Esplora come varia la frequenza osservata di un'onda quando sorgente e osservatore sono in moto relativo.",
    "waves": "Visualizza i principali fenomeni ondulatori, dalla propagazione all'interferenza, con enfasi sulle grandezze fisiche fondamentali.",
    "electric_field": "Esplora il campo elettrico generato da distribuzioni semplici di carica e la struttura delle linee di campo.",
    "magnetic_field": "Visualizza il campo magnetico e interpreta qualitativamente la geometria delle linee di campo.",
    "radioactive_decay": "Esplora il decadimento radioattivo e il significato fisico di attivita, costante di decadimento ed emivita.",
}

TOPIC_TO_DIDACTIC_ASSUMPTIONS = {
    "uniformly_accelerated_motion": [
        "Esplicitare che l'accelerazione e assunta costante in tutto l'intervallo temporale.",
        "Dichiarare con chiarezza la convenzione del segno per posizione, velocita e accelerazione.",
    ],
    "projectile_motion": [
        "Esplicitare che si trascura la resistenza dell'aria.",
        "Ricordare che la componente orizzontale della velocita resta costante solo in assenza di attrito.",
    ],
    "inclined_plane": [
        "Distinguere chiaramente tra componente del peso parallela al piano e reazione vincolare normale.",
        "Precisare se l'attrito simulato e statico, dinamico o un modello semplificato unico.",
    ],
    "energy_conservation": [
        "Specificare quando l'energia meccanica si conserva e quando invece diminuisce per effetto dissipativo.",
        "Evitare formulazioni del tipo 'l'energia si consuma': meglio parlare di trasformazione energetica.",
    ],
    "doppler_effect": [
        "Specificare il dominio di validita della formula usata, soprattutto nel caso del suono e dei regimi transonico/supersonico.",
        "Distinguere il caso delle onde meccaniche da quello elettromagnetico.",
    ],
    "radioactive_decay": [
        "Evitare di suggerire che il decadimento del singolo nucleo sia deterministico nel tempo.",
        "Distinguere tra comportamento probabilistico microscopico e legge esponenziale macroscopica.",
    ],
}


def normalize_spaces(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def safe_read_text(path: Path) -> str:
    for encoding in ("utf-8", "utf-8-sig", "cp1252", "latin-1"):
        try:
            return path.read_text(encoding=encoding)
        except UnicodeDecodeError:
            continue
    raise UnicodeDecodeError("unknown", b"", 0, 1, f"Impossibile leggere il file: {path}")


def clamp(value: float, vmin: float, vmax: float) -> float:
    return max(vmin, min(vmax, value))


@dataclass
class ParsedSimulation:
    path: Path
    raw_html: str
    soup: BeautifulSoup
    title: str
    visible_text: str
    script_text: str
    css_text: str
    metadata: Optional[Dict[str, Any]]
    canvas_count: int
    control_labels: List[str]
    found_formulas: List[str]
    slider_ranges: Dict[str, Tuple[Optional[float], Optional[float], Optional[float]]]


def extract_metadata(raw_html: str) -> Tuple[Optional[Dict[str, Any]], List[str], bool]:
    notes: List[str] = []
    match = re.search(r"window\.SIM_METADATA\s*=\s*(\{.*?\})\s*;", raw_html, flags=re.DOTALL)
    if not match:
        return None, notes, False
    raw_obj = match.group(1)
    try:
        metadata = json.loads(raw_obj)
        return metadata, notes, True
    except json.JSONDecodeError:
        notes.append("Blocco metadati trovato ma non in JSON rigoroso.")
    fallback = re.sub(r"(?<!\\)'", '"', raw_obj)
    fallback = re.sub(r",\s*([}\]])", r"\1", fallback)
    try:
        metadata = json.loads(fallback)
        notes.append("Metadati recuperati tramite fallback tollerante: conviene riscriverli in JSON rigoroso.")
        return metadata, notes, False
    except json.JSONDecodeError:
        notes.append("Impossibile interpretare il blocco metadati; suggerita riscrittura completa.")
        return None, notes, False


def parse_slider_ranges(soup: BeautifulSoup) -> Dict[str, Tuple[Optional[float], Optional[float], Optional[float]]]:
    ranges: Dict[str, Tuple[Optional[float], Optional[float], Optional[float]]] = {}
    for inp in soup.find_all("input"):
        if inp.get("type") != "range":
            continue
        name = inp.get("id") or inp.get("name") or f"slider_{len(ranges)+1}"
        try:
            vmin = float(inp.get("min")) if inp.get("min") is not None else None
        except ValueError:
            vmin = None
        try:
            vmax = float(inp.get("max")) if inp.get("max") is not None else None
        except ValueError:
            vmax = None
        try:
            vstep = float(inp.get("step")) if inp.get("step") is not None else None
        except ValueError:
            vstep = None
        ranges[name] = (vmin, vmax, vstep)
    return ranges


def find_formulas(text: str) -> List[str]:
    patterns = [
        r"[a-zA-Z]\s*=\s*[^\n;,<]{3,}",
        r"\b[a-zA-Z]+\([^)]*\)\s*=\s*[^\n;,<]{3,}",
        r"\bE\s*=\s*[^\n;,<]{3,}",
        r"\bv\s*=\s*[^\n;,<]{3,}",
        r"\bx\s*=\s*[^\n;,<]{3,}",
        r"\by\s*=\s*[^\n;,<]{3,}",
    ]
    found: List[str] = []
    for pat in patterns:
        for m in re.finditer(pat, text):
            candidate = normalize_spaces(m.group(0))
            if 4 <= len(candidate) <= 120 and candidate not in found:
                found.append(candidate)
    return found[:30]


def parse_simulation(path: Path) -> ParsedSimulation:
    raw_html = safe_read_text(path)
    soup = BeautifulSoup(raw_html, "html.parser")
    page_title = soup.title.string.strip() if soup.title and soup.title.string else path.stem
    visible_text = normalize_spaces(" ".join(soup.stripped_strings))
    script_text = "\n".join(tag.get_text("\n", strip=False) for tag in soup.find_all("script"))
    css_text = "\n".join(tag.get_text("\n", strip=False) for tag in soup.find_all("style"))
    metadata, _, _ = extract_metadata(raw_html)
    control_labels: List[str] = []
    for lab in soup.find_all("label"):
        txt = normalize_spaces(lab.get_text(" ", strip=True))
        if txt:
            control_labels.append(txt)
    return ParsedSimulation(
        path=path,
        raw_html=raw_html,
        soup=soup,
        title=page_title,
        visible_text=visible_text,
        script_text=script_text,
        css_text=css_text,
        metadata=metadata,
        canvas_count=len(soup.find_all("canvas")),
        control_labels=control_labels,
        found_formulas=find_formulas(raw_html),
        slider_ranges=parse_slider_ranges(soup),
    )


def infer_topic(parsed: ParsedSimulation) -> str:
    haystack = normalize_spaces(" ".join([
        parsed.title,
        parsed.visible_text,
        parsed.script_text,
        json.dumps(parsed.metadata, ensure_ascii=False) if parsed.metadata else "",
    ]).lower())
    scores: Dict[str, int] = {topic: 0 for topic in TOPIC_KEYWORDS}
    for topic, keywords in TOPIC_KEYWORDS.items():
        for kw in keywords:
            if kw.lower() in haystack:
                scores[topic] += 1
    best_topic = max(scores, key=scores.get)
    return best_topic if scores[best_topic] > 0 else "generic_physics"


def infer_level(parsed: ParsedSimulation, topic: str) -> str:
    text = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if any(m in text for m in ["lagrang", "hamilton", "schrodinger", "fourier", "bogoliubov", "lorentz"]):
        return "avanzato"
    if any(m in text for m in ["energia", "attrito", "campo", "frequenza", "fase", "vettore"]):
        return "intermedio"
    if topic in {"radioactive_decay", "doppler_effect", "electric_field", "magnetic_field"}:
        return "intermedio"
    return "base"


def suggest_icon(category: str, topic: str) -> str:
    mapping = {
        "projectile_motion": "➶",
        "simple_harmonic_motion": "∿",
        "doppler_effect": "🚑",
        "electric_field": "⚡",
        "magnetic_field": "🧲",
        "radioactive_decay": "☢️",
    }
    return mapping.get(topic, CATEGORY_DEFAULT_ICONS.get(category, "🔬"))


def suggest_metadata(parsed: ParsedSimulation, topic: str) -> MetadataSuggestion:
    current, notes, was_json_strict = extract_metadata(parsed.raw_html)
    found = current is not None
    valid = False
    if current is not None:
        missing = [k for k in STANDARD_METADATA_KEYS if k not in current]
        valid = not missing and isinstance(current.get("tags"), list) and current.get("level") in LEVEL_ALLOWED
        if missing:
            notes.append(f"Chiavi mancanti nei metadati: {', '.join(missing)}")
        if "tags" in current and not isinstance(current.get("tags"), list):
            notes.append("Il campo 'tags' dovrebbe essere una lista JSON.")
        if "level" in current and current.get("level") not in LEVEL_ALLOWED:
            notes.append("Il campo 'level' dovrebbe essere uno tra: base, intermedio, avanzato.")
        if not was_json_strict:
            notes.append("Conviene uniformare il blocco metadati a JSON rigoroso con doppi apici e virgole corrette.")
    category = TOPIC_TO_CATEGORY.get(topic, "Meccanica")
    title = current.get("title") if current and current.get("title") else parsed.title.replace("_", " ").strip()
    suggested = {
        "title": title,
        "description": TOPIC_TO_DESCRIPTION.get(topic, "Simulazione interattiva dedicata a un argomento di fisica, con attenzione agli aspetti qualitativi e quantitativi del fenomeno."),
        "category": category,
        "icon": suggest_icon(category, topic),
        "order": current.get("order", 999) if current else 999,
        "tags": current.get("tags", TOPIC_TO_TAGS.get(topic, ["fisica", "simulazione interattiva"])) if current else TOPIC_TO_TAGS.get(topic, ["fisica", "simulazione interattiva"]),
        "level": current.get("level", infer_level(parsed, topic)) if current else infer_level(parsed, topic),
    }
    return MetadataSuggestion(found=found, valid=valid, current_metadata=current, suggested_metadata=suggested, notes=notes)


def add_issue(issues: List[Issue], section: str, severity: str, nature: str, title: str, evidence: str, suggestion: str) -> None:
    issues.append(Issue(section, severity, nature, title, evidence, suggestion))


def detect_units_confusion(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    text = parsed.visible_text.lower()
    labels = " | ".join(parsed.control_labels).lower()
    script = parsed.script_text.lower()
    quantities = {
        "velocit": "m/s",
        "acceler": "m/s^2",
        "posizion": "m",
        "tempo": "s",
        "massa": "kg",
        "forza": "n",
        "energia": "j",
        "frequenza": "hz",
    }
    for stem, unit in quantities.items():
        if stem in text or stem in labels:
            window = text + " " + labels + " " + script[:2000]
            if unit.lower() not in window and unit.replace("^", "") not in window:
                add_issue(issues, "physics", "medium", "rischio didattico", f"Unita fisiche non esplicitate per grandezze legate a '{stem}'", f"Rilevate occorrenze di '{stem}' ma senza unita come '{unit}' nelle etichette principali.", f"Indicare chiaramente l'unita di misura associata a questa grandezza, ad esempio '{unit}'.")


def analyze_uniformly_accelerated_motion(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if not (("x0 + v0" in blob) or ("0.5*a" in blob) or ("a*t*t" in blob)):
        add_issue(issues, "physics", "high", "errore fisico", "Equazione oraria della posizione non riconosciuta o probabilmente assente", "Nel codice/testo non emergono in modo chiaro termini compatibili con x = x0 + v0 t + (1/2) a t^2.", "Verificare che la posizione dipenda quadraticamente dal tempo quando l'accelerazione e costante.")
    if not (("v0 + a" in blob) or ("v = v0" in blob)):
        add_issue(issues, "physics", "high", "errore fisico", "Legge della velocita non riconosciuta o probabilmente assente", "Nel codice/testo non emergono in modo chiaro termini compatibili con v = v0 + a t.", "Verificare che la velocita vari linearmente nel tempo nel modello ad accelerazione costante.")
    if not any(m in blob for m in ["asse", "segno", "positivo", "negativo"]):
        add_issue(issues, "didactic", "medium", "rischio didattico", "Convenzione dei segni non esplicitata", "Non sono stati trovati marcatori testuali chiari relativi alla convenzione dell'asse o dei segni.", "Specificare quale verso dell'asse e assunto positivo e come interpretare v0 e a con segno.")
    if parsed.canvas_count == 0:
        add_issue(issues, "physics", "medium", "incoerenza grafica", "Assenza di canvas o area grafica rilevata", "Non e stato trovato alcun elemento canvas nella simulazione.", "Se la simulazione e visiva, assicurarsi che la rappresentazione grafica delle grandezze sia effettivamente presente.")


def analyze_projectile_motion(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if "aria" not in blob and "attrito" not in blob and "resistenza" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Assenza di ipotesi sul ruolo della resistenza dell'aria", "Non emergono riferimenti espliciti all'assenza o presenza della resistenza del mezzo.", "Esplicitare se il modello trascura la resistenza dell'aria, altrimenti l'utente puo generalizzare in modo scorretto.")
    if "orizzont" in blob and "costante" not in blob and "vx" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Possibile mancata esplicitazione della costanza della componente orizzontale", "Si parla di moto/lancio ma non emerge chiaramente il ruolo della componente orizzontale della velocita.", "Chiarire che, in assenza di attrito, la componente orizzontale della velocita resta costante.")
    if "g" in blob and "9.81" not in blob and "grav" not in blob:
        add_issue(issues, "physics", "low", "miglioria espositiva", "Accelerazione gravitazionale non identificata chiaramente", "Compare il simbolo g o un riferimento implicito alla gravita senza una sua definizione chiara.", "Definire esplicitamente g come accelerazione di gravita e indicarne un valore tipico se opportuno.")


def analyze_inclined_plane(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if ("sin" not in blob and "cos" not in blob) and ("component" not in blob and "scompos" not in blob):
        add_issue(issues, "physics", "high", "errore fisico", "Scomposizione del peso non riconoscibile", "Non emergono riferimenti a componenti parallela e normale del peso lungo il piano inclinato.", "Mostrare o calcolare esplicitamente almeno le componenti mg sin(theta) e mg cos(theta), dove theta e l'angolo del piano.")
    if "attrito" in blob and "static" not in blob and "dinamic" not in blob and "μ" not in blob and "mu" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Modello di attrito non specificato", "Compare il termine 'attrito' senza dettagli sul modello o sul coefficiente usato.", "Precisare se l'attrito e statico, dinamico o una semplificazione unica, indicando il coefficiente coinvolto.")
    if "normale" not in blob and "reazione" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Reazione vincolare normale non esplicitata", "Non emergono riferimenti chiari alla forza normale o alla reazione del vincolo.", "Introdurre la forza normale e distinguerla chiaramente dalla componente del peso perpendicolare al piano.")


def analyze_energy_conservation(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    has_kin = ("energia cinetica" in blob) or re.search(r"\b0\.5\s*\*?\s*m\s*\*?\s*v", blob) is not None
    has_pot = ("energia potenziale" in blob) or ("mgh" in blob) or ("0.5*k*x*x" in blob) or ("1/2 k" in blob)
    has_total = ("energia meccanica" in blob) or ("energia totale" in blob)
    if has_kin and has_pot and not has_total:
        add_issue(issues, "physics", "medium", "rischio didattico", "Energia totale/meccanica non evidenziata chiaramente", "Si trovano riferimenti a energia cinetica e potenziale ma non a una loro sintesi come energia meccanica totale.", "Aggiungere la grandezza E_mecc = K + U, dove E_mecc e l'energia meccanica totale, K l'energia cinetica e U l'energia potenziale.")
    if "attrito" not in blob and "termic" in blob:
        add_issue(issues, "physics", "medium", "errore fisico", "Energia termica menzionata senza meccanismo dissipativo esplicito", "Si trovano riferimenti a energia termica ma non emergono forze dissipative o attrito nel modello.", "Introdurre esplicitamente un meccanismo dissipativo o rimuovere il richiamo all'energia termica se il sistema e conservativo.")
    if "consuma" in blob and "energia" in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Formulazione didatticamente fuorviante sull'energia", "Rilevato uso di espressioni vicine all'idea che l'energia 'si consumi'.", "Preferire una formulazione centrata sulla trasformazione tra forme di energia, non sulla scomparsa dell'energia.")


def analyze_simple_harmonic_motion(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if "periodo" not in blob and "frequenza" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Periodo e frequenza non evidenziati", "Per una simulazione di oscillazioni non emergono riferimenti chiari a periodo o frequenza.", "Introdurre periodo T e frequenza f, dove T e il tempo di un'oscillazione completa e f = 1/T.")
    if "ampiezza" not in blob and "fase" not in blob:
        add_issue(issues, "didactic", "low", "miglioria espositiva", "Ampiezza e fase non rese esplicite", "Non emergono riferimenti chiari ad ampiezza e fase del moto armonico.", "Esplicitare almeno le grandezze ampiezza e fase per collegare meglio rappresentazione e modello matematico.")


def analyze_doppler_effect(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if not any(m in blob for m in ["340", "343", "velocita del suono", "c_sound", "csound"]):
        add_issue(issues, "physics", "medium", "rischio didattico", "Velocita di propagazione del suono non identificata chiaramente", "Non emergono riferimenti espliciti a una velocita del suono di riferimento o a una variabile equivalente.", "Introdurre esplicitamente la velocita del suono nel mezzo, poiche delimita il regime di validita del modello classico nel caso acustico.")
    for name, (_, vmax, _) in parsed.slider_ranges.items():
        lname = name.lower()
        if any(k in lname for k in ["source", "sorg", "vs", "velocita"]):
            if vmax is not None and vmax >= 340 and "subson" not in blob and "transson" not in blob and "superson" not in blob:
                add_issue(issues, "physics", "high", "errore fisico", "Range parametrico compatibile con regime supersonico senza gestione esplicita del cambio di regime", f"Lo slider '{name}' arriva a vmax = {vmax}, compatibile con velocita comparabili o superiori a quella del suono.", "Quando la velocita della sorgente raggiunge o supera la velocita del suono, segnalare esplicitamente il cambio di regime e limitare l'uso ingenuo della formula classica.")
    if "elettromagnet" in blob and "relativ" not in blob:
        add_issue(issues, "physics", "high", "errore fisico", "Caso elettromagnetico menzionato senza distinzione dal Doppler classico acustico", "Compare un riferimento a onde elettromagnetiche senza indicazioni su una trattazione relativistica o su una separazione dei casi.", "Separare chiaramente il caso acustico classico da quello elettromagnetico, che richiede una trattazione diversa.")


def analyze_waves(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    for concept in ["frequenza", "lunghezza d'onda", "ampiezza"]:
        if concept not in blob:
            add_issue(issues, "didactic", "low", "miglioria espositiva", f"Grandezza ondulatoria '{concept}' non resa esplicita", f"Non emerge un riferimento chiaro a '{concept}'.", f"Considerare l'introduzione esplicita della grandezza '{concept}' per rendere piu robusta la lettura fisica del fenomeno.")


def analyze_radioactive_decay(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if "espon" not in blob and "exp(" not in blob and "e^-" not in blob and "emivita" not in blob:
        add_issue(issues, "physics", "high", "errore fisico", "Legge esponenziale del decadimento non riconoscibile", "Non emergono riferimenti chiari a una legge di decadimento esponenziale o all'emivita.", "Esplicitare una legge del tipo N(t) = N0 exp(-lambda t), dove N e il numero di nuclei non decaduti, N0 il valore iniziale, lambda la costante di decadimento e t il tempo.")
    if "probabil" not in blob and "casual" not in blob and "stocast" not in blob:
        add_issue(issues, "didactic", "medium", "rischio didattico", "Carattere probabilistico del decadimento non esplicitato", "Non emergono riferimenti al fatto che il decadimento del singolo nucleo sia un processo probabilistico.", "Sottolineare che l'andamento esponenziale emerge a livello statistico, mentre il singolo decadimento non e prevedibile deterministicamente.")


def analyze_generic_physics(parsed: ParsedSimulation, issues: List[Issue]) -> None:
    blob = (parsed.visible_text + "\n" + parsed.script_text).lower()
    if parsed.canvas_count == 0:
        add_issue(issues, "physics", "low", "miglioria espositiva", "Nessun canvas rilevato", "Non e stato trovato alcun elemento canvas. Potrebbe essere normale, ma limita il controllo grafico automatico.", "Se la simulazione e visuale, verificare che l'area grafica sia effettivamente presente e chiaramente collegata alle grandezze fisiche.")
    if "formula" not in blob and not parsed.found_formulas:
        add_issue(issues, "didactic", "low", "miglioria espositiva", "Poche formule o relazioni esplicite rilevate", "Il parser non ha individuato formule evidenti nel testo o nel codice visibile.", "Valutare se introdurre relazioni quantitative esplicite per collegare la simulazione al modello fisico sottostante.")


def run_physics_checks(parsed: ParsedSimulation, topic: str) -> Tuple[List[Issue], List[Issue]]:
    all_issues: List[Issue] = []
    detect_units_confusion(parsed, all_issues)
    if topic == "uniformly_accelerated_motion":
        analyze_uniformly_accelerated_motion(parsed, all_issues)
    elif topic == "projectile_motion":
        analyze_projectile_motion(parsed, all_issues)
    elif topic == "inclined_plane":
        analyze_inclined_plane(parsed, all_issues)
    elif topic == "energy_conservation":
        analyze_energy_conservation(parsed, all_issues)
    elif topic == "simple_harmonic_motion":
        analyze_simple_harmonic_motion(parsed, all_issues)
    elif topic == "doppler_effect":
        analyze_doppler_effect(parsed, all_issues)
    elif topic == "waves":
        analyze_waves(parsed, all_issues)
    elif topic == "radioactive_decay":
        analyze_radioactive_decay(parsed, all_issues)
    else:
        analyze_generic_physics(parsed, all_issues)
    physics_issues = [issue for issue in all_issues if issue.section != "didactic"]
    didactic_issues = [issue for issue in all_issues if issue.section == "didactic"]
    for suggestion in TOPIC_TO_DIDACTIC_ASSUMPTIONS.get(topic, []):
        didactic_issues.append(Issue("didactic", "low", "miglioria espositiva", "Ipotesi del modello da rendere esplicita", f"Per il tema '{topic}' e didatticamente utile esplicitare le assunzioni del modello.", suggestion))
    return physics_issues, didactic_issues


def analyze_style(parsed: ParsedSimulation) -> List[StyleSuggestion]:
    suggestions: List[StyleSuggestion] = []
    css = parsed.css_text.lower()
    html = parsed.raw_html.lower()
    if "background" not in css:
        suggestions.append(StyleSuggestion("Background non esplicitato", "Nel CSS inline non emerge una gestione chiara del background della pagina.", "Per uniformita con il sito, considerare un tema scuro con sfondo curato e contrasto leggibile."))
    if parsed.canvas_count > 0 and ("border-radius" not in css and "box-shadow" not in css):
        suggestions.append(StyleSuggestion("Canvas/pannelli poco coerenti con lo stile del sito", "Non emergono proprieta come border-radius o box-shadow nei blocchi stilistici principali.", "Uniformare i pannelli principali a card scure con angoli arrotondati, ombre morbide e gerarchia visiva pulita."))
    if "mathjax" not in html and (r"\(" in parsed.raw_html or r"\[" in parsed.raw_html or "$" in parsed.raw_html):
        suggestions.append(StyleSuggestion("Formule presenti ma motore LaTeX non riconosciuto", "Sono presenti marcatori che suggeriscono formule, ma non e stato riconosciuto un caricamento evidente di MathJax o analogo.", "Se vuoi formule nello stile delle altre simulazioni, assicurati che il rendering matematico sia caricato e configurato correttamente."))
    controls_blob = " ".join(parsed.control_labels).lower()
    if parsed.slider_ranges and not any(unit in controls_blob for unit in ["m/s", "m", "s", "n", "j", "hz", "kg"]):
        suggestions.append(StyleSuggestion("Controlli senza unita visibili", "Sono stati rilevati slider ma le etichette non sembrano riportare unita fisiche esplicite.", "Uniformare i controlli alle altre simulazioni indicando unita fisiche e valori correnti in modo chiaro."))
    if "panel" not in html and "card" not in html:
        suggestions.append(StyleSuggestion("Struttura dei pannelli non riconoscibile", "Nel markup non emergono classi o sezioni chiaramente organizzate in pannelli/card.", "Per coerenza con il sito, organizzare teoria, controlli, canvas e grafici in blocchi visivi separati ma armonizzati."))
    return suggestions


def weighted_score(issues: List[Issue], baseline: float = 10.0) -> float:
    penalty = 0.0
    for issue in issues:
        penalty += 2.0 if issue.severity == "high" else 1.0 if issue.severity == "medium" else 0.4
    return clamp(baseline - penalty, 0.0, 10.0)


def build_summary(topic: str, physics_issues: List[Issue], didactic_issues: List[Issue], style_suggestions: List[StyleSuggestion]) -> str:
    high_phys = sum(1 for x in physics_issues if x.severity == "high")
    med_phys = sum(1 for x in physics_issues if x.severity == "medium")
    core = "Non emergono criticita fisiche gravi o medie con le euristiche attuali" if (high_phys == 0 and med_phys == 0) else f"Sono emerse {high_phys} criticita fisiche alte e {med_phys} medie"
    return f"Argomento inferito: {topic}. {core}. Suggerimenti didattici: {len(didactic_issues)}. Suggerimenti di stile/coerenza col sito: {len(style_suggestions)}."


def render_markdown(report: ReviewReport) -> str:
    md: List[str] = []
    md.append("# Report revisione simulazione\n")
    md.append(f"**File:** `{report.simulation_path}`  ")
    md.append(f"**Titolo:** {report.title}  ")
    md.append(f"**Argomento inferito:** {report.inferred_topic}  ")
    md.append(f"**Punteggio fisica:** {report.physics_score:.1f}/10  ")
    md.append(f"**Punteggio didattica:** {report.didactic_score:.1f}/10  ")
    md.append(f"**Punteggio stile:** {report.style_score:.1f}/10\n")
    md.append(f"{report.summary}\n")
    md.append("## 1. Problemi fisici e incoerenze fisico-grafiche\n")
    if not report.physics_issues:
        md.append("Nessun problema fisico rilevante individuato con le regole attuali.\n")
    else:
        for i, issue in enumerate(report.physics_issues, start=1):
            md.append(f"### {i}. {issue.title}")
            md.append(f"- **Severita:** {issue.severity}")
            md.append(f"- **Natura:** {issue.nature}")
            md.append(f"- **Evidenza:** {issue.evidence}")
            md.append(f"- **Suggerimento:** {issue.suggestion}\n")
    md.append("## 2. Suggerimenti didattici\n")
    if not report.didactic_issues:
        md.append("Nessun suggerimento didattico prioritario rilevato.\n")
    else:
        for i, issue in enumerate(report.didactic_issues, start=1):
            md.append(f"### {i}. {issue.title}")
            md.append(f"- **Severita:** {issue.severity}")
            md.append(f"- **Natura:** {issue.nature}")
            md.append(f"- **Evidenza:** {issue.evidence}")
            md.append(f"- **Suggerimento:** {issue.suggestion}\n")
    md.append("## 3. Metadati\n")
    if report.metadata is None:
        md.append("Nessuna analisi metadata disponibile.\n")
    else:
        md.append(f"- **Trovati:** {'si' if report.metadata.found else 'no'}")
        md.append(f"- **Validi secondo lo schema standard:** {'si' if report.metadata.valid else 'no'}\n")
        if report.metadata.notes:
            md.append("### Note")
            for note in report.metadata.notes:
                md.append(f"- {note}")
            md.append("")
        if report.metadata.current_metadata:
            md.append("### Metadati attuali")
            md.append("```json")
            md.append(json.dumps(report.metadata.current_metadata, ensure_ascii=False, indent=2))
            md.append("```\n")
        if report.metadata.suggested_metadata:
            md.append("### Metadati suggeriti")
            md.append("```json")
            md.append(json.dumps(report.metadata.suggested_metadata, ensure_ascii=False, indent=2))
            md.append("```\n")
    md.append("## 4. Uniformita di stile\n")
    if not report.style_suggestions:
        md.append("Nessun suggerimento di stile prioritario rilevato.\n")
    else:
        for i, s in enumerate(report.style_suggestions, start=1):
            md.append(f"### {i}. {s.title}")
            md.append(f"- **Evidenza:** {s.evidence}")
            md.append(f"- **Suggerimento:** {s.suggestion}\n")
    return "\n".join(md).strip() + "\n"


def render_console(report: ReviewReport) -> str:
    lines: List[str] = []
    lines.append("=" * 72)
    lines.append("REVIEW SIMULAZIONE")
    lines.append("=" * 72)
    lines.append(f"File:     {report.simulation_path}")
    lines.append(f"Titolo:   {report.title}")
    lines.append(f"Argomento inferito: {report.inferred_topic}")
    lines.append(f"Fisica:   {report.physics_score:.1f}/10")
    lines.append(f"Didattica:{report.didactic_score:.1f}/10")
    lines.append(f"Stile:    {report.style_score:.1f}/10")
    lines.append("")
    lines.append(report.summary)
    lines.append("")
    lines.append("[1] Problemi fisici / fisico-grafici")
    if not report.physics_issues:
        lines.append("  - Nessun problema rilevante individuato con le euristiche attuali.")
    else:
        for issue in report.physics_issues:
            lines.append(f"  - ({issue.severity.upper()}) {issue.title}")
            lines.append(f"    Evidenza: {issue.evidence}")
            lines.append(f"    Suggerimento: {issue.suggestion}")
    lines.append("")
    lines.append("[2] Suggerimenti didattici")
    if not report.didactic_issues:
        lines.append("  - Nessun suggerimento didattico prioritario.")
    else:
        for issue in report.didactic_issues:
            lines.append(f"  - ({issue.severity.upper()}) {issue.title}")
            lines.append(f"    Evidenza: {issue.evidence}")
            lines.append(f"    Suggerimento: {issue.suggestion}")
    lines.append("")
    lines.append("[3] Metadati")
    if report.metadata:
        lines.append(f"  - Trovati: {'si' if report.metadata.found else 'no'}")
        lines.append(f"  - Validi: {'si' if report.metadata.valid else 'no'}")
        if report.metadata.notes:
            for note in report.metadata.notes:
                lines.append(f"    Nota: {note}")
        if report.metadata.suggested_metadata:
            lines.append("  - Proposta metadati:")
            proposed = json.dumps(report.metadata.suggested_metadata, ensure_ascii=False, indent=2)
            for row in proposed.splitlines():
                lines.append(f"    {row}")
    lines.append("")
    lines.append("[4] Uniformita di stile")
    if not report.style_suggestions:
        lines.append("  - Nessun suggerimento di stile prioritario.")
    else:
        for suggestion in report.style_suggestions:
            lines.append(f"  - {suggestion.title}")
            lines.append(f"    Evidenza: {suggestion.evidence}")
            lines.append(f"    Suggerimento: {suggestion.suggestion}")
    return "\n".join(lines)


def review_simulation(path: Path) -> ReviewReport:
    parsed = parse_simulation(path)
    topic = infer_topic(parsed)
    metadata = suggest_metadata(parsed, topic)
    physics_issues, didactic_issues = run_physics_checks(parsed, topic)
    style_suggestions = analyze_style(parsed)
    physics_score = weighted_score(physics_issues)
    didactic_score = weighted_score(didactic_issues)
    style_issue_wrapped = [Issue("style", "medium", "miglioria espositiva", s.title, s.evidence, s.suggestion) for s in style_suggestions]
    style_score = weighted_score(style_issue_wrapped)
    title = metadata.suggested_metadata["title"] if metadata and metadata.suggested_metadata else parsed.title
    summary = build_summary(topic, physics_issues, didactic_issues, style_suggestions)
    return ReviewReport(
        simulation_path=str(path),
        title=title,
        inferred_topic=topic,
        physics_score=physics_score,
        didactic_score=didactic_score,
        style_score=style_score,
        summary=summary,
        physics_issues=physics_issues,
        didactic_issues=didactic_issues,
        style_suggestions=style_suggestions,
        metadata=metadata,
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Reviewer on-demand per una singola simulazione HTML, con focus su errori fisici e suggerimenti didattici.")
    parser.add_argument("simulation", type=str, help="Percorso del file HTML da analizzare")
    parser.add_argument("--report-md", type=str, default=None, help="Percorso file Markdown in cui salvare il report")
    parser.add_argument("--report-json", type=str, default=None, help="Percorso file JSON in cui salvare il report strutturato")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    path = Path(args.simulation)
    if not path.exists():
        raise SystemExit(f"Errore: file non trovato: {path}")
    if path.suffix.lower() != ".html":
        raise SystemExit("Errore: il reviewer attuale si aspetta un file .html")
    report = review_simulation(path)
    print(render_console(report))
    if args.report_md:
        md_path = Path(args.report_md)
        md_path.write_text(render_markdown(report), encoding="utf-8")
        print(f"\nReport Markdown salvato in: {md_path}")
    if args.report_json:
        json_path = Path(args.report_json)
        json_path.write_text(json.dumps(asdict(report), ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Report JSON salvato in: {json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
