/*
 * diffrazione.c
 * Simulazione 2D di diffrazione d'onda con Angular Spectrum Method (ASM)
 *
 * Metodo: Angular Spectrum Method
 *   U(x,y,z) = IFFT2 [ FFT2[U(x,y,0)] * H(fx,fy,z) ]
 *   H(fx,fy,z) = exp(i*2π*z * sqrt(1/λ² - fx² - fy²))  se fx²+fy² < 1/λ²
 *              = exp(-2π*z * sqrt(fx²+fy² - 1/λ²))       evanescente (soppresso)
 *
 * Tutte le unità sono SI (metri) internamente.
 * La griglia è NxN punti, con passo dx = L/N (L = dimensione fisica del piano)
 *
 * Author: simulation engine for diffrazione.html
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* ================================================================
 * Configurazione griglia
 * ================================================================ */
#define N 512           /* lato griglia (potenza di 2) */
#define N2 (N*N)

/* Struttura complessa float */
typedef struct { float r, i; } cpx;

/* Buffer statici (evita malloc ripetuti) */
static cpx g_field[N2];      /* campo U(x,y) piano sorgente */
static cpx g_work[N2];       /* buffer di lavoro FFT */
static cpx g_out[N2];        /* campo propagato U(x,y,z) */

/* Output esposto a JS */
static float g_intensity[N2];
static float g_phase[N2];
static float g_amplitude[N2];
static float g_field_re[N2];
static float g_field_im[N2];

/* ================================================================
 * FFT 2D in-place, Cooley-Tukey radix-2
 * Algoritmo classico iterativo (bit-reversal + butterfly)
 * ================================================================ */

static void fft1d(cpx* x, int n, int inverse) {
    /* Bit-reversal permutation */
    int j = 0;
    for (int i = 1; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { cpx tmp = x[i]; x[i] = x[j]; x[j] = tmp; }
    }
    /* Butterfly stages */
    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * (float)M_PI / len * (inverse ? 1.0f : -1.0f);
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_r = 1.0f, cur_i = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                cpx u = x[i + k];
                cpx v = x[i + k + len/2];
                float t_r = cur_r*v.r - cur_i*v.i;
                float t_i = cur_r*v.i + cur_i*v.r;
                x[i+k].r = u.r + t_r;
                x[i+k].i = u.i + t_i;
                x[i+k+len/2].r = u.r - t_r;
                x[i+k+len/2].i = u.i - t_i;
                float new_r = cur_r*wr - cur_i*wi;
                cur_i = cur_r*wi + cur_i*wr;
                cur_r = new_r;
            }
        }
    }
    if (inverse) {
        float inv = 1.0f / n;
        for (int i = 0; i < n; i++) { x[i].r *= inv; x[i].i *= inv; }
    }
}

/* FFT 2D: righe poi colonne */
static void fft2d(cpx* data, int n, int inverse) {
    static cpx row[N];
    /* Righe */
    for (int r = 0; r < n; r++) {
        memcpy(row, data + r*n, n * sizeof(cpx));
        fft1d(row, n, inverse);
        memcpy(data + r*n, row, n * sizeof(cpx));
    }
    /* Colonne */
    static cpx col[N];
    for (int c = 0; c < n; c++) {
        for (int r = 0; r < n; r++) col[r] = data[r*n + c];
        fft1d(col, n, inverse);
        for (int r = 0; r < n; r++) data[r*n + c] = col[r];
    }
}

/* fftshift: porta le basse frequenze al centro (come numpy.fft.fftshift) */
static void fftshift2d(cpx* data, int n) {
    int h = n / 2;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < h; c++) {
            /* scambia 4 quadranti */
            cpx t;
            /* Q1 <-> Q3 */
            t = data[r*n + c];
            data[r*n + c] = data[(r+h)*n + (c+h)];
            data[(r+h)*n + (c+h)] = t;
            /* Q2 <-> Q4 */
            t = data[r*n + (c+h)];
            data[r*n + (c+h)] = data[(r+h)*n + c];
            data[(r+h)*n + c] = t;
        }
    }
}

/* ifftshift (identico a fftshift per N pari) */
static void ifftshift2d(cpx* data, int n) { fftshift2d(data, n); }

/* ================================================================
 * Geometrie: costruzione della maschera di apertura U(x,y,0)
 * Coordinate: x,y in [-L/2, L/2], passo dx = L/N
 * ================================================================ */

/*
 * Finestra di apodizzazione (super-Gaussian) ai bordi della griglia.
 * Attenua artefatti da bordo periodico della FFT.
 */
static float apodization(int ix, int iy, int n, float margin_frac) {
    int margin = (int)(margin_frac * n);
    float wx = 1.0f, wy = 1.0f;
    if (ix < margin)       wx = 0.5f*(1.0f - cosf((float)M_PI * ix / margin));
    if (ix > n-1-margin)   wx = 0.5f*(1.0f - cosf((float)M_PI * (n-1-ix) / margin));
    if (iy < margin)       wy = 0.5f*(1.0f - cosf((float)M_PI * iy / margin));
    if (iy > n-1-margin)   wy = 0.5f*(1.0f - cosf((float)M_PI * (n-1-iy) / margin));
    return wx * wy;
}

/*
 * Doppia fenditura di Young
 * slit_w: larghezza singola fenditura [m]
 * slit_d: distanza centro-centro tra fenditure [m]
 * slit_h: altezza fenditura [m] (0 = infinita = solo diffrazione 1D in x)
 * L: dimensione fisica griglia [m]
 */
EXPORT void build_double_slit(float lambda, float slit_w, float slit_d, float slit_h, float L) {
    float dx = L / N;
    for (int r = 0; r < N; r++) {
        float y = (r - N/2) * dx;
        for (int c = 0; c < N; c++) {
            float x = (c - N/2) * dx;
            /* Apertura: |x - d/2| < w/2  OR  |x + d/2| < w/2 */
            int in_slit1 = (fabsf(x - slit_d*0.5f) < slit_w*0.5f);
            int in_slit2 = (fabsf(x + slit_d*0.5f) < slit_w*0.5f);
            int in_height = (slit_h <= 0.0f) || (fabsf(y) < slit_h*0.5f);
            float amp = ((in_slit1 || in_slit2) && in_height) ? 1.0f : 0.0f;
            float apod = apodization(r, c, N, 0.05f);
            g_field[r*N + c].r = amp * apod;
            g_field[r*N + c].i = 0.0f;
        }
    }
}

/*
 * Reticolo di diffrazione
 * n_slits: numero di fenditure
 * slit_w: larghezza singola fenditura [m]
 * period: periodo reticolo [m]
 * L: dimensione fisica griglia [m]
 */
EXPORT void build_grating(float lambda, int n_slits, float slit_w, float period, float L) {
    float dx = L / N;
    float total_w = (n_slits - 1) * period; /* estensione totale */
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            float x = (c - N/2) * dx;
            /* Controlla se x è dentro una delle n_slits fenditure */
            int in_grating = 0;
            for (int s = 0; s < n_slits; s++) {
                float x_center = -total_w*0.5f + s * period;
                if (fabsf(x - x_center) < slit_w*0.5f) { in_grating = 1; break; }
            }
            float apod = apodization(r, c, N, 0.05f);
            g_field[r*N + c].r = in_grating ? 1.0f * apod : 0.0f;
            g_field[r*N + c].i = 0.0f;
        }
    }
}

/*
 * Ostacolo circolare (disco opaco) - produce anello di Poisson/Arago
 * radius: raggio disco [m]
 * L: dimensione griglia [m]
 */
EXPORT void build_circular_obstacle(float lambda, float radius, float L) {
    float dx = L / N;
    for (int r = 0; r < N; r++) {
        float y = (r - N/2) * dx;
        for (int c = 0; c < N; c++) {
            float x = (c - N/2) * dx;
            float rho = sqrtf(x*x + y*y);
            /* 1 ovunque tranne dentro il disco */
            float amp = (rho < radius) ? 0.0f : 1.0f;
            float apod = apodization(r, c, N, 0.05f);
            g_field[r*N + c].r = amp * apod;
            g_field[r*N + c].i = 0.0f;
        }
    }
}

/* ================================================================
 * Angular Spectrum Method: propagazione da z=0 a z=dist
 *
 * Passi:
 *  1. A = FFT2[U(x,y,0)]  (spettro angolare del campo sorgente)
 *  2. Shift DC al centro
 *  3. Moltiplica per H(fx,fy) = trasfer function di propagazione
 *  4. Shift inverso
 *  5. U(x,y,z) = IFFT2[A * H]
 *
 * Transfer function (scala delle lunghezze d'onda):
 *   kz = sqrt( k² - kx² - ky² )   se kx²+ky² < k²  (propagante)
 *      = i*sqrt( kx²+ky² - k² )    evanescente (ampiezza soppressa a grande z)
 *   H = exp(i*kz*z)
 *
 * lambda [m], dist [m], L [m]
 * ================================================================ */
EXPORT void propagate_asm(float lambda, float dist, float L) {
    float k = 2.0f * (float)M_PI / lambda;  /* numero d'onda [rad/m] */
    float dx = L / N;
    float dfx = 1.0f / (N * dx);            /* passo frequenza spaziale [1/m] */

    /* Copia campo in buffer di lavoro */
    memcpy(g_work, g_field, N2 * sizeof(cpx));

    /* Step 1: FFT2 */
    fft2d(g_work, N, 0);

    /* Step 2: fftshift - porta DC al centro */
    fftshift2d(g_work, N);

    /* Step 3: moltiplica per H(fx,fy,z) */
    for (int r = 0; r < N; r++) {
        float fy = (r - N/2) * dfx;  /* frequenza spaziale y */
        for (int c = 0; c < N; c++) {
            float fx = (c - N/2) * dfx;  /* frequenza spaziale x */
            float kx = 2.0f * (float)M_PI * fx;
            float ky = 2.0f * (float)M_PI * fy;
            float kxy2 = kx*kx + ky*ky;
            float k2 = k*k;
            cpx H;
            if (kxy2 < k2) {
                /* Onda propagante: H = exp(i*kz*z), |H|=1 */
                float kz = sqrtf(k2 - kxy2);
                float phase = kz * dist;
                H.r = cosf(phase);
                H.i = sinf(phase);
            } else {
                /* Onda evanescente: H = exp(-alpha*z), sopprimiamo completamente */
                /* Per z >> lambda questo decade rapidamente, ok trascurare */
                H.r = 0.0f;
                H.i = 0.0f;
            }
            /* Moltiplicazione complessa: g_work *= H */
            cpx w = g_work[r*N + c];
            g_work[r*N + c].r = w.r*H.r - w.i*H.i;
            g_work[r*N + c].i = w.r*H.i + w.i*H.r;
        }
    }

    /* Step 4: ifftshift */
    ifftshift2d(g_work, N);

    /* Step 5: IFFT2 -> campo propagato */
    fft2d(g_work, N, 1);  /* inverse=1 */

    /* Salva in g_out e calcola osservabili */
    memcpy(g_out, g_work, N2 * sizeof(cpx));

    /* Calcola intensità, fase, ampiezza */
    float max_I = 0.0f;
    for (int i = 0; i < N2; i++) {
        float re = g_out[i].r, im = g_out[i].i;
        float I = re*re + im*im;
        g_intensity[i] = I;
        g_amplitude[i] = sqrtf(I);
        g_phase[i] = atan2f(im, re);
        g_field_re[i] = re;
        g_field_im[i] = im;
        if (I > max_I) max_I = I;
    }
    /* Normalizza intensità */
    if (max_I > 0.0f) {
        float inv = 1.0f / max_I;
        for (int i = 0; i < N2; i++) g_intensity[i] *= inv;
    }
    /* Normalizza ampiezza */
    float max_A = sqrtf(max_I);
    if (max_A > 0.0f) {
        float inv = 1.0f / max_A;
        for (int i = 0; i < N2; i++) g_amplitude[i] *= inv;
    }
}

/* ================================================================
 * Funzioni accessor per JS
 * ================================================================ */
EXPORT float* get_intensity()  { return g_intensity; }
EXPORT float* get_phase()      { return g_phase; }
EXPORT float* get_amplitude()  { return g_amplitude; }
EXPORT float* get_field_re()   { return g_field_re; }
EXPORT float* get_field_im()   { return g_field_im; }
EXPORT int    get_N()          { return N; }

/* Espone anche il campo sorgente (apertura) per visualizzarlo */
EXPORT void get_source_field(float* out_re, float* out_im) {
    for (int i = 0; i < N2; i++) {
        out_re[i] = g_field[i].r;
        out_im[i] = g_field[i].i;
    }
}

/* Copia diretta del campo sorgente nei buffer output per visualizzazione */
EXPORT void compute_source_observables(void) {
    float max_A = 0.0f;
    for (int i = 0; i < N2; i++) {
        float re = g_field[i].r, im = g_field[i].i;
        float I = re*re + im*im;
        g_intensity[i] = I;
        g_amplitude[i] = sqrtf(I);
        g_phase[i] = atan2f(im, re);

        /* Mantiene coerenti anche i buffer del campo complesso esposti a JS. */
        g_field_re[i] = re;
        g_field_im[i] = im;

        if (g_amplitude[i] > max_A) max_A = g_amplitude[i];
    }
    if (max_A > 0.0f) {
        float inv = 1.0f / (max_A * max_A);
        float invA = 1.0f / max_A;
        for (int i = 0; i < N2; i++) {
            g_intensity[i] *= inv;
            g_amplitude[i] *= invA;
        }
    }
}

