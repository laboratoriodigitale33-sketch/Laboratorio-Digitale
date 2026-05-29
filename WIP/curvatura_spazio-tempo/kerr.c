/*
 * ============================================================
 *  KERR SPACETIME SIMULATOR — versione Emscripten/WebAssembly
 *
 *  Fisica: Metrica di Kerr in coordinate Boyer-Lindquist
 *  Unità geometrizzate: G = c = M = 1, r in unità di r_g
 *
 *  Rendering: Canvas HTML5 via emscripten_set_main_loop()
 *  Buffer pixel: RGBA 32bpp scritto direttamente in memoria
 *  e trasferito al canvas tramite CanvasRenderingContext2D.putImageData
 *
 *  Compilare con:
 *    emcc kerr.c -o kerr.js \
 *         -O3 -msimd128 \
 *         -s WASM=1 \
 *         -s EXPORTED_FUNCTIONS='["_main","_render_frame","_set_spin","_set_zoom","_set_pitch","_set_mode","_set_nrays","_set_remit","_step_anim","_get_rp","_get_rergo","_get_risco","_get_rph","_get_omH","_get_width","_get_height","_get_pixel_buffer"]' \
 *         -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8"]' \
 *         -s ALLOW_MEMORY_GROWTH=1 \
 *         -lm
 * ============================================================
 */

#include <emscripten.h>
#include <emscripten/html5.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  CONFIGURAZIONE                                                      */
/* ------------------------------------------------------------------ */
#define WIDTH        900
#define HEIGHT       700
#define PI           3.14159265358979323846
#define TWO_PI       6.28318530717958647692
#define MAX_STEPS    6000
#define MAX_PATH     5000
#define N_RAYS_MAX   120

typedef enum {
    MODE_GRID     = 0,
    MODE_GEODESIC = 1,
    MODE_LENS     = 2,
    MODE_REDSHIFT = 3,
    MODE_COUNT    = 4
} RenderMode;

/* ------------------------------------------------------------------ */
/*  STRUTTURE                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    double gtt, gtphi, grr, gphiphi;
    double Delta, Sigma, det;
} KerrMetric;

typedef struct {
    double rp, r_ergo, r_isco, r_ph, Omega_H;
} KerrProps;

typedef struct {
    double r[MAX_PATH];
    double phi[MAX_PATH];
    int    n, captured, escaped;
} GeodesicPath;

/* ------------------------------------------------------------------ */
/*  STATO GLOBALE                                                       */
/* ------------------------------------------------------------------ */
static uint8_t  pixels[WIDTH * HEIGHT * 4];   /* RGBA */
static double   g_a       = 0.60;             /* spin */
static double   g_zoom    = 2.2;
static int      g_nrays   = 40;
static double   g_remit   = 18.0;
static RenderMode g_mode  = MODE_GRID;
static double   g_phi_off = 0.0;
static double   g_flow_t  = 0.0;
static double   g_cam_pitch = 0.60;
static KerrProps g_kp;
static int      g_kp_valid = 0;
static GeodesicPath g_geo_cache[N_RAYS_MAX];
static int      g_geo_cache_valid = 0;

static void kerr_props(double a, KerrProps *p);

static void invalidate_geodesic_cache(void)
{
    g_geo_cache_valid = 0;
}

static void ensure_kerr_props(void)
{
    if(!g_kp_valid){
        kerr_props(g_a,&g_kp);
        g_kp_valid=1;
    }
}

/* ------------------------------------------------------------------ */
/*  METRICA DI KERR (piano equatoriale θ=π/2, M=1)                    */
/* ------------------------------------------------------------------ */
static inline void kerr_metric(double r, double a, KerrMetric *m)
{
    double r2    = r * r;
    double a2    = a * a;
    m->Sigma     = r2;
    m->Delta     = r2 - 2.0*r + a2;
    m->gtt       = -(1.0 - 2.0*r/m->Sigma);
    m->gtphi     =  2.0*a*r/m->Sigma;
    m->grr       =  m->Sigma / m->Delta;
    m->gphiphi   =  r2 + a2 + 2.0*a2*r/m->Sigma;
    m->det       =  m->gtt * m->gphiphi - m->gtphi * m->gtphi;
}

/* ------------------------------------------------------------------ */
/*  QUANTITÀ FISICHE                                                    */
/* ------------------------------------------------------------------ */
static void kerr_props(double a, KerrProps *p)
{
    double sqrd  = sqrt(fmax(0.0, 1.0 - a*a));
    p->rp        = 1.0 + sqrd;
    p->r_ergo    = 2.0;

    double a2    = a*a;
    double cb    = cbrt(1.0 - a2);
    double z1    = 1.0 + cb*(cbrt(1.0+a) + cbrt(1.0-a));
    double z2    = sqrt(3.0*a2 + z1*z1);
    p->r_isco    = 3.0 + z2 - sqrt((3.0-z1)*(3.0+z1+2.0*z2));

    double arg   = (fabs(a) < 1e-10) ? 0.0 : -a;
    p->r_ph      = 2.0*(1.0 + cos(2.0/3.0 * acos(fmax(-1.0,fmin(1.0,arg)))));
    p->Omega_H   = a / (2.0 * p->rp);
}

/* ------------------------------------------------------------------ */
/*  POTENZIALE EFFETTIVO E DERIVATE                                     */
/* ------------------------------------------------------------------ */
static inline double Veff(double r, double a, double E, double L)
{
    KerrMetric m;
    kerr_metric(r, a, &m);
    return (E*E*m.gphiphi + 2.0*E*L*m.gtphi + L*L*m.gtt) / m.det;
}

static void geodesic_derivs(
    double r, double dr, double a, double E, double L,
    double *dr_out, double *ddr_out)
{
    KerrMetric m;
    kerr_metric(r, a, &m);

    const double eps = r * 5e-6;
    double Vp = Veff(r+eps, a, E, L);
    double Vm = Veff(r-eps, a, E, L);
    double dVdr = (Vp - Vm) / (2.0*eps);

    KerrMetric mp, mm;
    kerr_metric(r+eps, a, &mp);
    kerr_metric(r-eps, a, &mm);
    double dgrr = (mp.grr - mm.grr) / (2.0*eps);

    *dr_out  = dr;
    *ddr_out = (dVdr - dr*dr*dgrr) / (2.0*m.grr);
}

static inline double dphi_from_EL(double r, double a, double E, double L)
{
    KerrMetric m;
    kerr_metric(r, a, &m);
    return (E*m.gtphi + L*m.gtt) / m.det;
}

/* ------------------------------------------------------------------ */
/*  RK4 A PASSO ADATTIVO                                               */
/* ------------------------------------------------------------------ */
static void rk4_step(
    double *r, double *phi, double *dr,
    double a, double E, double L, double h)
{
    double k1r, k1dr, k2r, k2dr, k3r, k3dr, k4r, k4dr;

    geodesic_derivs(*r,          *dr,          a,E,L, &k1r,&k1dr);
    geodesic_derivs(*r+h*k1r/2, *dr+h*k1dr/2, a,E,L, &k2r,&k2dr);
    geodesic_derivs(*r+h*k2r/2, *dr+h*k2dr/2, a,E,L, &k3r,&k3dr);
    geodesic_derivs(*r+h*k3r,   *dr+h*k3dr,   a,E,L, &k4r,&k4dr);

    double r_new  = *r  + h*(k1r +2*k2r +2*k3r +k4r )/6.0;
    double dr_new = *dr + h*(k1dr+2*k2dr+2*k3dr+k4dr)/6.0;

    /* Aggiornamento phi con phi' medio lungo il passo */
    double dphi_mid = dphi_from_EL(*r + h*0.5*(k1r+k2r)/2.0, a, E, L);
    *phi += h * dphi_mid;
    *r   = r_new;
    *dr  = dr_new;
}

static void rk4_adaptive(
    double *r, double *phi, double *dr,
    double a, double E, double L,
    double *h, double h_min, double h_max, double tol)
{
    double r1=*r, p1=*phi, d1=*dr;
    double r2=*r, p2=*phi, d2=*dr;

    rk4_step(&r1,&p1,&d1, a,E,L, *h);

    double hh=*h*0.5;
    rk4_step(&r2,&p2,&d2, a,E,L, hh);
    rk4_step(&r2,&p2,&d2, a,E,L, hh);

    double err = fabs(r1-r2);
    if (err < 1e-15) err=1e-15;

    double factor = 0.9*pow(tol/err, 0.2);
    factor = fmax(0.1, fmin(5.0, factor));
    *h = fmax(h_min, fmin(h_max, (*h)*factor));

    if (err < tol || *h <= h_min) {
        *r=r2; *phi=p2; *dr=d2;
    }
}

/* ------------------------------------------------------------------ */
/*  INTEGRAZIONE GEODETICA                                             */
/* ------------------------------------------------------------------ */
static void __attribute__((unused)) integrate_geodesic(
    double r0, double phi0, double E, double L, double a,
    double r_max, double r_cap, GeodesicPath *path)
{
    double r=r0, phi=phi0;
    double V0=Veff(r,a,E,L);
    double dr = (V0>=0.0) ? -sqrt(V0) : 0.0;

    path->n=0; path->captured=0; path->escaped=0;

    double h=0.06, h_min=1e-5, h_max=0.15, tol=1e-7;

    for (int step=0; step<MAX_STEPS && path->n<MAX_PATH-1; step++) {
        if (r < r_cap)    { path->captured=1; break; }
        if (r > r_max)    { path->escaped=1;  break; }
        if (!isfinite(r)) break;

        path->r[path->n]   = r;
        path->phi[path->n] = phi;
        path->n++;

        rk4_adaptive(&r,&phi,&dr, a,E,L, &h, h_min,h_max, tol);

        double Vn = Veff(r,a,E,L);
        if (dr<0.0 && Vn<0.0) dr=fabs(dr);
    }
}

static void build_geodesic_cache(void)
{
    double a = g_a;
    double r_far = g_remit * 1.35;
    double r_cap = g_kp.rp * 1.035;
    double r_escape = r_far * 1.18;
    int n = g_nrays;

    for (int i = 0; i < n; i++) {
        GeodesicPath *path = &g_geo_cache[i];
        double frac = (n > 1) ? ((double)i / (double)(n - 1)) * 2.0 - 1.0 : 0.0;
        double b = frac * g_remit * 0.82;
        double x = -r_far;
        double y = b;
        double vx = 1.0;
        double vy = 0.0;
        double ds = (2.25 * r_far) / 900.0;

        path->n = 0;
        path->captured = 0;
        path->escaped = 0;

        for (int step = 0; step < 900 && path->n < MAX_PATH - 1; step++) {
            double r = sqrt(x*x + y*y);
            if(!isfinite(r)) break;
            if(r < r_cap) { path->captured = 1; break; }
            if(x > r_far || (r > r_escape && x > 0.0)) { path->escaped = 1; break; }

            path->r[path->n] = r;
            path->phi[path->n] = atan2(y, x);
            path->n++;

            double r_soft = sqrt(r*r + 0.28);
            double invr3 = 1.0 / (r_soft*r_soft*r_soft);
            double pull = 2.20 * invr3;
            double frame = 2.10 * a * invr3 / fmax(1.0, r_soft);

            double ax = -pull*x - frame*y;
            double ay = -pull*y + frame*x;

            vx += ax * ds;
            vy += ay * ds;
            double vl = sqrt(vx*vx + vy*vy);
            if(vl < 1e-9) break;
            vx /= vl;
            vy /= vl;

            x += vx * ds;
            y += vy * ds;
        }
    }

    for (int i = n; i < N_RAYS_MAX; i++) {
        g_geo_cache[i].n = 0;
        g_geo_cache[i].captured = 0;
        g_geo_cache[i].escaped = 0;
    }

    g_geo_cache_valid = 1;
}

/* ------------------------------------------------------------------ */
/*  PIXEL BUFFER                                                        */
/* ------------------------------------------------------------------ */
static inline void put_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x>=(unsigned)WIDTH || (unsigned)y>=(unsigned)HEIGHT) return;
    int i=(y*WIDTH+x)*4;
    pixels[i]=r; pixels[i+1]=g; pixels[i+2]=b; pixels[i+3]=255;
}

static void draw_line(int x0,int y0,int x1,int y1,
                      uint8_t r,uint8_t g,uint8_t b)
{
    int dx=abs(x1-x0),dy=abs(y1-y0);
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    while(1){
        put_pixel(x0,y0,r,g,b);
        if(x0==x1&&y0==y1) break;
        int e2=2*err;
        if(e2>-dy){err-=dy;x0+=sx;}
        if(e2< dx){err+=dx;y0+=sy;}
    }
}

static void draw_circle(int cx,int cy,int radius,
                        uint8_t r,uint8_t g,uint8_t b,int dashed)
{
    if(radius<=0) return;
    int x=radius,y=0,err=0,dash=0;
    while(x>=y){
        if(!dashed||(dash/3)%2==0){
            put_pixel(cx+x,cy+y,r,g,b); put_pixel(cx+y,cy+x,r,g,b);
            put_pixel(cx-y,cy+x,r,g,b); put_pixel(cx-x,cy+y,r,g,b);
            put_pixel(cx-x,cy-y,r,g,b); put_pixel(cx-y,cy-x,r,g,b);
            put_pixel(cx+y,cy-x,r,g,b); put_pixel(cx+x,cy-y,r,g,b);
        }
        dash++;
        if(err<=0){y++;err+=2*y+1;}
        else      {x--;err-=2*x+1;}
    }
}

static void fill_circle(int cx,int cy,int radius,
                        uint8_t r,uint8_t g,uint8_t b)
{
    for(int dy=-radius;dy<=radius;dy++)
    for(int dx=-radius;dx<=radius;dx++)
        if(dx*dx+dy*dy<=radius*radius)
            put_pixel(cx+dx,cy+dy,r,g,b);
}

static void fill_ellipse(int cx,int cy,int rx,int ry,
                         uint8_t r,uint8_t g,uint8_t b)
{
    if(rx<=0||ry<=0) return;
    double irx2=1.0/(double)(rx*rx);
    double iry2=1.0/(double)(ry*ry);
    for(int dy=-ry;dy<=ry;dy++)
    for(int dx=-rx;dx<=rx;dx++)
        if(dx*dx*irx2+dy*dy*iry2<=1.0)
            put_pixel(cx+dx,cy+dy,r,g,b);
}

static const uint8_t* glyph5x7(char c)
{
    static const uint8_t A[7]={14,17,17,31,17,17,17};
    static const uint8_t B[7]={30,17,17,30,17,17,30};
    static const uint8_t C[7]={14,17,16,16,16,17,14};
    static const uint8_t D[7]={30,17,17,17,17,17,30};
    static const uint8_t E[7]={31,16,16,30,16,16,31};
    static const uint8_t F[7]={31,16,16,30,16,16,16};
    static const uint8_t G[7]={14,17,16,23,17,17,15};
    static const uint8_t H[7]={17,17,17,31,17,17,17};
    static const uint8_t I[7]={14,4,4,4,4,4,14};
    static const uint8_t J[7]={7,2,2,2,18,18,12};
    static const uint8_t K[7]={17,18,20,24,20,18,17};
    static const uint8_t L[7]={16,16,16,16,16,16,31};
    static const uint8_t M[7]={17,27,21,21,17,17,17};
    static const uint8_t N[7]={17,25,21,19,17,17,17};
    static const uint8_t O[7]={14,17,17,17,17,17,14};
    static const uint8_t P[7]={30,17,17,30,16,16,16};
    static const uint8_t Q[7]={14,17,17,17,21,18,13};
    static const uint8_t R[7]={30,17,17,30,20,18,17};
    static const uint8_t S[7]={15,16,16,14,1,1,30};
    static const uint8_t T[7]={31,4,4,4,4,4,4};
    static const uint8_t U[7]={17,17,17,17,17,17,14};
    static const uint8_t V[7]={17,17,17,17,10,10,4};
    static const uint8_t W[7]={17,17,17,21,21,27,17};
    static const uint8_t X[7]={17,10,4,4,4,10,17};
    static const uint8_t Y[7]={17,10,4,4,4,4,4};
    static const uint8_t Z[7]={31,1,2,4,8,16,31};
    static const uint8_t D0[7]={14,17,19,21,25,17,14};
    static const uint8_t D1[7]={4,12,4,4,4,4,14};
    static const uint8_t D2[7]={14,17,1,2,4,8,31};
    static const uint8_t D3[7]={30,1,1,14,1,1,30};
    static const uint8_t D4[7]={2,6,10,18,31,2,2};
    static const uint8_t D5[7]={31,16,16,30,1,1,30};
    static const uint8_t D6[7]={14,16,16,30,17,17,14};
    static const uint8_t D7[7]={31,1,2,4,8,8,8};
    static const uint8_t D8[7]={14,17,17,14,17,17,14};
    static const uint8_t D9[7]={14,17,17,15,1,1,14};
    static const uint8_t PLUS[7]={0,4,4,31,4,4,0};
    static const uint8_t MINUS[7]={0,0,0,31,0,0,0};
    static const uint8_t SLASH[7]={1,1,2,4,8,16,16};
    static const uint8_t BAR[7]={4,4,4,4,4,4,4};
    static const uint8_t COLON[7]={0,4,4,0,4,4,0};
    static const uint8_t DOT[7]={0,0,0,0,0,12,12};
    static const uint8_t LP[7]={2,4,8,8,8,4,2};
    static const uint8_t RP[7]={8,4,2,2,2,4,8};
    static const uint8_t SPACE[7]={0,0,0,0,0,0,0};
    if(c>='a'&&c<='z') c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z'){
        const uint8_t* letters[]={
            A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z
        };
        return letters[c-'A'];
    }
    if(c>='0'&&c<='9'){
        const uint8_t* digits[]={D0,D1,D2,D3,D4,D5,D6,D7,D8,D9};
        return digits[c-'0'];
    }
    switch(c){
        case '+': return PLUS;
        case '-': return MINUS;
        case '/': return SLASH;
        case '|': return BAR;
        case ':': return COLON;
        case '.': return DOT;
        case '(': return LP;
        case ')': return RP;
        default: return SPACE;
    }
}

static void draw_text5(int x,int y,const char *s,
                       uint8_t R,uint8_t G,uint8_t B,int scale)
{
    if(scale<1) scale=1;
    int ox=x;
    while(*s){
        if(*s=='\n'){y+=8*scale; x=ox; s++; continue;}
        const uint8_t *g=glyph5x7(*s++);
        for(int row=0;row<7;row++){
            for(int col=0;col<5;col++){
                if(g[row]&(1<<(4-col))){
                    for(int yy=0;yy<scale;yy++)
                    for(int xx=0;xx<scale;xx++)
                        put_pixel(x+col*scale+xx,y+row*scale+yy,R,G,B);
                }
            }
        }
        x+=6*scale;
    }
}

/* ------------------------------------------------------------------ */
/*  COORDINATE                                                          */
/* ------------------------------------------------------------------ */
static inline void w2s(double wx,double wy,double cx,double cy,
                       double scale,int *sx,int *sy)
{
    *sx=(int)(cx+wx*scale);
    *sy=(int)(cy-wy*scale);
}

static inline double clamp01(double x)
{
    return fmax(0.0, fmin(1.0, x));
}

static inline double positive_mod(double x,double period)
{
    double y=fmod(x,period);
    return (y<0.0) ? y+period : y;
}

static inline double smooth01(double x)
{
    x=clamp01(x);
    return x*x*(3.0-2.0*x);
}

static inline double kerr_frame_drag_rate(double r,double a)
{
    KerrMetric m;
    kerr_metric(r,a,&m);
    double omega=(m.gphiphi!=0.0) ? fabs(m.gtphi/m.gphiphi) : 0.0;
    return (a>=0.0) ? omega : -omega;
}

static inline double kerr_embedding_depth(double r,double a)
{
    KerrMetric m;
    kerr_metric(r,a,&m);
    if(m.Delta<=0.0 || !isfinite(m.grr)) return 0.0;

    /* Profondità didattica: conserva la divergenza verso r+, ma la
     * comprime logaritmicamente per non sparare la mesh fuori canvas. */
    double stretch=sqrt(fmax(1.0,m.grr))-1.0;
    return r*0.82*log1p(fmin(280.0,stretch));
}

static inline double mesh_twist(double r,double r_min,double r_max,double a)
{
    double span=fmax(1e-6,r_max-r_min);
    double near=1.0-clamp01((r-r_min)/span);
    double omH=fmax(1e-6,fabs(g_kp.Omega_H));
    double local=fabs(kerr_frame_drag_rate(r,a))/omH;
    double spin_dir=(a>=0.0) ? 1.0 : -1.0;

    double static_shear=0.34*fabs(a)*log((r_max+1.0)/(r+0.65));
    double bounded_lag=0.34*smooth01(near)*sin(g_flow_t*(0.18+0.10*local));
    return g_phi_off+spin_dir*(static_shear+bounded_lag);
}

static inline void project_embedding_point(double r,double phi,double a,
                                           double cx,double cy,double scale,
                                           int *sx,int *sy)
{
    double z=kerr_embedding_depth(r,a);
    double wx=r*cos(phi);

    /* Segno intenzionale: z positivo viene proiettato verso il basso,
     * come nella visualizzazione didattica usuale dell'imbuto. */
    double wy=r*sin(phi)*cos(g_cam_pitch)-z*0.58*sin(g_cam_pitch);
    w2s(wx,wy,cx,cy,scale,sx,sy);
}

static inline int radial_log_x(double r,double r_min,double r_max,
                               int gx,int gw)
{
    double u=log(fmax(r,r_min)/r_min)/log(r_max/r_min);
    return gx+(int)(clamp01(u)*gw);
}

static void draw_projected_ring(double r,double r_min,double r_max,
                                double a,double cx,double cy,double scale,
                                uint8_t R,uint8_t G,uint8_t B,int dashed)
{
    const int N=192;
    int px0=0,py0=0,first=1;
    int dash=0;
    double tw=mesh_twist(r,r_min,r_max,a);
    for(int ip=0;ip<=N;ip++){
        double phi=(double)ip/N*TWO_PI+tw;
        int sx,sy;
        project_embedding_point(r,phi,a,cx,cy,scale,&sx,&sy);
        if(!first && (!dashed || (dash/8)%2==0))
            draw_line(px0,py0,sx,sy,R,G,B);
        px0=sx; py0=sy; first=0; dash++;
    }
}

static void draw_infall_mesh(double cx,double cy,double scale,
                             double r_min,double r_max,
                             int Nr,int Np,double opacity,int animated)
{
    double a=g_a;
    double span=fmax(1e-6,r_max-r_min);
    double fall_speed=0.62+0.34*fabs(a);
    opacity=clamp01(opacity);

    for(int ip=0;ip<Np;ip++){
        double base_phi=(double)ip/Np*TWO_PI;
        int px0=0,py0=0,first=1;

        for(int ir=0;ir<=Nr;ir++){
            double u=(double)ir/Nr;
            double r=r_min+span*u;
            double phi=base_phi+mesh_twist(r,r_min,r_max,a);
            int sx,sy;
            project_embedding_point(r,phi,a,cx,cy,scale,&sx,&sy);
            if(!first){
                KerrMetric mm; kerr_metric(r,a,&mm);
                double near=1.0-u;
                double curv=(mm.Delta>0.06)?1.0/(r*r):4.0;
                double pulse=0.82+0.18*sin(5.8*r-g_flow_t*2.4);
                double alpha=opacity*(0.11+fmin(0.62,curv*5.0)+0.20*near*near)*pulse;
                uint8_t R=(uint8_t)(fmin(255.0,(185.0-45.0*u)*alpha));
                uint8_t G=(uint8_t)(fmin(255.0,(115.0+75.0*u)*alpha));
                uint8_t B=(uint8_t)(fmin(255.0,( 78.0+96.0*u)*alpha));
                draw_line(px0,py0,sx,sy,R,G,B);
            }
            px0=sx; py0=sy; first=0;
        }
    }

    for(int ir=0;ir<=Nr;ir++){
        double base=(double)ir/Nr*span;
        double adv=animated ? positive_mod(base-g_flow_t*fall_speed,span) : base;
        double r=r_min+adv;
        double u=(r-r_min)/span;
        double near=1.0-u;
        double alpha=opacity*(0.16+0.64*near*near);
        uint8_t R=(uint8_t)(fmin(255.0,(165.0+70.0*near)*alpha));
        uint8_t G=(uint8_t)(fmin(255.0,( 80.0+70.0*u   )*alpha));
        uint8_t B=(uint8_t)(fmin(255.0,(105.0+65.0*u   )*alpha));

        int px0=0,py0=0,first=1;
        double tw=mesh_twist(r,r_min,r_max,a);
        for(int ip=0;ip<=Np;ip++){
            double phi=(double)ip/Np*TWO_PI+tw;
            int sx,sy;
            project_embedding_point(r,phi,a,cx,cy,scale,&sx,&sy);
            if(!first) draw_line(px0,py0,sx,sy,R,G,B);
            px0=sx; py0=sy; first=0;
        }
    }
}

static void draw_marker_label(int sx,int sy,const char *label,
                              uint8_t R,uint8_t G,uint8_t B)
{
    fill_circle(sx,sy,5,0,0,0);
    fill_circle(sx,sy,3,R,G,B);
    draw_line(sx+6,sy,sx+16,sy-8,R,G,B);
    draw_text5(sx+19,sy-14,label,R,G,B,1);
}

static void draw_projected_marker(double cx,double cy,double scale,
                                  double r,double phi,double a,
                                  uint8_t R,uint8_t G,uint8_t B,
                                  const char *label)
{
    int sx,sy;
    project_embedding_point(r,phi,a,cx,cy,scale,&sx,&sy);
    draw_marker_label(sx,sy,label,R,G,B);
}

static void draw_orbit_segment(double cx,double cy,double scale,
                               double r,double phi0,double phi1,double a,
                               uint8_t R,uint8_t G,uint8_t B)
{
    int sx0,sy0,sx1,sy1;
    project_embedding_point(r,phi0,a,cx,cy,scale,&sx0,&sy0);
    project_embedding_point(r,phi1,a,cx,cy,scale,&sx1,&sy1);
    draw_line(sx0,sy0,sx1,sy1,R,G,B);
}

static void draw_diagnostic_worldlines(double cx,double cy,double scale,
                                       double r_min,double r_max,double a)
{
    double spin_dir=(a>=0.0)?1.0:-1.0;
    double t=g_flow_t;

    /* Free fall: r scende verso r+ e la fase azimutale accelera per il
     * trascinamento dei sistemi inerziali. La traiettoria si resetta fuori
     * campo per restare leggibile come dimostrazione ciclica. */
    double phase=positive_mod(t*0.055,1.0);
    const double tail_step=0.018;
    const double visible_until=0.94;
    double prevx=0.0,prevy=0.0;
    int first=1;
    for(int k=34;k>=0;k--){
        double p=phase-(double)k*tail_step;
        if(p<0.0 || p>visible_until){
            first=1;
            continue;
        }
        double u=smooth01(p);
        double r=r_max-(r_max-r_min*1.16)*u;
        double phi=-2.40+spin_dir*(0.85*u+1.65*log((r_max+1.0)/(r+0.55)));
        int sx,sy;
        project_embedding_point(r,phi,a,cx,cy,scale,&sx,&sy);
        if(!first){
            double age_fade=(double)(35-k)/35.0;
            double horizon_fade=1.0-smooth01((p-0.82)/(visible_until-0.82));
            double fade=age_fade*horizon_fade;
            draw_line((int)prevx,(int)prevy,sx,sy,
                      (uint8_t)(60+120*fade),(uint8_t)(120+80*fade),220);
        }
        prevx=sx; prevy=sy; first=0;
    }
    if(phase<=visible_until){
        double u=smooth01(phase);
        double r_free=r_max-(r_max-r_min*1.16)*u;
        double phi_free=-2.40+spin_dir*(0.85*u+1.65*log((r_max+1.0)/(r_free+0.55)));
        double vanish=1.0-smooth01((phase-0.84)/(visible_until-0.84));
        draw_projected_marker(cx,cy,scale,r_free,phi_free,a,
                              (uint8_t)(90*vanish),(uint8_t)(190*vanish),
                              (uint8_t)(235*vanish),"FREE FALL");
    }

    /* Corotazione forzata: un osservatore locale dentro/accanto
     * all'ergosfera non puo restare statico rispetto all'infinito. */
    double r_zamo=fmax(g_kp.rp*1.10,fmin(g_kp.r_ergo*0.96,r_max*0.40));
    double omH=fmax(1e-6,fabs(g_kp.Omega_H));
    double om=fabs(kerr_frame_drag_rate(r_zamo,a))/omH;
    double phi_z=1.05+spin_dir*t*(0.045+0.19*om);
    for(int k=0;k<42;k++){
        double p0=phi_z-spin_dir*(double)(k+1)*0.045;
        double p1=phi_z-spin_dir*(double)k*0.045;
        double fade=1.0-(double)k/42.0;
        draw_orbit_segment(cx,cy,scale,r_zamo,p0,p1,a,
                           (uint8_t)(230*fade),(uint8_t)(112*fade),35);
    }
    draw_projected_marker(cx,cy,scale,r_zamo,phi_z,a,235,130,35,"ZAMO DRAG");

    /* Orbita quasi circolare a r_ISCO: non cade subito, ma mostra la
     * rotazione kepleriana minima ancora stabile per quel valore di spin. */
    double r_orb=fmin(fmax(g_kp.r_isco,r_min*1.35),r_max*0.82);
    double omega_k=1.0/(pow(r_orb,1.5)+fabs(a));
    double phi_o=2.20+spin_dir*t*omega_k*0.58;
    for(int k=0;k<52;k++){
        double p0=phi_o-spin_dir*(double)(k+1)*0.035;
        double p1=phi_o-spin_dir*(double)k*0.035;
        double fade=1.0-(double)k/52.0;
        draw_orbit_segment(cx,cy,scale,r_orb,p0,p1,a,
                           (uint8_t)(45*fade),(uint8_t)(210*fade),(uint8_t)(120*fade));
    }
    draw_projected_marker(cx,cy,scale,r_orb,phi_o,a,45,220,130,"ISCO ORBIT");
}

/* ------------------------------------------------------------------ */
/*  COLORE DA LUNGHEZZA D'ONDA (CIE approssimata)                      */
/* ------------------------------------------------------------------ */
static void wavelength_rgb(double lam,
                           uint8_t *R,uint8_t *G,uint8_t *B)
{
    double r,g,b;
    if      (lam<380){r=0.5;g=0;b=0.5;}
    else if (lam<440){r=(440-lam)/60.0;g=0;b=1;}
    else if (lam<490){r=0;g=(lam-440)/50.0;b=1;}
    else if (lam<510){r=0;g=1;b=(510-lam)/20.0;}
    else if (lam<580){r=(lam-510)/70.0;g=1;b=0;}
    else if (lam<645){r=1;g=(645-lam)/65.0;b=0;}
    else if (lam<750){r=1;g=0;b=0;}
    else             {r=0.5;g=0;b=0;}
    *R=(uint8_t)(pow(r,0.8)*235);
    *G=(uint8_t)(pow(g,0.8)*235);
    *B=(uint8_t)(pow(b,0.8)*235);
}

/* ------------------------------------------------------------------ */
/*  RENDER 0: GRIGLIA / EMBEDDING DI FLAMM                             */
/* ------------------------------------------------------------------ */
static void render_grid(void)
{
    /* Sfondo: nero caldo */
    for(int i=0;i<WIDTH*HEIGHT*4;i+=4){
        pixels[i]=12;pixels[i+1]=9;pixels[i+2]=6;pixels[i+3]=255;
    }

    double cx=WIDTH/2.0, cy=HEIGHT/2.0-18.0;
    double scale=g_zoom*20.0;
    double a=g_a;
    double r_min=g_kp.rp*1.005, r_max=26.0/g_zoom;
    if(r_max<r_min+2.0) r_max=r_min+2.0;

    draw_infall_mesh(cx,cy,scale,r_min,r_max,76,108,1.0,1);

    /* Frame-dragging: frecce tangenziali intorno ergosfera */
    if(a>0.01){
        double r_arr=g_kp.r_ergo*1.35;
        for(int i=0;i<14;i++){
            double phi=(double)i/14*TWO_PI + mesh_twist(r_arr,r_min,r_max,a);
            int ax,ay,ax2,ay2;
            project_embedding_point(r_arr,phi,a,cx,cy,scale,&ax,&ay);
            project_embedding_point(r_arr,phi+0.075,a,cx,cy,scale,&ax2,&ay2);
            double al=0.35+a*0.55;
            uint8_t fc=(uint8_t)(230*al),gc=(uint8_t)(110*al),bc=(uint8_t)(15*al);
            draw_line(ax,ay,ax2,ay2,fc,gc,bc);
            double vx=ax2-ax, vy=ay2-ay;
            double vl=fmax(1.0,sqrt(vx*vx+vy*vy));
            vx/=vl; vy/=vl;
            put_pixel(ax2-(int)(5*(vx*0.55-vy)),ay2-(int)(5*(vy*0.55+vx)),fc,gc,bc);
            put_pixel(ax2-(int)(5*(vx*0.55+vy)),ay2-(int)(5*(vy*0.55-vx)),fc,gc,bc);
        }
    }

    /* Superfici caratteristiche proiettate sulla stessa geometria. */
    draw_projected_ring(g_kp.r_ergo,r_min,r_max,a,cx,cy,scale,230,115,20,1);
    draw_projected_ring(g_kp.r_isco,r_min,r_max,a,cx,cy,scale, 41,128,185,1);
    draw_projected_ring(g_kp.r_ph  ,r_min,r_max,a,cx,cy,scale, 39,174, 96,1);

    double zh=kerr_embedding_depth(g_kp.rp*1.005,a);
    int hx=(int)cx;
    int hy=(int)(cy+zh*0.58*sin(g_cam_pitch)*scale);
    int hrx=(int)(g_kp.rp*scale);
    int hry=(int)(fmax(2.0,g_kp.rp*cos(g_cam_pitch)*scale));
    fill_ellipse(hx,hy,hrx,hry,0,0,0);
    draw_projected_ring(g_kp.rp*1.005,r_min,r_max,a,cx,cy,scale,192,57,43,0);
    draw_diagnostic_worldlines(cx,cy,scale,r_min,r_max,a);
}

/* ------------------------------------------------------------------ */
/*  RENDER 1: GEODETICHE NULLE                                          */
/* ------------------------------------------------------------------ */
static void render_geodesics(void)
{
    for(int i=0;i<WIDTH*HEIGHT*4;i+=4){
        pixels[i]=8;pixels[i+1]=7;pixels[i+2]=5;pixels[i+3]=255;
    }

    double cx=WIDTH/2.0, cy=HEIGHT/2.0;
    double scale=g_zoom*19.0;
    double r_max=g_remit*2.4;

    /* Griglia circolare tenue */
    int icx=(int)cx,icy=(int)cy;
    for(int rg=5;rg<=(int)r_max;rg+=5)
        draw_circle(icx,icy,(int)(rg*scale),30,26,20,0);
    draw_text5(16,18,"GEODETICHE NULLE - SISTEMA NON ROTANTE",116,150,175,1);

    int n=g_nrays;
    if (!g_geo_cache_valid) {
        build_geodesic_cache();
    }

    for(int i=0;i<n;i++){
        GeodesicPath *path=&g_geo_cache[i];
        double frac=(n>1)?((double)i/(n-1))*2.0-1.0:0.0;
        double b=frac*g_remit*0.90;
        if(path->n<2) continue;

        /* Colore basato su parametro d'impatto e esito */
        uint8_t R,G,B;
        if(path->captured){
            double t=fmin(1.0,(double)path->n/500.0);
            R=(uint8_t)(220);G=(uint8_t)(150*(1-t*0.4));B=20;
        } else {
            double b_norm=(b+g_remit*0.90)/(2.0*g_remit*0.90);
            double lam=380.0+b_norm*370.0;
            wavelength_rgb(lam,&R,&G,&B);
        }

        for(int j=1;j<path->n;j++){
            double phi0=path->phi[j-1];
            double phi1=path->phi[j  ];
            double wx0=path->r[j-1]*cos(phi0);
            double wy0=path->r[j-1]*sin(phi0);
            double wx1=path->r[j  ]*cos(phi1);
            double wy1=path->r[j  ]*sin(phi1);
            int sx0,sy0,sx1,sy1;
            w2s(wx0,wy0,cx,cy,scale,&sx0,&sy0);
            w2s(wx1,wy1,cx,cy,scale,&sx1,&sy1);
            draw_line(sx0,sy0,sx1,sy1,R,G,B);
        }
    }

    draw_circle(icx,icy,(int)(g_kp.r_ergo*scale),230,115,20,1);
    draw_circle(icx,icy,(int)(g_kp.r_isco*scale), 41,128,185,1);
    draw_circle(icx,icy,(int)(g_kp.r_ph  *scale), 39,174, 96,1);
    draw_circle(icx,icy,(int)(g_kp.rp    *scale),192, 57, 43,0);
    fill_circle(icx,icy,(int)(g_kp.rp    *scale),  0,  0,  0);
}

/* ------------------------------------------------------------------ */
/*  RENDER 2: LENTE GRAVITAZIONALE                                     */
/* ------------------------------------------------------------------ */
static double source_sample(double tx,double ty,
                             uint8_t *R,uint8_t *G,uint8_t *B)
{
    double r=sqrt(tx*tx+ty*ty);
    double phi=atan2(ty,tx);
    double bulge=exp(-r*r*0.55)*0.45;
    double arm=0.0;
    for(int k=0;k<2;k++){
        double p=phi-PI*k-log(r+0.05)*1.9;
        double w=fmod(fabs(p),TWO_PI); if(w>PI) w=TWO_PI-w;
        arm+=exp(-w*w*3.2)*exp(-r*0.45)*0.48;
    }
    double grid=0.0;
    double gx=fabs(sin(tx*10.0)), gy=fabs(sin(ty*10.0));
    if(gx<0.055) grid+=0.30*(1.0-gx/0.055);
    if(gy<0.055) grid+=0.30*(1.0-gy/0.055);

    double dx1=tx-0.45, dy1=ty+0.10;
    double dx2=tx+0.42, dy2=ty-0.34;
    double src1=exp(-(dx1*dx1+dy1*dy1)*15.0);
    double src2=0.70*exp(-(dx2*dx2+dy2*dy2)*24.0);

    double fx=tx*17.0,fy=ty*17.0;
    double stars=0.0;
    for(int i=-1;i<=1;i++) for(int j=-1;j<=1;j++){
        double dx=fx-round(fx)-i*0.6, dy=fy-round(fy)-j*0.6;
        double d2=dx*dx+dy*dy;
        if(d2<0.018) stars+=0.50*exp(-d2*70.0);
    }
    double tot=fmin(1.0,bulge+arm+grid+src1+src2+stars);
    *R=(uint8_t)(fmin(255.0,(bulge*0.90+arm*0.65+grid*0.22+src1*1.00+src2*0.45+stars*1.0)*235));
    *G=(uint8_t)(fmin(255.0,(bulge*0.72+arm*0.78+grid*0.55+src1*0.82+src2*0.75+stars*0.9)*235));
    *B=(uint8_t)(fmin(255.0,(bulge*0.45+arm*1.00+grid*1.00+src1*0.30+src2*1.00+stars*0.8)*235));
    return tot;
}

static void render_lens(void)
{
    for(int i=0;i<WIDTH*HEIGHT*4;i+=4){
        pixels[i]=5;pixels[i+1]=4;pixels[i+2]=3;pixels[i+3]=255;
    }

    double cx=WIDTH/2.0, cy=HEIGHT/2.0;
    double scale=g_zoom*19.0;
    double a=g_a;
    double r_ph=g_kp.r_ph;
    double shadow_shift=0.72*a;

    int step=2;
    for(int sy=0;sy<HEIGHT;sy+=step){
        for(int sx=0;sx<WIDTH;sx+=step){
            double wx=(sx-cx)/scale, wy=-(sy-cy)/scale;
            double r=sqrt(wx*wx+wy*wy);
            double xs=wx+shadow_shift;
            double ys=wy*(1.0+0.08*a*a);
            double rs=sqrt(xs*xs+ys*ys);
            double phs=atan2(ys,xs);
            double shadow_r=r_ph*(1.0-0.08*a*cos(phs));

            uint8_t R=0,G=0,B=0;

            if(rs<shadow_r){
                /* Ombra Kerr: approssimata come disco spostato e lievemente
                 * deformato; il bordo vicino a r_ph rappresenta la cattura. */
            } else {
                double b=fmax(0.08,r);
                double ux=wx/b, uy=wy/b;
                double bi=1.0/b;
                double alpha=4.0*bi + (15.0*PI/4.0)*bi*bi;
                alpha=fmin(alpha,9.0);

                /* Equazione lente schematica: deflessione radiale più
                 * termine perpendicolare di frame dragging. La sorgente è
                 * una griglia luminosa; la curvatura deve piegarla. */
                double spin_shear=2.4*a*bi*bi;
                double beta_x=wx-alpha*ux - spin_shear*uy;
                double beta_y=wy-alpha*uy + spin_shear*ux;
                double tx=0.075*beta_x+0.18;
                double ty=0.075*beta_y-0.05;
                double brightness=source_sample(tx,ty,&R,&G,&B);

                double ring_gain=exp(-pow((rs-shadow_r*1.05)/(0.28+0.08*a),2.0));
                double jac=fabs(1.0-alpha/(b+0.35));
                double mu=fmin(3.8,1.0/(0.32+jac));
                double bright=clamp01((0.70+0.30*brightness)*(0.48+0.36*sqrt(mu)+0.26*ring_gain));
                R=(uint8_t)(R*bright);
                G=(uint8_t)(G*bright);
                B=(uint8_t)(B*bright);

                double ring_center=shadow_r*1.18;
                double ring_width=0.18+0.04*a;
                double ring=exp(-pow((rs-ring_center)/ring_width,2.0));
                if(ring>0.01){
                    double phase=phs-g_flow_t*(0.08+0.24*a);
                    double dop=clamp01(0.55+0.45*(1.0+a*sin(phase))*0.5);
                    double hot=ring*(0.45+0.90*dop);
                    R=(uint8_t)fmin(255.0,(double)R+235.0*hot);
                    G=(uint8_t)fmin(255.0,(double)G+120.0*hot);
                    B=(uint8_t)fmin(255.0,(double)B+ 26.0*hot);
                }
            }

            for(int dy=0;dy<step&&sy+dy<HEIGHT;dy++)
            for(int dx=0;dx<step&&sx+dx<WIDTH;dx++)
                put_pixel(sx+dx,sy+dy,R,G,B);
        }
    }

    int icx=(int)cx,icy=(int)cy;
    int shx=(int)(cx-shadow_shift*scale);
    draw_text5(16,18,"SFONDO LENSATO: GRIGLIA E SORGENTI",150,170,190,1);
    draw_text5(16,30,"ANELLO CALDO + OMBRA KERR",225,140,70,1);
    draw_circle(shx,icy,(int)(r_ph*scale),230,115,20,1);
    draw_circle(icx,icy,(int)(g_kp.rp*scale),192,57,43,0);
    fill_circle(icx,icy,(int)(g_kp.rp*scale)-1,0,0,0);
}

/* ------------------------------------------------------------------ */
/*  RENDER 3: DILATAZIONE TEMPORALE / REDSHIFT                         */
/* ------------------------------------------------------------------ */
static void render_redshift(void)
{
    for(int i=0;i<WIDTH*HEIGHT*4;i+=4){
        pixels[i]=10;pixels[i+1]=8;pixels[i+2]=5;pixels[i+3]=255;
    }

    double a=g_a;
    double r_min=g_kp.rp*1.001, r_max=28.0;
    int Nr=800;

    /* ---- Mappa cromatica 2D (metà sinistra) ---- */
    int map_w=WIDTH/2-10, map_h=HEIGHT-60;
    int map_ox=10, map_oy=30;
    double map_cx=map_ox+map_w/2.0, map_cy=map_oy+map_h/2.0;
    double map_sc=fmin(map_w,map_h)/2.0/(r_max*0.46);
    double lam_emit=550.0;

    for(int sy=map_oy;sy<map_oy+map_h;sy++){
        for(int sx=map_ox;sx<map_ox+map_w;sx++){
            double wx=(sx-map_cx)/map_sc, wy=-(sy-map_cy)/map_sc;
            double r=sqrt(wx*wx+wy*wy);
            if(r>r_max*0.46) continue;
            if(r<g_kp.rp){pixels[(sy*WIDTH+sx)*4]=0;pixels[(sy*WIDTH+sx)*4+1]=0;pixels[(sy*WIDTH+sx)*4+2]=0;pixels[(sy*WIDTH+sx)*4+3]=255;continue;}

            KerrMetric m; kerr_metric(r,a,&m);
            double neg_gtt=-m.gtt;
            uint8_t R,G,B;
            if(neg_gtt<=0.0){
                /* Ergosfera */
                R=230;G=115;B=20;
            } else {
                double z=1.0/sqrt(neg_gtt)-1.0;
                double lam_obs=lam_emit*(1.0+z);
                lam_obs=fmax(350.0,fmin(780.0,lam_obs));
                wavelength_rgb(lam_obs,&R,&G,&B);
                double bright=fmin(1.0,0.55+0.45*exp(-r*0.14));
                R=(uint8_t)(R*bright); G=(uint8_t)(G*bright); B=(uint8_t)(B*bright);
            }
            put_pixel(sx,sy,R,G,B);
        }
    }

    /* Cerchi sulla mappa */
    int icx=(int)map_cx, icy=(int)map_cy;
    draw_circle(icx,icy,(int)(g_kp.rp    *map_sc),255,255,255,0);
    draw_circle(icx,icy,(int)(g_kp.r_ergo*map_sc),230,115, 20,1);
    draw_circle(icx,icy,(int)(g_kp.r_isco*map_sc), 41,128,185,1);
    fill_circle(icx,icy,(int)(g_kp.rp*map_sc)-1,0,0,0);
    draw_text5(map_ox+8,map_oy+8,"MAPPA COLORE OSSERVATO",210,190,140,1);
    draw_text5(map_ox+8,map_oy+20,"ARANCIO: ERGOSFERA",230,125,40,1);

    /* ---- Grafici (metà destra) ---- */
    int gx=WIDTH/2+10, gw=WIDTH/2-20;
    int gy=30,  gh=HEIGHT-60;
    int p1h=(gh-30)/3, p2h=p1h, p3h=p1h;
    int p1y=gy, p2y=gy+p1h+15, p3y=gy+2*(p1h+15);

    /* Linee assi */
    draw_line(gx,p1y,gx,p1y+p1h,80,70,50);
    draw_line(gx,p1y+p1h,gx+gw,p1y+p1h,80,70,50);
    draw_line(gx,p2y,gx,p2y+p2h,80,70,50);
    draw_line(gx,p2y+p2h,gx+gw,p2y+p2h,80,70,50);
    draw_line(gx,p3y,gx,p3y+p3h,80,70,50);
    draw_line(gx,p3y+p3h,gx+gw,p3y+p3h,80,70,50);
    draw_text5(gx+7,p1y+6,"DTAU/DT TEMPO PROPRIO",90,175,225,1);
    draw_text5(gx+7,p2y+6,"LOG(1+Z) REDSHIFT",215,95,75,1);
    draw_text5(gx+7,p3y+6,"|OMEGA| FRAME DRAG",85,220,135,1);
    draw_text5(gx+gw-116,p3y+p3h-14,"ASSE X: LOG R",150,130,95,1);

    int px_prev=0,py_prev=0,first;
    double om_max=0.0;

    /* Pre-calcolo omega_max */
    for(int i=0;i<=Nr;i++){
        double u=(double)i/Nr;
        double r=r_min*pow(r_max/r_min,u);
        double om=fabs(kerr_frame_drag_rate(r,a));
        if(om>om_max) om_max=om;
    }
    if(om_max<1e-9) om_max=1.0;

    first=1;
    for(int i=0;i<=Nr;i++){
        double u=(double)i/Nr;
        double r=r_min*pow(r_max/r_min,u);
        KerrMetric m; kerr_metric(r,a,&m);
        int px=radial_log_x(r,r_min,r_max,gx,gw);

        /* dτ/dt */
        double neg_gtt=-m.gtt;
        double dtau=(neg_gtt>0.0)?sqrt(neg_gtt):0.0;
        int py=p1y+p1h-(int)(fmin(1.0,dtau)*p1h);
        if(!first) draw_line(px_prev,py_prev,px,py,52,152,219);
        if(i==0) first=0;
        px_prev=px; py_prev=py;
    }
    first=1;
    for(int i=0;i<=Nr;i++){
        double u=(double)i/Nr;
        double r=r_min*pow(r_max/r_min,u);
        KerrMetric m; kerr_metric(r,a,&m);
        int px=radial_log_x(r,r_min,r_max,gx,gw);
        double neg_gtt=-m.gtt;
        double dtau=(neg_gtt>0.0)?sqrt(neg_gtt):1e-4;
        double z=fmin(30.0,1.0/dtau-1.0);
        double zlog=log1p(z)/log1p(30.0);
        int py=p2y+p2h-(int)(clamp01(zlog)*p2h);
        if(!first) draw_line(px_prev,py_prev,px,py,192,57,43);
        if(i==0) first=0;
        px_prev=px; py_prev=py;
    }
    first=1;
    for(int i=0;i<=Nr;i++){
        double u=(double)i/Nr;
        double r=r_min*pow(r_max/r_min,u);
        int px=radial_log_x(r,r_min,r_max,gx,gw);
        double om=fabs(kerr_frame_drag_rate(r,a));
        int py=p3y+p3h-(int)(sqrt(clamp01(om/om_max))*p3h);
        if(!first) draw_line(px_prev,py_prev,px,py,46,204,113);
        if(i==0) first=0;
        px_prev=px; py_prev=py;
    }

    /* Linee verticali r+, ergo, ISCO */
    typedef struct{double r;uint8_t R,G,B;}VL;
    VL vl[]={{g_kp.rp,192,57,43},{g_kp.r_ergo,230,115,20},{g_kp.r_isco,41,128,185},{g_kp.r_ph,39,174,96}};
    for(int k=0;k<4;k++){
        if(vl[k].r<r_min||vl[k].r>r_max) continue;
        int xv=radial_log_x(vl[k].r,r_min,r_max,gx,gw);
        draw_line(xv,p1y,xv,p3y+p3h,vl[k].R,vl[k].G,vl[k].B);
    }
    draw_text5(gx+7,p1y+6,"DTAU/DT TEMPO PROPRIO",90,175,225,1);
    draw_text5(gx+7,p2y+6,"LOG(1+Z) REDSHIFT",215,95,75,1);
    draw_text5(gx+7,p3y+6,"|OMEGA| FRAME DRAG",85,220,135,1);
    draw_text5(gx+gw-116,p3y+p3h-14,"ASSE X: LOG R",150,130,95,1);
}

/* ------------------------------------------------------------------ */
/*  FUNZIONI ESPORTATE (chiamate da JS)                                 */
/* ------------------------------------------------------------------ */
EMSCRIPTEN_KEEPALIVE void set_spin(double a)
{
    g_a=fmax(0.001,fmin(0.998,a));
    kerr_props(g_a,&g_kp);
    g_kp_valid=1;
    invalidate_geodesic_cache();
}

EMSCRIPTEN_KEEPALIVE void set_zoom(double z)
{ g_zoom=fmax(0.2,fmin(4.5,z)); }

EMSCRIPTEN_KEEPALIVE void set_pitch(double p)
{ g_cam_pitch=fmax(0.10,fmin(1.40,p)); }

EMSCRIPTEN_KEEPALIVE void set_mode(int m)
{
    if(m<0) m=0;
    g_mode=(RenderMode)(m%MODE_COUNT);
}

EMSCRIPTEN_KEEPALIVE void set_nrays(int n)
{
    g_nrays=fmax(4,fmin(N_RAYS_MAX,n));
    invalidate_geodesic_cache();
}

EMSCRIPTEN_KEEPALIVE void set_remit(double r)
{
    g_remit=fmax(5.0,fmin(40.0,r));
    invalidate_geodesic_cache();
}

EMSCRIPTEN_KEEPALIVE void step_anim(void)
{
    double spin=fabs(g_a);
    g_phi_off+=0.010+0.018*spin;
    g_flow_t +=0.045+0.035*spin;
    if(g_phi_off>TWO_PI) g_phi_off-=TWO_PI;
    if(g_flow_t>10000.0) g_flow_t=fmod(g_flow_t,10000.0);
}

EMSCRIPTEN_KEEPALIVE double get_rp(void)    { ensure_kerr_props(); return g_kp.rp; }
EMSCRIPTEN_KEEPALIVE double get_rergo(void) { ensure_kerr_props(); return g_kp.r_ergo; }
EMSCRIPTEN_KEEPALIVE double get_risco(void) { ensure_kerr_props(); return g_kp.r_isco; }
EMSCRIPTEN_KEEPALIVE double get_rph(void)   { ensure_kerr_props(); return g_kp.r_ph; }
EMSCRIPTEN_KEEPALIVE double get_omH(void)   { ensure_kerr_props(); return g_kp.Omega_H; }
EMSCRIPTEN_KEEPALIVE int    get_width(void) { return WIDTH; }
EMSCRIPTEN_KEEPALIVE int    get_height(void){ return HEIGHT; }

/* Puntatore al buffer pixel (letto da JS per putImageData) */
EMSCRIPTEN_KEEPALIVE uint8_t* get_pixel_buffer(void){ return pixels; }

/* Funzione di render chiamata dal main loop */
EMSCRIPTEN_KEEPALIVE void render_frame(void)
{
    ensure_kerr_props();
    switch(g_mode){
        case MODE_GRID:     render_grid();      break;
        case MODE_GEODESIC: render_geodesics(); break;
        case MODE_LENS:     render_lens();      break;
        case MODE_REDSHIFT: render_redshift();  break;
        default: break;
    }
    /* Notify JS che il buffer è pronto */
    EM_ASM( Module.onFrameReady && Module.onFrameReady(); );
}

/* ------------------------------------------------------------------ */
/*  MAIN                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    kerr_props(g_a, &g_kp);
    g_kp_valid=1;
    /* Il loop è gestito da JS tramite requestAnimationFrame;
     * qui non usiamo emscripten_set_main_loop perché il controllo
     * del frame rate e della logica animazione è più pulito in JS. */
    return 0;
}
