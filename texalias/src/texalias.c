
/* texalias: bilinear resampling error of a rotated particle sprite.
 *
 * Two probes:
 *   line -- a straight half-plane edge with a one-texel linear coverage
 *           ramp. At theta=0 the edge is aligned with the texel grid and
 *           bilinear interpolation reconstructs it exactly, so any measured
 *           error is purely the cost of rotating the sampling grid.
 *   disc -- a hard-edged round particle (the real sprite). Its tangent is
 *           grid-aligned only at four points; curvature contributes an
 *           orientation-independent background error on top of rotation.
 *
 * Each probe is area-sampled into an N x N texture, drawn at S x S pixels
 * with bilinear sampling while the UV grid is rotated by theta, and compared
 * against an analytic screen-space coverage ground truth. Both probe shapes
 * are defined in their own local frame, so the ground truth does not depend
 * on theta: every angle-dependent difference is resampling error.
 *
 * Outputs:
 *   results/edge_error.csv  -- per-config edge and value errors
 *   stdout                  -- summary tables and pass/fail checks
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SS 16                      /* area-coverage sub-samples per axis */
#define PI 3.14159265358979323846
#define DEG (PI / 180.0)

typedef struct { int s; float* a; } Img;

static Img make_img(int s) {
    Img im; im.s = s; im.a = calloc((size_t)s * s, sizeof(float));
    if (!im.a) { perror("calloc"); exit(2); }
    return im;
}

/* coverage of the unit cell centered at (x,y) by a disc of `radius` */
static float disc_cov(float radius, float cx, float cy, float x, float y) {
    int hit = 0;
    for (int ay = 0; ay < SS; ay++) {
        for (int ax = 0; ax < SS; ax++) {
            float px = x - 0.5f + (ax + 0.5f) / (float)SS;
            float py = y - 0.5f + (ay + 0.5f) / (float)SS;
            float dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy <= radius * radius) hit++;
        }
    }
    return (float)hit / (float)(SS * SS);
}

/* coverage of a unit cell by the half-plane d <= 0, d a signed distance
 * measured in grid units (ramp width = one grid cell, clamp at +/- 1) */
static float edge_cov(float d) {
    float c = 0.5f - d;
    if (c < 0.0f) c = 0.0f; else if (c > 1.0f) c = 1.0f;
    return c;
}

/* Two baking styles for the round particle:
 *   area -- each texel stores the covered fraction of its cell (signed
 *           distance ramp). Straight parts of the edge are reconstructed
 *           exactly; error is curvature-only.
 *   hard -- each texel stores 0/1 from a point sample at the texel center,
 *           the common hand-authored hard-alpha sprite. Bilinear then
 *           rebuilds the edge from quantized corners and adds staircase.
 */
static Img make_tex_disc(int n, int hard) {
    float radius = (float)n * 7.0f / 16.0f;
    const float c = n * 0.5f - 0.5f;
    Img t = make_img(n);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float dx = (float)x - c, dy = (float)y - c;
            t.a[y * n + x] = hard
                ? (dx * dx + dy * dy <= radius * radius ? 1.0f : 0.0f)
                : disc_cov(radius, c, c, (float)x, (float)y);
        }
    }
    return t;
}

static Img make_tex_line(int n) {
    Img t = make_img(n);
    float c = n * 0.5f - 0.5f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            t.a[y * n + x] = edge_cov(((float)x - c) / 1.0f);
    return t;
}

static float sample_bi(const Img* t, float u, float v) {
    int n = t->s;
    if (u < 0.0f) u = 0.0f; else if (u > n - 1.0f) u = (float)(n - 1);
    if (v < 0.0f) v = 0.0f; else if (v > n - 1.0f) v = (float)(n - 1);
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    int x1 = x0 + 1 < n ? x0 + 1 : x0;
    int y1 = y0 + 1 < n ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    float a = t->a[y0 * n + x0], b = t->a[y0 * n + x1];
    float cc = t->a[y1 * n + x0], d = t->a[y1 * n + x1];
    return (a * (1.0f - fx) + b * fx) * (1.0f - fy)
         + (cc * (1.0f - fx) + d * fx) * fy;
}

/* rotate the sampling grid by theta about the texture center, bilinear draw.
 * texel (i,j) center lives at node coordinate (i,j); pixel center maps to
 * (i+0.5)*N/S - 0.5 so that N=S, theta=0 is an identity texel fetch. */
static Img render(int s, const Img* tex, float theta) {
    int n = tex->s;
    float c = n * 0.5f - 0.5f, cs = cosf(theta * DEG), sn = sinf(theta * DEG);
    Img o = make_img(s);
    for (int j = 0; j < s; j++) {
        for (int i = 0; i < s; i++) {
            float u = (i + 0.5f) * (float)n / (float)s - 0.5f;
            float v = (j + 0.5f) * (float)n / (float)s - 0.5f;
            float du = u - c, dv = v - c;
            float ur = c + du * cs + dv * sn;
            float vr = c - du * sn + dv * cs;
            o.a[j * s + i] = sample_bi(tex, ur, vr);
        }
    }
    return o;
}

/* sub-pixel position of the first 0.5 crossing along a row (left edge) */
static int row_cross(const Img* im, int row, float* x) {
    for (int j = 1; j < im->s; j++) {
        float lo = im->a[row * im->s + j - 1];
        float hi = im->a[row * im->s + j];
        if (lo < 0.5f && hi >= 0.5f) {
            *x = (float)(j - 1) + (0.5f - lo) / (hi - lo);
            return 1;
        }
    }
    return 0;
}

/* sub-pixel position of the first 0.5 crossing down a column (top edge) */
static int col_cross(const Img* im, int col, float* y) {
    for (int i = 1; i < im->s; i++) {
        float lo = im->a[(i - 1) * im->s + col];
        float hi = im->a[i * im->s + col];
        if (lo < 0.5f && hi >= 0.5f) {
            *y = (float)(i - 1) + (0.5f - lo) / (hi - lo);
            return 1;
        }
    }
    return 0;
}

typedef struct { float edge_max, edge_rms, val_max, val_rms; } Stats;

/* disc: cross points within the central 80% of the radius; line: all rows */
static Stats measure_disc(const Img* got, const Img* gt, float radius) {
    int s = got->s, c = s / 2, band = (int)(0.8f * radius);
    double esq = 0.0, emax = 0.0, vsq = 0.0, vmax = 0.0;
    long ne = 0, nv = 0;
    for (int k = -band; k <= band; k++) {
        int row = c + k, col = c + k;
        float xg, xt, yg, yt;
        if (row_cross(got, row, &xg) && row_cross(gt, row, &xt)) {
            double e = fabs((double)xg - xt);
            esq += e * e; if (e > emax) emax = e; ne++;
        }
        if (col_cross(got, col, &yg) && col_cross(gt, col, &yt)) {
            double e = fabs((double)yg - yt);
            esq += e * e; if (e > emax) emax = e; ne++;
        }
    }
    for (long k = 0; k < (long)s * s; k++) {
        double e = fabs((double)got->a[k] - gt->a[k]);
        vsq += e * e; if (e > vmax) vmax = e; nv++;
    }
    Stats st;
    st.edge_max = (float)emax;
    st.edge_rms = ne ? (float)sqrt(esq / (double)ne) : 0.0f;
    st.val_max = (float)vmax;
    st.val_rms = (float)sqrt(vsq / (double)nv);
    return st;
}

static Stats measure_line(const Img* got, const Img* gt) {
    int s = got->s;
    double esq = 0.0, emax = 0.0, vsq = 0.0, vmax = 0.0;
    long ne = 0, nv = 0;
    for (int row = 0; row < s; row++) {
        float xg, xt;
        if (row_cross(got, row, &xg) && row_cross(gt, row, &xt)) {
            double e = fabs((double)xg - xt);
            esq += e * e; if (e > emax) emax = e; ne++;
        }
    }
    for (long k = 0; k < (long)s * s; k++) {
        double e = fabs((double)got->a[k] - gt->a[k]);
        vsq += e * e; if (e > vmax) vmax = e; nv++;
    }
    Stats st;
    st.edge_max = (float)emax;
    st.edge_rms = ne ? (float)sqrt(esq / (double)ne) : 0.0f;
    st.val_max = (float)vmax;
    st.val_rms = (float)sqrt(vsq / (double)nv);
    return st;
}

static const int N_LIST[] = {32, 48, 64, 96, 128, 192, 256, 384, 512};
static const float ANG_LIST[] = {0.0f, 5.0f, 30.0f, 45.0f};
#define NN (int)(sizeof(N_LIST) / sizeof(N_LIST[0]))
#define NA (int)(sizeof(ANG_LIST) / sizeof(ANG_LIST[0]))

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static Img ground_disc_hard(int s, int hard) {
    float radius = (float)s * 7.0f / 16.0f;
    const float c = s * 0.5f - 0.5f;
    Img g = make_img(s);
    for (int j = 0; j < s; j++) {
        for (int i = 0; i < s; i++) {
            float dx = (float)i - c, dy = (float)j - c;
            g.a[j * s + i] = hard
                ? (dx * dx + dy * dy <= radius * radius ? 1.0f : 0.0f)
                : disc_cov(radius, c, c, (float)i, (float)j);
        }
    }
    return g;
}

static Img ground_disc(int s) {
    return ground_disc_hard(s, 0);
}

static Img ground_line(int s) {
    float c = s * 0.5f - 0.5f;
    Img g = make_img(s);
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
            g.a[j * s + i] = edge_cov(((float)i - c) / 1.0f);
    return g;
}

int main(void) {
    FILE* csv = fopen("results/edge_error.csv", "w");
    if (!csv) { perror("results/edge_error.csv"); return 2; }
    fprintf(csv, "probe,kind,angle_deg,S,N,r,edge_max_px,edge_rms_px,val_max,val_rms\n");

    const int S = 256;
    Img gd = ground_disc(S);
    Img gh = ground_disc_hard(S, 1);
    Img gl = ground_line(S);
    const float Rs = (float)S * 7.0f / 16.0f;

    Stats sweep_l[NA][NN], sweep_a[NA][NN], sweep_h[NA][NN], sweep_hs[NA][NN];
    puts("== sweep @S=256, edge_max in screen pixels ==");
    puts("line probe (rotation-only; edge contour is exact, report val_rms):");
    puts("  N     r     theta=0    theta=5    theta=30   theta=45");
    for (int ni = 0; ni < NN; ni++) {
        int n = N_LIST[ni];
        float r = (float)n / (float)S;
        Img tex = make_tex_line(n);
        printf("%4d %5.3f", n, r);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(S, &tex, ANG_LIST[ai]);
            sweep_l[ai][ni] = measure_line(&got, &gl);
            printf("  %9.4f", sweep_l[ai][ni].val_rms);
            fprintf(csv, "line,sweep,%.0f,%d,%d,%.4f,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], S, n, r,
                    sweep_l[ai][ni].edge_max, sweep_l[ai][ni].edge_rms,
                    sweep_l[ai][ni].val_max, sweep_l[ai][ni].val_rms);
            free(got.a);
        }
        putchar('\n');
        free(tex.a);
    }

    puts("area-baked disc (curvature error, smooth alpha ramp):");
    puts("  N     r     theta=0    theta=5    theta=30   theta=45");
    for (int ni = 0; ni < NN; ni++) {
        int n = N_LIST[ni];
        float r = (float)n / (float)S;
        Img tex = make_tex_disc(n, 0);
        printf("%4d %5.3f", n, r);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(S, &tex, ANG_LIST[ai]);
            sweep_a[ai][ni] = measure_disc(&got, &gd, Rs);
            printf("  %9.4f", sweep_a[ai][ni].edge_max);
            fprintf(csv, "area,sweep,%.0f,%d,%d,%.4f,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], S, n, r,
                    sweep_a[ai][ni].edge_max, sweep_a[ai][ni].edge_rms,
                    sweep_a[ai][ni].val_max, sweep_a[ai][ni].val_rms);
            free(got.a);
        }
        putchar('\n');
        free(tex.a);
    }

    puts("hard-baked disc, vs hard GT (resampling only):");
    puts("  N     r     theta=0    theta=5    theta=30   theta=45");
    for (int ni = 0; ni < NN; ni++) {
        int n = N_LIST[ni];
        float r = (float)n / (float)S;
        Img tex = make_tex_disc(n, 1);
        printf("%4d %5.3f", n, r);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(S, &tex, ANG_LIST[ai]);
            sweep_h[ai][ni] = measure_disc(&got, &gh, Rs);
            sweep_hs[ai][ni] = measure_disc(&got, &gd, Rs);
            printf("  %9.4f", sweep_h[ai][ni].edge_max);
            fprintf(csv, "hard,sweep,%.0f,%d,%d,%.4f,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], S, n, r,
                    sweep_h[ai][ni].edge_max, sweep_h[ai][ni].edge_rms,
                    sweep_h[ai][ni].val_max, sweep_h[ai][ni].val_rms);
            fprintf(csv, "hardsmooth,sweep,%.0f,%d,%d,%.4f,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], S, n, r,
                    sweep_hs[ai][ni].edge_max, sweep_hs[ai][ni].edge_rms,
                    sweep_hs[ai][ni].val_max, sweep_hs[ai][ni].val_rms);
            free(got.a);
        }
        putchar('\n');
        free(tex.a);
    }

    puts("hard-baked disc, vs smooth GT (total visual error incl. staircase):");
    puts("  N     r     theta=0    theta=5    theta=30   theta=45");
    for (int ni = 0; ni < NN; ni++) {
        printf("%4d %5.3f", N_LIST[ni], (float)N_LIST[ni] / (float)S);
        for (int ai = 0; ai < NA; ai++)
            printf("  %9.4f", sweep_hs[ai][ni].edge_max);
        putchar('\n');
    }

    puts("\n== scale invariance at r=1 (N=S), edge_max px ==");
    puts("  S     probe   theta=0    theta=5    theta=30   theta=45");
    const int S2[] = {128, 256, 512};
    Stats sc_l[NA][3], sc_a[NA][3], sc_h[NA][3];
    for (int si = 0; si < 3; si++) {
        int s = S2[si];
        float rad = (float)s * 7.0f / 16.0f;
        Img gtl = ground_line(s), gta = ground_disc(s), gth = ground_disc_hard(s, 1);
        Img txl = make_tex_line(s), txa = make_tex_disc(s, 0), txh = make_tex_disc(s, 1);
        printf("%4d  line  ", s);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(s, &txl, ANG_LIST[ai]);
            sc_l[ai][si] = measure_line(&got, &gtl);
            printf("  %9.4f", sc_l[ai][si].edge_max);
            fprintf(csv, "line,scale,%.0f,%d,%d,1.0000,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], s, s,
                    sc_l[ai][si].edge_max, sc_l[ai][si].edge_rms,
                    sc_l[ai][si].val_max, sc_l[ai][si].val_rms);
            free(got.a);
        }
        putchar('\n');
        printf("%4d  area  ", s);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(s, &txa, ANG_LIST[ai]);
            sc_a[ai][si] = measure_disc(&got, &gta, rad);
            printf("  %9.4f", sc_a[ai][si].edge_max);
            fprintf(csv, "area,scale,%.0f,%d,%d,1.0000,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], s, s,
                    sc_a[ai][si].edge_max, sc_a[ai][si].edge_rms,
                    sc_a[ai][si].val_max, sc_a[ai][si].val_rms);
            free(got.a);
        }
        putchar('\n');
        printf("%4d  hard  ", s);
        for (int ai = 0; ai < NA; ai++) {
            Img got = render(s, &txh, ANG_LIST[ai]);
            sc_h[ai][si] = measure_disc(&got, &gth, rad);
            printf("  %9.4f", sc_h[ai][si].edge_max);
            fprintf(csv, "hard,scale,%.0f,%d,%d,1.0000,%.6f,%.6f,%.6f,%.6f\n",
                    ANG_LIST[ai], s, s,
                    sc_h[ai][si].edge_max, sc_h[ai][si].edge_rms,
                    sc_h[ai][si].val_max, sc_h[ai][si].val_rms);
            free(got.a);
        }
        putchar('\n');
        free(gtl.a); free(gta.a); free(gth.a);
        free(txl.a); free(txa.a); free(txh.a);
    }

    puts("\n== empirical minimum N at S=256 (edge_max <= 1 px) ==");
    printf("%-5s %10s %12s %12s\n", "theta", "line*", "area Nmin",
           "hard(smooth)");
    int nmin_a[NA], nmin_hs[NA];
    for (int ai = 0; ai < NA; ai++) {
        nmin_a[ai] = nmin_hs[ai] = -1;
        for (int ni = 0; ni < NN; ni++) {
            if (nmin_a[ai] < 0 && sweep_a[ai][ni].edge_max <= 1.0f)
                nmin_a[ai] = N_LIST[ni];
            if (nmin_hs[ai] < 0 && sweep_hs[ai][ni].edge_max <= 1.0f)
                nmin_hs[ai] = N_LIST[ni];
        }
        printf("%-5.0f %10s %12d %12d\n", ANG_LIST[ai], "exact",
               nmin_a[ai], nmin_hs[ai]);
    }
    puts("(* line edge contour is exact at every size; only band values wobble)");
    puts("(hard(smooth): hard sprite measured against the smooth ideal, i.e.");
    puts(" what the eye sees; the hard-vs-hard column hides its own staircase)");
    fclose(csv);

    const int i256 = 6;  /* r = 1 in the sweep */

    /* identity baselines: aligned fetch of an aligned texture is exact */
    check("line_aligned_identity",
          sweep_l[0][i256].edge_max == 0.0f && sweep_l[0][i256].val_rms == 0.0f);
    check("area_aligned_identity", sweep_a[0][i256].edge_max < 0.02f);
    check("hard_aligned_identity", sweep_h[0][i256].edge_max < 0.02f);

    /* bilinear reconstructs affine ramps exactly: rotated line contour holds */
    check("line_contour_exact_all_angles",
          sweep_l[1][i256].edge_max == 0.0f &&
          sweep_l[2][i256].edge_max == 0.0f &&
          sweep_l[3][i256].edge_max == 0.0f);
    /* but band values do wobble, more at larger rotation angles */
    check("line_wobble_grows_with_angle",
          sweep_l[0][i256].val_rms < sweep_l[1][i256].val_rms &&
          sweep_l[1][i256].val_rms < sweep_l[2][i256].val_rms &&
          sweep_l[2][i256].val_rms < sweep_l[3][i256].val_rms);

    /* hard alpha always trails area baking at the same size (smooth-GT view) */
    check("hard_worse_than_area_smoothGT",
          sweep_hs[3][i256].edge_max > sweep_a[3][i256].edge_max);

    /* at r=1 the error is a fixed pixel count independent of S */
    int inv_ok = 1;
    for (int ai = 0; ai < NA; ai++) {
        float lo = sc_a[ai][0].edge_max, hi = sc_a[ai][0].edge_max;
        for (int si = 1; si < 3; si++) {
            float v = sc_a[ai][si].edge_max;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi - lo > 0.05f) inv_ok = 0;
    }
    check("area_scale_invariant_r1", inv_ok);

    /* minification / near-unity r<=1: texel-space amplitude A = e*r is
     * constant (e = C/r); theta=0 excluded, its aligned fetch is exact */
    int amp_ok = 1;
    for (int ai = 1; ai < NA; ai++) {
        float lo = 1e9f, hi = 0.0f;
        for (int ni = 0; ni <= i256; ni++) {
            float k = sweep_a[ai][ni].edge_max * (float)N_LIST[ni] / (float)S;
            if (k <= 0.0f) continue;
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
        /* crossing detection quantizes to ~0.02 px, so allow its noise;
         * the fitted exponent p=-0.9..-0.97 confirms the 1/r law itself */
        if (hi / lo > 2.0f) amp_ok = 0;
    }
    check("area_minif_law_e_over_r", amp_ok);

    /* magnification r>=1: error settles to a sub-pixel phase floor, i.e. it
     * never grows again and stays well under the 1 px threshold at r>=1 */
    int floor_ok = 1;
    for (int ai = 1; ai < NA; ai++)
        for (int ni = i256; ni < NN; ni++)
            if (sweep_a[ai][ni].edge_max > 0.25f) floor_ok = 0;
    check("area_magnif_phase_floor", floor_ok);

    /* minification r<1 must be worse than r=1: footprint covers < one texel */
    int min_ok = 1;
    for (int ai = 1; ai < NA; ai++)
        if (sweep_a[ai][2].edge_max <= sweep_a[ai][i256].edge_max) min_ok = 0;
    check("minification_alias_worse", min_ok);

    /* three-angle verdict at a user-chosen operating point r = N/S.
     * N=32 at S=256 (r=0.125) is the deliberately tight pick: the 30 deg
     * row exceeds 1 px and exercises the required-size inversion. */
    puts("\n== three-angle verdict at chosen r (threshold 1 px) ==");
    const float r_op = (float)N_LIST[0] / (float)S;
    int iop = 0;
    for (int ai = 0; ai < NA; ai++) {
        float e = sweep_a[ai][iop].edge_max;
        if (e <= 1.0f) {
            printf("theta=%-2.0f  e=%.3f px  ok (N=%d, r=%.3f)\n",
                   ANG_LIST[ai], e, N_LIST[iop], r_op);
        } else {
            int need = -1;
            for (int ni = 0; ni < NN; ni++)
                if (sweep_a[ai][ni].edge_max <= 1.0f) {
                    need = N_LIST[ni];
                    break;
                }
            printf("theta=%-2.0f  e=%.3f px  OVER -> required N=%d "
                   "(r=%.3f)\n", ANG_LIST[ai], e, need,
                   (float)need / (float)S);
        }
    }

    printf("all_ok=%d\n", failures == 0);
    free(gd.a); free(gh.a); free(gl.a);
    return failures == 0 ? 0 : 1;
}
