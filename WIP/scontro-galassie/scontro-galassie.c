/*
 * scontro-galassie.c — Collisione tra due galassie, modello collisionless 2.5D
 * Core fisico per WebAssembly/Emscripten.
 *
 * Modello fisico:
 * - due nuclei/alone massivi descritti da potenziali di Plummer ammorbiditi;
 * - particelle stellari collisionless usate come traccianti con coordinate 3D interne;
 * - ogni particella sente il campo gravitazionale dei due nuclei e un termine di alone della galassia di appartenenza;
 * - non si calcola l'interazione stella-stella: questo permette N molto elevati e animazione fluida.
 *
 * Unità interne adimensionali:
 * - lunghezze in unità di raggio di disco iniziale;
 * - masse in unità arbitrarie;
 * - tempi in unità dinamiche.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten/emscripten.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define MAX_PARTICLES 50000
#define CORE_COUNT 2
#define STATE_STRIDE 5
#define MORPH_SPIRAL 0
#define MORPH_ELLIPTICAL 1

static int g_N = 0;
static float g_time = 0.0f;
static float g_dt = 0.0065f;
static float g_G = 1.0f;
static float g_soft_star = 0.035f;
static float g_core_soft = 0.11f;
static float g_halo_soft = 0.42f;
static float g_halo_strength = 0.55f;
static float g_tidal_boost = 1.0f;
static float g_dynamical_friction = 0.012f;
static int g_paused = 0;

static float px[MAX_PARTICLES], py[MAX_PARTICLES], pz[MAX_PARTICLES];
static float vx[MAX_PARTICLES], vy[MAX_PARTICLES], vz[MAX_PARTICLES];
static float ax[MAX_PARTICLES], ay[MAX_PARTICLES], az[MAX_PARTICLES];
static float seed_radius[MAX_PARTICLES];
static float seed_gal_x[MAX_PARTICLES], seed_gal_y[MAX_PARTICLES];
static float tag[MAX_PARTICLES];
static float tracer_energy[MAX_PARTICLES];
static float state_buf[MAX_PARTICLES * STATE_STRIDE];

static float cx[CORE_COUNT], cy[CORE_COUNT];
static float cvx[CORE_COUNT], cvy[CORE_COUNT];
static float cax[CORE_COUNT], cay[CORE_COUNT];
static float cmass[CORE_COUNT];
static float cspin[CORE_COUNT];
static float disk_inclination[CORE_COUNT];
static float disk_view_angle[CORE_COUNT];
static float core_buf[CORE_COUNT * 6];
static float diag_buf[12];

static uint32_t rng_state = 0x12345678u;

static inline float clampf(float x, float a, float b) {
    return x < a ? a : (x > b ? b : x);
}

static inline uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static inline float frand(void) {
    return (xorshift32() & 0x00ffffffu) / 16777216.0f;
}

static inline float randn(void) {
    float u1 = fmaxf(frand(), 1e-6f);
    float u2 = frand();
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PI * u2);
}

static void add_plummer_accel(float x, float y, float z, float sx, float sy, float sz, float mass, float soft, float *out_ax, float *out_ay, float *out_az) {
    float dx = sx - x;
    float dy = sy - y;
    float dz = sz - z;
    float r2 = dx*dx + dy*dy + dz*dz + soft*soft;
    float inv = 1.0f / sqrtf(r2);
    float inv3 = inv * inv * inv;
    float a = g_G * mass * inv3;
    *out_ax += a * dx;
    *out_ay += a * dy;
    *out_az += a * dz;
}

static void add_galaxy_accel(float x, float y, float z, int gal, float mass_factor, float *out_ax, float *out_ay, float *out_az) {
    /* Ogni galassia è rappresentata come nucleo compatto + alone esteso.
       Questo mantiene il costo O(N), ma evita di trattare il compagno come massa puntiforme troppo impulsiva. */
    float m_core = cmass[gal] * mass_factor;
    float m_halo = cmass[gal] * g_halo_strength * mass_factor;
    add_plummer_accel(x, y, z, cx[gal], cy[gal], 0.0f, m_core, g_core_soft, out_ax, out_ay, out_az);
    if (m_halo > 0.0f) {
        add_plummer_accel(x, y, z, cx[gal], cy[gal], 0.0f, m_halo, g_halo_soft, out_ax, out_ay, out_az);
    }
}

static float circular_speed(float r, float mass) {
    /* v_c^2/r = |a_r| per una somma nucleo + alone di Plummer.
       Per una singola componente: v_c^2 = G M r^2 / (r^2 + eps^2)^(3/2). */
    float r2 = r * r;
    float core = g_G * mass * r2 / fmaxf(powf(r2 + g_core_soft*g_core_soft, 1.5f), 1e-6f);
    float halo = g_G * mass * g_halo_strength * r2 / fmaxf(powf(r2 + g_halo_soft*g_halo_soft, 1.5f), 1e-6f);
    return sqrtf(fmaxf(core + halo, 0.0f));
}

static void set_disk_planes(float plane_mode) {
    int mode = (int)(plane_mode + 0.5f);
    disk_inclination[0] = 0.0f;
    disk_inclination[1] = 0.0f;
    disk_view_angle[0] = 0.0f;
    disk_view_angle[1] = 0.0f;

    if (mode == 1) {
        disk_inclination[1] = 0.85f;
        disk_view_angle[1] = -0.65f;
    } else if (mode == 2) {
        disk_inclination[0] = 0.68f;
        disk_inclination[1] = 0.98f;
        disk_view_angle[0] = 0.55f;
        disk_view_angle[1] = -0.85f;
    } else if (mode == 3) {
        disk_inclination[0] = 1.02f;
        disk_inclination[1] = 0.55f;
        disk_view_angle[0] = 0.18f;
        disk_view_angle[1] = 1.34f;
    }
}

static void orient_disk_vector(int gal, float dx, float dy, float dz, float *out_x, float *out_y, float *out_z) {
    float inc = clampf(disk_inclination[gal], 0.0f, 1.25f);
    float a = disk_view_angle[gal];
    float ca = cosf(a);
    float sa = sinf(a);
    float ci = cosf(inc);
    float si = sinf(inc);

    float tx = dx;
    float ty = dy * ci - dz * si;
    float tz = dy * si + dz * ci;

    *out_x = ca * tx - sa * ty;
    *out_y = sa * tx + ca * ty;
    *out_z = tz;
}

static void compute_core_accel(void) {
    cax[0] = cay[0] = cax[1] = cay[1] = 0.0f;
    float dummy_az = 0.0f;

    /* Moto dei centri galattici nel campo esteso dell'altra galassia.
       Anche qui usiamo nucleo + alone, con accelerazione per unità di massa del centro testato. */
    add_galaxy_accel(cx[0], cy[0], 0.0f, 1, 1.0f, &cax[0], &cay[0], &dummy_az);
    dummy_az = 0.0f;
    add_galaxy_accel(cx[1], cy[1], 0.0f, 0, 1.0f, &cax[1], &cay[1], &dummy_az);

    /* Attrito dinamico fenomenologico: rimuove energia orbitale e favorisce la fusione.
       Non è la formula completa di Chandrasekhar; è un termine dissipativo controllabile. */
    cax[0] -= g_dynamical_friction * cvx[0];
    cay[0] -= g_dynamical_friction * cvy[0];
    cax[1] -= g_dynamical_friction * cvx[1];
    cay[1] -= g_dynamical_friction * cvy[1];
}

static void compute_particle_accel(void) {
    for (int i = 0; i < g_N; i++) {
        float axi = 0.0f, ayi = 0.0f, azi = 0.0f;
        int home = tag[i] < 0.5f ? 0 : 1;
        int other = 1 - home;

        add_galaxy_accel(px[i], py[i], pz[i], home, 1.0f, &axi, &ayi, &azi);
        add_galaxy_accel(px[i], py[i], pz[i], other, g_tidal_boost, &axi, &ayi, &azi);

        ax[i] = axi;
        ay[i] = ayi;
        az[i] = azi;
    }
}

static void update_buffers(void) {
    int escaped = 0;
    float rmax = 0.0f;
    float speed_sum = 0.0f;

    for (int i = 0; i < g_N; i++) {
        float x = px[i];
        float y = py[i];
        float z = pz[i];
        float v2 = vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i];
        int home = tag[i] < 0.5f ? 0 : 1;
        float dxh = x - cx[home];
        float dyh = y - cy[home];
        float dzh = z;
        float r_home = sqrtf(dxh*dxh + dyh*dyh + dzh*dzh);
        float r_global = sqrtf(x*x + y*y);
        float stretch = r_home / fmaxf(seed_radius[i], 0.05f);
        if (r_home > 2.65f * fmaxf(seed_radius[i], 0.20f) && r_home > 1.25f) escaped++;
        if (r_global > rmax) rmax = r_global;
        speed_sum += sqrtf(v2);

        tracer_energy[i] = clampf(0.13f * sqrtf(v2) + 0.11f * stretch, 0.0f, 1.0f);

        int k = i * STATE_STRIDE;
        state_buf[k + 0] = x;
        state_buf[k + 1] = y;
        state_buf[k + 2] = tag[i];
        state_buf[k + 3] = tracer_energy[i];
        state_buf[k + 4] = seed_radius[i];
    }

    for (int j = 0; j < CORE_COUNT; j++) {
        int k = j * 6;
        core_buf[k + 0] = cx[j];
        core_buf[k + 1] = cy[j];
        core_buf[k + 2] = cvx[j];
        core_buf[k + 3] = cvy[j];
        core_buf[k + 4] = cmass[j];
        core_buf[k + 5] = cspin[j];
    }

    float dx = cx[1] - cx[0];
    float dy = cy[1] - cy[0];
    float sep = sqrtf(dx*dx + dy*dy);
    diag_buf[0] = g_time;
    diag_buf[1] = sep;
    diag_buf[2] = (float)escaped / fmaxf((float)g_N, 1.0f);
    diag_buf[3] = rmax;
    diag_buf[4] = speed_sum / fmaxf((float)g_N, 1.0f);
    diag_buf[5] = (float)g_N;
    diag_buf[6] = g_dt;
    diag_buf[7] = g_dynamical_friction;
    diag_buf[8] = g_tidal_boost;
    diag_buf[9] = g_halo_strength;
    diag_buf[10] = cmass[0];
    diag_buf[11] = cmass[1];
}

static void seed_spiral_disk(int offset, int n, int gal, float disk_radius, float thickness, float noise) {
    float center_x = cx[gal];
    float center_y = cy[gal];
    float base_vx = cvx[gal];
    float base_vy = cvy[gal];
    float spin = cspin[gal];
    float mass = cmass[gal];

    for (int m = 0; m < n; m++) {
        int i = offset + m;

        /* Distribuzione radiale empirica concentrata verso il centro, non autogravitante. */
        float u = frand();
        float r = disk_radius * powf(u, 0.72f);
        float theta = 2.0f * M_PI * frand();

        if (r > 0.16f * disk_radius && frand() < 0.52f) {
            float arm_id = frand() < 0.5f ? 0.0f : M_PI;
            float rr = fmaxf(r / fmaxf(disk_radius, 0.05f), 0.08f);
            float width = 0.20f + 0.22f * rr;
            theta = arm_id + 1.55f * logf(fmaxf(rr, 0.18f)) + (float)gal * 1.25f + width * randn();
        }

        float arm = 0.030f * sinf(2.0f * theta + 2.7f * r + (float)gal * 1.7f);
        r *= 1.0f + arm;
        float zeta = thickness * randn();

        float dx = r * cosf(theta) + zeta * cosf(theta + M_PI*0.5f);
        float dy = r * sinf(theta) + zeta * sinf(theta + M_PI*0.5f);
        float dz = thickness * 0.55f * randn();
        float rx, ry, rz;
        orient_disk_vector(gal, dx, dy, dz, &rx, &ry, &rz);
        px[i] = center_x + rx;
        py[i] = center_y + ry;
        pz[i] = rz;

        /* Velocità circolare coerente con il potenziale nucleo + alone usato nelle forze. */
        float vc = circular_speed(fmaxf(r, 0.01f), mass);
        vc *= spin;
        vc += noise * randn();

        float tx = -sinf(theta);
        float ty =  cosf(theta);
        float tvx, tvy, tvz;
        orient_disk_vector(gal, vc * tx, vc * ty, noise * 0.18f * randn(), &tvx, &tvy, &tvz);
        vx[i] = base_vx + tvx + noise * 0.35f * randn();
        vy[i] = base_vy + tvy + noise * 0.35f * randn();
        vz[i] = tvz + noise * 0.30f * randn();

        ax[i] = ay[i] = az[i] = 0.0f;
        tag[i] = (float)gal;
        seed_radius[i] = fmaxf(r, 0.04f);
        seed_gal_x[i] = rx;
        seed_gal_y[i] = ry;
        tracer_energy[i] = 0.0f;
    }
}

static void seed_elliptical(int offset, int n, int gal, float scale_radius, float flattening, float noise) {
    float center_x = cx[gal];
    float center_y = cy[gal];
    float base_vx = cvx[gal];
    float base_vy = cvy[gal];
    float spin = cspin[gal];
    float mass = cmass[gal];
    float angle = gal == 0 ? 0.45f : -0.55f;
    float ca = cosf(angle);
    float sa = sinf(angle);
    float axis_q = clampf(flattening, 0.48f, 0.92f);

    for (int m = 0; m < n; m++) {
        int i = offset + m;

        float u = frand();
        float r;
        if (frand() < 0.84f) {
            r = scale_radius * 0.82f * powf(u, 1.55f);
        } else {
            r = scale_radius * (0.72f + 0.78f * frand());
        }
        float theta = 2.0f * M_PI * frand();
        float ex = r * cosf(theta);
        float ey = axis_q * r * sinf(theta);

        float ez = 0.42f * axis_q * r * randn();
        float rx = ca * ex - sa * ey;
        float ry = sa * ex + ca * ey;
        px[i] = center_x + rx;
        py[i] = center_y + ry;
        pz[i] = ez;

        float vc = circular_speed(fmaxf(r, 0.03f), mass);
        float sigma = 0.30f * vc + 0.05f + noise;
        float tx = -sinf(theta);
        float ty =  cosf(theta);

        vx[i] = base_vx + 0.16f * spin * vc * (ca * tx - sa * ty) + sigma * randn();
        vy[i] = base_vy + 0.16f * spin * vc * (sa * tx + ca * ty) + sigma * randn();
        vz[i] = 0.72f * sigma * randn();

        ax[i] = ay[i] = az[i] = 0.0f;
        tag[i] = (float)gal;
        seed_radius[i] = fmaxf(r, 0.08f);
        seed_gal_x[i] = rx;
        seed_gal_y[i] = ry;
        tracer_energy[i] = 0.0f;
    }
}

EMSCRIPTEN_KEEPALIVE
void sim_set_params(float dt, float tidal_boost, float halo_strength, float friction, float core_soft) {
    if (dt > 0.0005f && dt < 0.03f) g_dt = dt;
    g_tidal_boost = clampf(tidal_boost, 0.2f, 3.0f);
    g_halo_strength = clampf(halo_strength, 0.0f, 1.8f);
    g_dynamical_friction = clampf(friction, 0.0f, 0.08f);
    g_core_soft = clampf(core_soft, 0.04f, 0.35f);
}

EMSCRIPTEN_KEEPALIVE
void sim_pause(int paused) {
    g_paused = paused ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void sim_init(int n_req, float impact, float v_in, float disk_radius, float mass_ratio, float spin_mode, float morph_primary, float morph_secondary, float plane_mode, unsigned int seed) {
    if (n_req < 1000) n_req = 1000;
    if (n_req > MAX_PARTICLES) n_req = MAX_PARTICLES;
    g_N = n_req;
    g_time = 0.0f;
    rng_state = seed ? seed : 0x12345678u;

    float q = clampf(mass_ratio, 0.25f, 1.0f);
    cmass[0] = 1.00f;
    cmass[1] = q;

    float b = clampf(impact, 0.0f, 1.6f);
    float vin = clampf(v_in, 0.05f, 1.6f);
    float d0 = 2.85f;
    set_disk_planes(plane_mode);

    cx[0] = -d0;
    cy[0] = -0.5f * b;
    cx[1] =  d0;
    cy[1] =  0.5f * b;

    /* Moto iniziale con centro di massa fermo. Il parametro b resta un vero offset trasversale. */
    cvx[0] =  vin * cmass[1] / (cmass[0] + cmass[1]);
    cvy[0] =  0.0f;
    cvx[1] = -vin * cmass[0] / (cmass[0] + cmass[1]);
    cvy[1] =  0.0f;

    /* Con la geometria scelta, per b > 0 il momento angolare orbitale è positivo.
       Spin positivo = rotazione antioraria = configurazione prograde. */
    if (spin_mode < 0.5f) {         /* prograde-prograde */
        cspin[0] =  1.0f; cspin[1] =  1.0f;
    } else if (spin_mode < 1.5f) {  /* prograde-retrograde */
        cspin[0] =  1.0f; cspin[1] = -1.0f;
    } else {                        /* secondaria più calda e meno coerente */
        cspin[0] =  1.0f; cspin[1] =  0.45f;
    }

    int n0 = (int)((float)g_N * cmass[0] / (cmass[0] + cmass[1]));
    if (n0 < 1) n0 = 1;
    if (n0 > g_N - 1) n0 = g_N - 1;
    int n1 = g_N - n0;

    float rad = clampf(disk_radius, 0.45f, 1.25f);
    float heat0 = 0.012f;
    float heat1 = spin_mode >= 1.5f ? 0.035f : 0.016f;
    int morph0 = morph_primary >= 0.5f ? MORPH_ELLIPTICAL : MORPH_SPIRAL;
    int morph1 = morph_secondary >= 0.5f ? MORPH_ELLIPTICAL : MORPH_SPIRAL;

    if (morph0 == MORPH_ELLIPTICAL) {
        seed_elliptical(0, n0, 0, rad * 0.96f, 0.66f, 0.010f);
    } else {
        seed_spiral_disk(0, n0, 0, rad, 0.016f, heat0);
    }

    if (morph1 == MORPH_ELLIPTICAL) {
        seed_elliptical(n0, n1, 1, rad * sqrtf(q) * 0.96f, 0.70f, 0.012f);
    } else {
        seed_spiral_disk(n0, n1, 1, rad * sqrtf(q), 0.018f, heat1);
    }

    compute_core_accel();
    compute_particle_accel();
    update_buffers();
}

EMSCRIPTEN_KEEPALIVE
void sim_step(int substeps) {
    if (g_paused) return;
    if (substeps < 1) substeps = 1;
    if (substeps > 12) substeps = 12;

    for (int s = 0; s < substeps; s++) {
        float h = g_dt;

        for (int j = 0; j < CORE_COUNT; j++) {
            cvx[j] += 0.5f * h * cax[j];
            cvy[j] += 0.5f * h * cay[j];
            cx[j] += h * cvx[j];
            cy[j] += h * cvy[j];
        }

        for (int i = 0; i < g_N; i++) {
            vx[i] += 0.5f * h * ax[i];
            vy[i] += 0.5f * h * ay[i];
            vz[i] += 0.5f * h * az[i];
            px[i] += h * vx[i];
            py[i] += h * vy[i];
            pz[i] += h * vz[i];
        }

        compute_core_accel();
        compute_particle_accel();

        for (int j = 0; j < CORE_COUNT; j++) {
            cvx[j] += 0.5f * h * cax[j];
            cvy[j] += 0.5f * h * cay[j];
        }

        for (int i = 0; i < g_N; i++) {
            vx[i] += 0.5f * h * ax[i];
            vy[i] += 0.5f * h * ay[i];
            vz[i] += 0.5f * h * az[i];
        }

        g_time += h;
    }

    update_buffers();
}

EMSCRIPTEN_KEEPALIVE
int sim_get_count(void) { return g_N; }

EMSCRIPTEN_KEEPALIVE
float sim_get_time(void) { return g_time; }

EMSCRIPTEN_KEEPALIVE
float* sim_get_state_ptr(void) { return state_buf; }

EMSCRIPTEN_KEEPALIVE
float* sim_get_core_ptr(void) { return core_buf; }

EMSCRIPTEN_KEEPALIVE
float* sim_get_diag_ptr(void) { return diag_buf; }

EMSCRIPTEN_KEEPALIVE
int sim_get_max_particles(void) { return MAX_PARTICLES; }
