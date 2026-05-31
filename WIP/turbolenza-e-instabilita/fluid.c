/*
 * fluid.c — Solver Navier-Stokes 2D incomprimibile
 * Metodi: LBM D2Q9 per scenari con ostacoli, Stable Fluids per Kelvin-Helmholtz.
 *
 * Compilare con Emscripten:
 *   emcc fluid.c -O3 -o fluid.js \
 *     -s EXPORTED_FUNCTIONS='["_step","_compute_vorticity","_add_force","_add_turbulence","_set_params","_reset_fields","_build_obstacle","_get_ux","_get_uy","_get_vort","_get_pres","_get_dye","_get_wall","_NX","_NY"]' \
 *     -s EXPORTED_RUNTIME_METHODS='["cwrap","HEAPF32","HEAPU8"]' \
 *     -s ALLOW_MEMORY_GROWTH=1 \
 *     -s MODULARIZE=1 \
 *     -s EXPORT_NAME='FluidModule'
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── Dimensioni griglia ─────────────────────────────────────── */
#define NX 390
#define NY 208
#define N  (NX * NY)

int _NX = NX;
int _NY = NY;

/* ── Campi fisici ───────────────────────────────────────────── */
float _ux[N],   _uy[N];       /* velocità corrente              */
float _ux0[N],  _uy0[N];      /* buffer temporaneo              */
float _pres[N], _div[N];      /* pressione e divergenza         */
float _dye[N],  _dye0[N];     /* tracciante passivo             */
float _vort[N];               /* vorticità ω = duy/dx - dux/dy */
unsigned char _wall[N];       /* maschera solido (1=ostacolo)   */
float _lbm[9 * N], _lbm0[9 * N], _rho[N];

/* ── Parametri (modificabili a runtime) ─────────────────────── */
static float inlet_vel = 0.15f;
static float viscosity  = 0.0008f;
static float dt         = 1.0f;
static int kelvin_mode  = 0;
static int current_scenario = 0;

static const int   lbm_cx[9]  = {0, 1, 0,-1, 0, 1,-1,-1, 1};
static const int   lbm_cy[9]  = {0, 0, 1, 0,-1, 1, 1,-1,-1};
static const int   lbm_opp[9] = {0, 3, 4, 1, 2, 7, 8, 5, 6};
static const float lbm_w[9]   = {4.0f/9.0f, 1.0f/9.0f, 1.0f/9.0f, 1.0f/9.0f, 1.0f/9.0f,
                                 1.0f/36.0f, 1.0f/36.0f, 1.0f/36.0f, 1.0f/36.0f};
static float randf(void);

/* ── Indice con clamp ───────────────────────────────────────── */
static inline int IX(int x, int y) {
    if (x < 0)    x = 0;
    if (x >= NX)  x = NX - 1;
    if (y < 0)    y = 0;
    if (y >= NY)  y = NY - 1;
    return x + y * NX;
}

static inline int LBM(int q, int k) {
    return q * N + k;
}

static inline float lbm_feq(int q, float rho, float ux, float uy) {
    float cu = 3.0f * ((float)lbm_cx[q] * ux + (float)lbm_cy[q] * uy);
    float uu = 1.5f * (ux*ux + uy*uy);
    return lbm_w[q] * rho * (1.0f + cu + 0.5f*cu*cu - uu);
}

static int uses_lbm_solver(void) {
    return current_scenario == 0 || current_scenario == 1 ||
           current_scenario == 3 || current_scenario == 4;
}

/* ═══════════════════════════════════════════════════════════════
   CONDIZIONI AL CONTORNO
   Identiche alla versione JS che funzionava:
   - Inlet sinistro: velocità uniforme con profilo Poiseuille leggero
   - Outlet destro:  Neumann (copia dal vicino)
   - Scenari aperti: free-slip in alto/basso
   - Gradino:        canale no-slip
   - Ostacoli:       no-slip
═══════════════════════════════════════════════════════════════ */
static void apply_bc(float *ux, float *uy) {
    int i, j;

    if (kelvin_mode) {
        /* Shear layer: evita che l'inlet uniforme cancelli il controflusso. */
        for (j = 1; j < NY - 1; j++) {
            ux[IX(0, j)]    = ux[IX(NX-2, j)];
            uy[IX(0, j)]    = uy[IX(NX-2, j)];
            ux[IX(NX-1, j)] = ux[IX(1, j)];
            uy[IX(NX-1, j)] = uy[IX(1, j)];
        }
        for (i = 0; i < NX; i++) {
            ux[IX(i, 0)]    = ux[IX(i, 1)];
            uy[IX(i, 0)]    = 0.0f;
            ux[IX(i, NY-1)] = ux[IX(i, NY-2)];
            uy[IX(i, NY-1)] = 0.0f;
        }
        return;
    }

    /* Inlet */
    for (j = 1; j < NY - 1; j++) {
        ux[IX(0, j)] = inlet_vel;
        uy[IX(0, j)] = 0.0f;
    }
    /* Outlet: Neumann */
    for (j = 0; j < NY; j++) {
        ux[IX(NX-1, j)] = ux[IX(NX-2, j)];
        uy[IX(NX-1, j)] = uy[IX(NX-2, j)];
    }
    if (current_scenario == 4) {
        for (i = 0; i < NX; i++) {
            ux[IX(i, 0)]    = 0.0f;  uy[IX(i, 0)]    = 0.0f;
            ux[IX(i, NY-1)] = 0.0f;  uy[IX(i, NY-1)] = 0.0f;
        }
    } else {
        for (i = 0; i < NX; i++) {
            ux[IX(i, 0)]    = ux[IX(i, 1)];
            uy[IX(i, 0)]    = 0.0f;
            ux[IX(i, NY-1)] = ux[IX(i, NY-2)];
            uy[IX(i, NY-1)] = 0.0f;
        }
    }
    /* Ostacoli: no-slip */
    for (i = 0; i < N; i++) {
        int y = i / NX;
        if (current_scenario != 4 && (y == 0 || y == NY-1)) continue;
        if (_wall[i]) { ux[i] = 0.0f; uy[i] = 0.0f; }
    }
}

static void apply_dye_bc(float *dye) {
    int i, j;
    if (kelvin_mode) {
        for (j = 1; j < NY - 1; j++) {
            dye[IX(0, j)]    = dye[IX(NX-2, j)];
            dye[IX(NX-1, j)] = dye[IX(1, j)];
        }
        for (i = 0; i < NX; i++) {
            dye[IX(i, 0)]    = dye[IX(i, 1)];
            dye[IX(i, NY-1)] = dye[IX(i, NY-2)];
        }
    }
}

static void lbm_update_macros(void) {
    int i, j, q;
    for (j = 0; j < NY; j++) {
        for (i = 0; i < NX; i++) {
            int k = IX(i,j);
            if (_wall[k]) {
                _rho[k] = 1.0f;
                _ux[k] = 0.0f;
                _uy[k] = 0.0f;
                _pres[k] = 0.0f;
                _dye[k] = 0.0f;
                continue;
            }

            float rho = 0.0f, ux = 0.0f, uy = 0.0f;
            for (q = 0; q < 9; q++) {
                float f = _lbm[LBM(q,k)];
                rho += f;
                ux  += f * (float)lbm_cx[q];
                uy  += f * (float)lbm_cy[q];
            }
            if (rho < 1e-6f) rho = 1.0f;
            ux /= rho;
            uy /= rho;
            _rho[k] = rho;
            _ux[k] = ux;
            _uy[k] = uy;
            _pres[k] = rho - 1.0f;
            _dye[k] = sqrtf(ux*ux + uy*uy) * 8.0f;
        }
    }
}

static void lbm_init(void) {
    int i, j, q;
    memset(_pres, 0, N * sizeof(float));
    memset(_dye0, 0, N * sizeof(float));
    memset(_vort, 0, N * sizeof(float));

    float u0 = inlet_vel;
    if (u0 < 0.02f) u0 = 0.02f;
    if (u0 > 0.09f) u0 = 0.09f;

    for (j = 0; j < NY; j++) {
        for (i = 0; i < NX; i++) {
            int k = IX(i,j);
            float ux = _wall[k] ? 0.0f : u0;
            float uy = _wall[k] ? 0.0f : randf() * u0 * 0.050f;
            _rho[k] = 1.0f;
            for (q = 0; q < 9; q++) {
                _lbm[LBM(q,k)] = lbm_feq(q, 1.0f, ux, uy);
                _lbm0[LBM(q,k)] = _lbm[LBM(q,k)];
            }
        }
    }
    lbm_update_macros();
}

static void lbm_step(void) {
    int i, j, q;
    float u0 = inlet_vel;
    if (u0 < 0.02f) u0 = 0.02f;
    if (u0 > 0.09f) u0 = 0.09f;

    float tau = 0.5f + 3.0f * viscosity;
    if (tau < 0.515f) tau = 0.515f;
    if (tau > 1.50f) tau = 1.50f;
    float omega = 1.0f / tau;

    memset(_lbm0, 0, 9 * N * sizeof(float));

    for (j = 0; j < NY; j++) {
        for (i = 0; i < NX; i++) {
            int k = IX(i,j);
            if (_wall[k]) continue;

            float rho = 0.0f, ux = 0.0f, uy = 0.0f;
            for (q = 0; q < 9; q++) {
                float f = _lbm[LBM(q,k)];
                rho += f;
                ux  += f * (float)lbm_cx[q];
                uy  += f * (float)lbm_cy[q];
            }
            if (rho < 1e-6f) rho = 1.0f;
            ux /= rho;
            uy /= rho;
            if (i == 0) { rho = 1.0f; ux = u0; uy = 0.0f; }

            for (q = 0; q < 9; q++) {
                float post = _lbm[LBM(q,k)] - omega * (_lbm[LBM(q,k)] - lbm_feq(q, rho, ux, uy));
                int nx = i + lbm_cx[q];
                int ny = j + lbm_cy[q];

                if (ny < 0) ny = NY - 1;
                if (ny >= NY) ny = 0;
                if (nx < 0) continue;
                if (nx >= NX) continue;

                int nk = IX(nx, ny);
                if (_wall[nk]) {
                    _lbm0[LBM(lbm_opp[q], k)] += post;
                } else {
                    _lbm0[LBM(q, nk)] += post;
                }
            }
        }
    }

    for (j = 0; j < NY; j++) {
        int kin = IX(0,j);
        int kout = IX(NX-1,j);
        int kprev = IX(NX-2,j);
        if (!_wall[kin]) {
            for (q = 0; q < 9; q++) _lbm0[LBM(q,kin)] = lbm_feq(q, 1.0f, u0, 0.0f);
        }
        if (!_wall[kout]) {
            for (q = 0; q < 9; q++) _lbm0[LBM(q,kout)] = _lbm0[LBM(q,kprev)];
        }
    }

    memcpy(_lbm, _lbm0, 9 * N * sizeof(float));
    lbm_update_macros();
}

/* ═══════════════════════════════════════════════════════════════
   DIFFUSIONE IMPLICITA — Gauss-Seidel
   (I - ν·Δt·∇²) dst = src
═══════════════════════════════════════════════════════════════ */
static void diffuse(float *dst, const float *src, float nu, float dt_) {
    float a     = dt_ * nu;
    float c_inv = 1.0f / (1.0f + 4.0f * a);
    int i, j, iter;

    memcpy(dst, src, N * sizeof(float));

    for (iter = 0; iter < 4; iter++) {
        for (j = 1; j < NY - 1; j++) {
            for (i = 1; i < NX - 1; i++) {
                if (_wall[IX(i,j)]) continue;
                dst[IX(i,j)] = (src[IX(i,j)] + a * (
                    dst[IX(i-1,j)] + dst[IX(i+1,j)] +
                    dst[IX(i,j-1)] + dst[IX(i,j+1)]
                )) * c_inv;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   ADVECTION SEMI-LAGRANGIANA
   dst(x) = src(x - u·Δt)  con interpolazione bilineare
═══════════════════════════════════════════════════════════════ */
static void advect(float *dst, const float *src,
                   const float *ufx, const float *ufy, float dt_) {
    int i, j;
    for (j = 1; j < NY - 1; j++) {
        for (i = 1; i < NX - 1; i++) {
            if (_wall[IX(i,j)]) { dst[IX(i,j)] = 0.0f; continue; }

            float xi = (float)i - dt_ * ufx[IX(i,j)];
            float yi = (float)j - dt_ * ufy[IX(i,j)];

            if (kelvin_mode) {
                float span = (float)(NX - 2);
                while (xi < 1.0f)      xi += span;
                while (xi > NX - 2.0f) xi -= span;
            } else {
                if (xi < 0.5f)        xi = 0.5f;
                if (xi > NX - 1.5f)   xi = NX - 1.5f;
            }
            if (yi < 0.5f)        yi = 0.5f;
            if (yi > NY - 1.5f)   yi = NY - 1.5f;

            int i0 = (int)xi, i1 = i0 + 1;
            int j0 = (int)yi, j1 = j0 + 1;
            if (kelvin_mode && i1 >= NX - 1) i1 = 1;
            float sx = xi - i0, sy = yi - j0;

            dst[IX(i,j)] =
                (1.0f - sx) * ((1.0f - sy) * src[IX(i0,j0)] + sy * src[IX(i0,j1)]) +
                         sx  * ((1.0f - sy) * src[IX(i1,j0)] + sy * src[IX(i1,j1)]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   PROIEZIONE DI HELMHOLTZ-HODGE
   Rimuove la componente a divergenza non-nulla:
   1) div ← -∇·u
   2) ∇²p = div   (Poisson, Gauss-Seidel)
   3) u  ← u - ∇p
═══════════════════════════════════════════════════════════════ */
static void project(void) {
    int i, j, iter;

    /* divergenza */
    for (j = 1; j < NY - 1; j++) {
        for (i = 1; i < NX - 1; i++) {
            if (_wall[IX(i,j)]) {
                _div[IX(i,j)]  = 0.0f;
                _pres[IX(i,j)] = 0.0f;
                continue;
            }
            _div[IX(i,j)]  = -0.5f * (
                _ux[IX(i+1,j)] - _ux[IX(i-1,j)] +
                _uy[IX(i,j+1)] - _uy[IX(i,j-1)]
            );
            _pres[IX(i,j)] = 0.0f;
        }
    }

    /* Poisson per la pressione — 20 iterazioni come nella versione JS */
    for (iter = 0; iter < 20; iter++) {
        for (j = 1; j < NY - 1; j++) {
            for (i = 1; i < NX - 1; i++) {
                if (_wall[IX(i,j)]) continue;
                _pres[IX(i,j)] = (
                    _div[IX(i,j)] +
                    _pres[IX(i-1,j)] + _pres[IX(i+1,j)] +
                    _pres[IX(i,j-1)] + _pres[IX(i,j+1)]
                ) * 0.25f;
            }
        }
    }

    /* sottrai gradiente pressione */
    for (j = 1; j < NY - 1; j++) {
        for (i = 1; i < NX - 1; i++) {
            if (_wall[IX(i,j)]) continue;
            _ux[IX(i,j)] -= 0.5f * (_pres[IX(i+1,j)] - _pres[IX(i-1,j)]);
            _uy[IX(i,j)] -= 0.5f * (_pres[IX(i,j+1)] - _pres[IX(i,j-1)]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   STEP PRINCIPALE — identico alla versione JS
═══════════════════════════════════════════════════════════════ */
void step(void) {
    int k;

    if (uses_lbm_solver()) {
        lbm_step();
        return;
    }

    /* 1. Diffusione implicita velocità */
    diffuse(_ux0, _ux, viscosity, dt);
    diffuse(_uy0, _uy, viscosity, dt);
    apply_bc(_ux0, _uy0);

    /* 2. Self-advection */
    advect(_ux, _ux0, _ux0, _uy0, dt);
    advect(_uy, _uy0, _ux0, _uy0, dt);
    apply_bc(_ux, _uy);

    /* 3. Proiezione */
    project();
    apply_bc(_ux, _uy);

    /* 4. Advection tracciante passivo */
    advect(_dye0, _dye, _ux, _uy, dt);

    /* decay + ricarica inlet */
    for (k = 0; k < N; k++) {
        _dye0[k] *= kelvin_mode ? 0.9995f : 0.998f;
        if (_wall[k]) _dye0[k] = 0.0f;
    }
    apply_dye_bc(_dye0);

    /* swap dye */
    memcpy(_dye, _dye0, N * sizeof(float));
}

/* ═══════════════════════════════════════════════════════════════
   VORTICITÀ   ω positiva = rotazione antioraria (coordinate matematiche)
═══════════════════════════════════════════════════════════════ */
void compute_vorticity(void) {
    int i, j;
    for (j = 1; j < NY - 1; j++) {
        for (i = 1; i < NX - 1; i++) {
            /* y del canvas cresce verso il basso: inverti il curl di schermo
               per mostrare omega positiva come rotazione antioraria. */
            _vort[IX(i,j)] =
                (_ux[IX(i,j+1)] - _ux[IX(i,j-1)]) * 0.5f -
                (_uy[IX(i+1,j)] - _uy[IX(i-1,j)]) * 0.5f;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   INTERAZIONE ESTERNA
═══════════════════════════════════════════════════════════════ */
void add_force(int cx, int cy, float fx, float fy, int radius) {
    int di, dj;
    for (dj = -radius; dj <= radius; dj++) {
        for (di = -radius; di <= radius; di++) {
            if (di*di + dj*dj > radius*radius) continue;
            int xi = cx + di, yi = cy + dj;
            if (xi < 0 || xi >= NX || yi < 0 || yi >= NY) continue;
            if (_wall[IX(xi,yi)]) continue;
            float w = 1.0f - (float)(di*di+dj*dj) / (float)(radius*radius);
            _ux[IX(xi,yi)] += fx * w;
            _uy[IX(xi,yi)] += fy * w;
        }
    }
}

/* LCG per numeri pseudo-casuali leggero */
static unsigned int rng = 12345u;
static float randf(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 16) / 65535.0f - 0.5f;
}

void add_turbulence(float strength) {
    int i, j;
    for (j = NY/8; j < NY*7/8; j++)
        for (i = NX/8; i < NX*7/8; i++) {
            if (_wall[IX(i,j)]) continue;
            _ux[IX(i,j)] += randf() * strength;
            _uy[IX(i,j)] += randf() * strength;
        }
}

/* ═══════════════════════════════════════════════════════════════
   SETTER PARAMETRI
═══════════════════════════════════════════════════════════════ */
void set_params(float vel, float visc) {
    inlet_vel = vel;
    viscosity  = visc;
}

/* ═══════════════════════════════════════════════════════════════
   RESET CAMPI — esattamente come la versione JS
═══════════════════════════════════════════════════════════════ */
void reset_fields(void) {
    int i, j;
    memset(_ux,   0, N * sizeof(float));
    memset(_uy,   0, N * sizeof(float));
    memset(_ux0,  0, N * sizeof(float));
    memset(_uy0,  0, N * sizeof(float));
    memset(_pres, 0, N * sizeof(float));
    memset(_dye,  0, N * sizeof(float));
    memset(_dye0, 0, N * sizeof(float));

    if (uses_lbm_solver()) {
        lbm_init();
        return;
    }

    /* Inizializza flusso uniforme nelle celle libere */
    for (j = 1; j < NY-1; j++)
        for (i = 1; i < NX-1; i++)
            if (!_wall[IX(i,j)])
                _ux[IX(i,j)] = inlet_vel;

    /* Rumore iniziale minimo: rompe solo la simmetria numerica. */
    for (j = 1; j < NY-1; j++)
        for (i = 1; i < NX-1; i++)
            if (!_wall[IX(i,j)])
                _uy[IX(i,j)] = randf() * inlet_vel * 0.012f;

    /* Dye a strisce all'inlet */
    for (j = 1; j < NY-1; j++)
        _dye[IX(0,j)] = ((j / 12) % 2 == 0) ? 1.0f : 0.0f;
}

/* ═══════════════════════════════════════════════════════════════
   COSTRUZIONE OSTACOLI
   Tutte le build azzerano il wall e aggiungono le pareti.
   Il chiamante (JS) chiama poi reset_fields().
═══════════════════════════════════════════════════════════════ */
static void add_walls(void) {
    int i;
    if (current_scenario != 4) return;
    for (i = 0; i < NX; i++) {
        _wall[IX(i, 0)]    = 1;
        _wall[IX(i, NY-1)] = 1;
    }
}

/* Scenario 1: cilindro singolo — scia di Kármán */
void build_obstacle(int scenario, int param) {
    int i, j;
    memset(_wall, 0, N);
    current_scenario = scenario;
    kelvin_mode = (scenario == 5);

    int cx = NX * 28 / 100;
    int cy = NY / 2;
    int r  = param;   /* raggio o parametro geometrico */

    switch (scenario) {

    case 0: /* Cilindro singolo */
        for (j = 0; j < NY; j++)
            for (i = 0; i < NX; i++) {
                int dx = i-cx, dy = j-cy;
                if (dx*dx + dy*dy < r*r) _wall[IX(i,j)] = 1;
            }
        break;

    case 1: /* Due cilindri accoppiati: gap jet + scie interagenti */
        for (j = 0; j < NY; j++)
            for (i = 0; i < NX; i++) {
                int r2 = r * 9 / 10;
                int gap = r * 15 / 10;
                int off = r2 + gap / 2;
                int dx1 = i - (cx - r / 7);
                int dx2 = i - (cx + r / 7);
                int dy1 = j - (cy - off), dy2 = j - (cy + off);
                if (dx1*dx1+dy1*dy1 < r2*r2 || dx2*dx2+dy2*dy2 < r2*r2)
                    _wall[IX(i,j)] = 1;
            }
        break;

    case 2: /* Fessura / Venturi */
        {
            int gap  = param;
            int xpos = NX * 28 / 100;
            for (j = 0; j < NY; j++) {
                int inGap = (j > NY/2 - gap/2 && j < NY/2 + gap/2);
                if (!inGap)
                    for (i = xpos-2; i <= xpos+2; i++)
                        _wall[IX(i,j)] = 1;
            }
        }
        break;

    case 3: /* Griglia sfalsata di ostacoli: interazione fra scie */
        {
            int r2   = r * 48 / 100;
            int xs[] = {NX*24/100, NX*40/100, NX*56/100, NX*72/100};
            int pi, yi;
            if (r2 < 5) r2 = 5;
            for (j = 0; j < NY; j++) {
                for (i = 0; i < NX; i++) {
                    for (pi = 0; pi < 4; pi++) {
                        int rows = (pi % 2) ? 4 : 3;
                        for (yi = 0; yi < rows; yi++) {
                            int yperc = (pi % 2) ? 20 + yi * 20 : 30 + yi * 20;
                            int dx = i - xs[pi];
                            int dy = j - (NY * yperc / 100);
                            if (dx*dx + dy*dy < r2*r2) _wall[IX(i,j)] = 1;
                        }
                    }
                }
            }
        }
        break;

    case 4: /* Gradino — backward-facing step */
        for (j = 0; j < NY/2; j++)
            for (i = 0; i < NX/4; i++)
                _wall[IX(i,j)] = 1;
        break;

    case 5: /* Kelvin-Helmholtz: no ostacolo, shear layer */
        /* Nessun ostacolo. L'init del campo velocità va fatto separatamente. */
        break;
    }

    add_walls();
}

/* Inizializza il campo per Kelvin-Helmholtz:
   strato superiore a +v, inferiore a -v, interfaccia perturbata */
void init_kelvin_helmholtz(void) {
    int i, j;
    memset(_ux,   0, N * sizeof(float));
    memset(_uy,   0, N * sizeof(float));
    memset(_ux0,  0, N * sizeof(float));
    memset(_uy0,  0, N * sizeof(float));
    memset(_pres, 0, N * sizeof(float));
    memset(_dye,  0, N * sizeof(float));
    memset(_dye0, 0, N * sizeof(float));
    memset(_vort, 0, N * sizeof(float));

    float v0 = inlet_vel * 0.84f;
    float pi2 = 6.28318f;

    for (j = 0; j < NY; j++) {
        for (i = 0; i < NX; i++) {
            float x = (float)i / (float)NX;
            float mode = sinf(pi2 * 6.0f * x);
            float harmonic = sinf(pi2 * 12.0f * x + 0.35f);
            float fine = sinf(pi2 * 18.0f * x + 1.10f);
            float iface = NY * 0.50f + 5.6f * mode + 0.75f * harmonic;
            float yc = ((float)j - iface) / (NY * 0.031f);
            float sgn = tanhf(yc);
            float band = expf(-0.48f * yc * yc);

            if (_wall[IX(i,j)]) continue;
            /* Dominante pulita a sei onde: roll-up KH leggibili invece di creste spezzate. */
            float pert = v0 * band * (0.105f * mode + 0.018f * harmonic + 0.006f * fine);
            _ux[IX(i,j)] = -v0 * sgn;
            _uy[IX(i,j)] = pert;
            _dye[IX(i,j)] = 0.5f - 0.5f * tanhf(yc * 1.28f);
        }
    }
    apply_dye_bc(_dye);
}

/* Ricarica il dye all'inlet per scenari con flusso da sinistra */
void refresh_inlet_dye(int stripe_width) {
    int j;
    for (j = 1; j < NY-1; j++)
        if (!_wall[IX(0,j)])
            _dye[IX(0,j)] = ((j / stripe_width) % 2 == 0) ? 1.0f : 0.0f;
}

/* ═══════════════════════════════════════════════════════════════
   ACCESSOR — restituiscono puntatori ai buffer per JS/WASM
═══════════════════════════════════════════════════════════════ */
float         *get_ux(void)   { return _ux;   }
float         *get_uy(void)   { return _uy;   }
float         *get_vort(void) { return _vort; }
float         *get_pres(void) { return _pres; }
float         *get_dye(void)  { return _dye;  }
unsigned char *get_wall(void) { return _wall; }
