
/* softground: 粒子与地面交叉的软过渡。
   片元 alpha 按离地高度衰减，衰减长度由关系式反解（见 README.md），不是试出来的。 */
#include <stdio.h>
#include <math.h>

#define K 4.0f          /* 屏幕比例：像素 / 世界单位 */
#define SCR_H 160
#define GROUND_ROW 100  /* 地面线在第 GROUND_ROW-1 与 GROUND_ROW 行之间 */

static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* 片元离地高度 y（世界单位，向上为正）→ alpha */
static float fade_alpha(float y, float lambda) {
    if (lambda <= 0.0f) return y > 0.0f ? 1.0f : 0.0f;
    return clampf(y / lambda, 0.0f, 1.0f);
}

/* 有效衰减长度：基础值 λ 由关系式反解；粒子离地时衰减带随之收窄，
   完全离开地面（δ ≥ r）时为零 —— 不交叉不淡化，且在接触处连续 */
static float lambda_eff(float delta, float r, float lambda) {
    return clampf(r - delta, 0.0f, lambda);
}

/* 关系式：过渡带宽（像素）= K · |[0, λ_eff] ∩ [δ−r, δ+r]| */
static float predicted_width(float delta, float r, float lambda) {
    float le = lambda_eff(delta, r, lambda);
    float lo = fmaxf(0.0f, delta - r), hi = fminf(le, delta + r);
    return K * fmaxf(0.0f, hi - lo);
}

static float pix_wy(int py) { return (GROUND_ROW - py - 0.5f) / K; }
static float pix_wx(int px) { return (px + 0.5f) / K; }

/* 粒子圆盘内返回片元 alpha，圆盘外返回 -1 */
static float particle_alpha(int px, int py, float cx, float delta, float r, float lambda) {
    float dx = pix_wx(px) - cx, dy = pix_wy(py) - delta;
    if (dx * dx + dy * dy > r * r) return -1.0f;
    return fade_alpha(pix_wy(py), lambda_eff(delta, r, lambda));
}

/* 沿过粒子中心的采样列统计过渡像素（0 < α < 1），并回传粒子上 α 的极值 */
static int measure(int col, float cx, float delta, float r, float lambda,
                   float* amin, float* amax) {
    int w = 0;
    *amin = 2.0f; *amax = -1.0f;
    for (int py = 0; py < SCR_H; py++) {
        float a = particle_alpha(col, py, cx, delta, r, lambda);
        if (a < 0.0f) continue;
        if (a < *amin) *amin = a;
        if (a > *amax) *amax = a;
        if (a > 0.0f && a < 1.0f) w++;
    }
    return w;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static char ach(float a) {
    const char* ramp = " .:-=+*#%@";
    int i = (int)(a * 9.0f + 0.5f);
    return ramp[i < 0 ? 0 : (i > 9 ? 9 : i)];
}

/* 同一粒子（r=4, δ=0）的硬切边与软过渡对照 */
static void render_compare(float cx) {
    const float r = 4.0f, delta = 0.0f;
    const int c0 = 64, hw = 20, hh = 20;
    printf("\n        hard clip         |         soft fade\n");
    for (int py = GROUND_ROW - hh; py < GROUND_ROW + hh; py++) {
        if (py == GROUND_ROW) {
            for (int i = 0; i < 2 * hw + 1; i++) putchar('-');
            printf(" | ");
            for (int i = 0; i < 2 * hw + 1; i++) putchar('-');
            putchar('\n');
        }
        for (int panel = 0; panel < 2; panel++) {
            for (int px = c0 - hw; px <= c0 + hw; px++) {
                float dx = pix_wx(px) - cx, dy = pix_wy(py) - delta;
                float a;
                if (dx * dx + dy * dy > r * r) { putchar(' '); continue; }
                if (panel == 0) a = pix_wy(py) > 0.0f ? 1.0f : 0.0f;
                else a = fade_alpha(pix_wy(py), lambda_eff(delta, r, r));
                putchar(ach(a));
            }
            printf(panel == 0 ? " | " : "\n");
        }
    }
    putchar('\n');
}

int main(void) {
    const int col = 64;
    const float cx = (col + 0.5f) / K;
    float amin, amax;

    /* 三档尺寸，同一采样线，中心贴地（δ=0）；λ=r 由关系式反解 */
    const float rs[3] = { 2.0f, 4.0f, 8.0f };
    int w[3];
    printf("== 过渡宽度：三档尺寸 (delta=0, col=%d) ==\n", col);
    for (int i = 0; i < 3; i++) {
        w[i] = measure(col, cx, 0.0f, rs[i], rs[i], &amin, &amax);
        printf("s=%.0fpx r=%.0f  W=%d px  (公式预测 %.0f)\n",
               2.0f * rs[i] * K, rs[i], w[i], predicted_width(0.0f, rs[i], rs[i]));
    }

    /* 同一粒子（r=4）改变中心-地面深度差，宽度应随 δ 变化 */
    const float ds[7] = { -6.0f, -4.0f, -2.0f, 0.0f, 2.0f, 4.0f, 6.0f };
    int wd[7];
    printf("\n== 过渡宽度：深度差扫描 (r=4, s=32px) ==\n");
    for (int i = 0; i < 7; i++) {
        wd[i] = measure(col, cx, ds[i], 4.0f, 4.0f, &amin, &amax);
        printf("delta=%+.0f  W=%d px  (公式预测 %.0f)\n",
               ds[i], wd[i], predicted_width(ds[i], 4.0f, 4.0f));
    }

    /* 边界 */
    measure(col, cx, -6.0f, 4.0f, 4.0f, &amin, &amax);
    float buried_a = amax;
    measure(col, cx, 6.0f, 4.0f, 4.0f, &amin, &amax);
    float above_a = amin;
    measure(col, cx, 4.0f, 4.0f, 4.0f, &amin, &amax);
    float resting_a = amin;
    printf("\n== 边界 ==\n");
    printf("完全埋入 (delta=-6, r=4): alpha=%.2f\n", buried_a);
    printf("完全离地 (delta=+6, r=4): alpha=%.2f\n", above_a);
    printf("贴地未交叉 (delta=+4, r=4): alpha=%.2f\n", resting_a);

    check("width_s16", w[0] == 8);
    check("width_s32", w[1] == 16);
    check("width_s64", w[2] == 32);
    check("width_scales_with_size", w[0] < w[1] && w[1] < w[2]);
    int ok_formula = 1;
    for (int i = 0; i < 7; i++)
        if (fabsf(predicted_width(ds[i], 4.0f, 4.0f) - (float)wd[i]) > 0.5f) ok_formula = 0;
    check("width_matches_formula", ok_formula);
    check("width_varies_with_delta", wd[0] == 0 && wd[3] == 16 && wd[6] == 0);
    check("buried_alpha_zero", buried_a == 0.0f);
    check("above_no_fade", above_a == 1.0f);
    check("resting_no_fade", resting_a == 1.0f);

    render_compare(cx);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
