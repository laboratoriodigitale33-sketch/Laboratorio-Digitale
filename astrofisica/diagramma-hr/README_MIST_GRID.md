# Diagramma H-R interattivo con griglia MIST

Questa versione estende la simulazione in tre direzioni:

- più masse iniziali;
- più metallicità `[Fe/H]`;
- rotazione opzionale `v/vcrit`.

In più:
- gli assi del canvas si riscalano automaticamente al dataset caricato;
- la modalità temporale di default è **didattica bilanciata**, per comprimere le transizioni quasi istantanee
  (per esempio il tratto pre-cooling delle nane bianche) e mantenere leggibili le fasi rapide senza
  rallentamenti eccessivi.

## File inclusi

- `diagramma_hr_evoluzione_mist_grid.html`
- `prepare_mist_hr_grid.py`
- `README_MIST_GRID.md`

## Procedura minima

Apri un terminale nella cartella del pacchetto e lancia:

```bash
python prepare_mist_hr_grid.py
```

Questo genera:

```text
data/manifest_mist_hr.json
data/mist_feh_..._vvcrit_....json
```

Poi avvia un server locale:

```bash
python -m http.server 8000
```

e apri:

```text
http://localhost:8000/diagramma_hr_evoluzione_mist_grid.html
```

## Default consigliati

Lo script usa di default:

```text
masse: 0.6 0.8 1.0 1.2 1.5 2.0 3.0 5.0 8.0 15.0 20.0 40.0
[Fe/H]: -1.0 -0.5 0.0 +0.25
v/vcrit: 0.0
```

Questa scelta è abbastanza ricca ma non troppo pesante.

## Esempi

Più metallicità:

```bash
python prepare_mist_hr_grid.py --feh -2.0 -1.0 -0.5 0.0 0.25 0.5
```

Aggiunta della rotazione:

```bash
python prepare_mist_hr_grid.py --vvcrit 0.0 0.4
```

Più metallicità e rotazione:

```bash
python prepare_mist_hr_grid.py --feh -1.0 -0.5 0.0 0.25 --vvcrit 0.0 0.4
```

## Note sui pesi dei download

Ogni pacchetto MIST `EEP` per una singola combinazione di `[Fe/H]` e `v/vcrit`
può essere dell'ordine di ~80–110 MB.
Per questo motivo la pagina non carica tutto in un unico JSON, ma usa:

- un file `manifest_mist_hr.json`;
- un file JSON separato per ogni dataset `[Fe/H], v/vcrit`.

In questo modo il browser scarica solo il dataset che serve.

## Nota sul tempo evolutivo

La modalità **didattica bilanciata** usa una parametrizzazione intermedia:
- parte dalla lunghezza geometrica della traccia nel piano H-R;
- comprime i segmenti che hanno estensione geometrica ma quasi nessun avanzamento temporale;
- evita quindi di "spendere" troppo tempo animando lunghi tratti quasi istantanei.

Le modalità fisiche lineare e logaritmica in età restano disponibili.


## Aggiornamento 0.3.1

- corretta la posizione delle etichette 'Sequenza principale' e 'Nane bianche' nel canvas;
- metadati confermati e arricchiti nel file HTML;
- parte teorica ampliata con descrizione dettagliata delle fasi evolutive, del ruolo della massa, della metallicità e della rotazione.
