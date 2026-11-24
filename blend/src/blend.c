
/* blend: 正常 source-over 与叠加 additive 混合律、相对亮度判别，
   以及多颗火花重叠时的中心过曝上限。`make test` 跑自检。 */
#include <stdio.h>
#include <math.h>

#define WHITE 255.0f
/* 多颗重叠复合后“中心峰值亮度”的硬上限(8-bit code value)。
   取 235 = BT.601/709 限幅范围的基准白，距硬钳位白点 255 留 20 码
   (约 7.8%) 余量，吸收取整/抖动/额外重叠，且保证不出现 Y==255 死白平台。 */
#define OVERLAP_CEIL 235.0f

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int clamp_u8(float x) {
    long v = lroundf(x);
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int)v;
}

/* 正常 alpha 混合(直 alpha): Y = b*(1-a) + s*a */
static float normal_f(float b, float s, float a) { return b * (1.0f - a) + s * a; }
static int   normal_u8(float b, float s, float a) { return clamp_u8(normal_f(b, s, a)); }

/* 叠加 additive: HDR 累加 H = b + s*a；显示时钳到白点 */
static float additive_hdr(float b, float s, float a) { return b + s * a; }
static int   additive_u8(float b, float s, float a) { return clamp_u8(additive_hdr(b, s, a)); }

/* 相对增亮 ΔY = 复合码值 - 背景码值 */
static int rel_brightness(int y, float b) { return y - clamp_u8(b); }

/* 在背景 b 上，把 HDR 火花中心峰值 h_peak 压到复合后 <= ceil 所需增益。
   b + g*h_peak <= ceil  =>  g = min(1, (ceil-b)/h_peak)；背景本身已亮于
   ceil 时没有合法增益(返回 0)，需改用软压/tonemap。 */
static float exposure_gain(float h_peak, float b, float ceilv) {
    if (b >= ceilv) return 0.0f;
    float g = (ceilv - b) / h_peak;
    return g > 1.0f ? 1.0f : g;
}

int main(void) {
    const float s = 255.0f, a = 0.40f;   /* 与 artifacts/blend-lab 标定光场一致 */
    const float bg[3] = {0.0f, 128.0f, 255.0f};

    int dn[3], da[3];
    for (int i = 0; i < 3; i++) {
        dn[i] = rel_brightness(normal_u8(bg[i], s, a), bg[i]);
        da[i] = rel_brightness(additive_u8(bg[i], s, a), bg[i]);
        printf("normal   b=%3.0f Y=%3d dY=%3d\n", bg[i], normal_u8(bg[i], s, a), dn[i]);
        printf("additive b=%3.0f Y=%3d dY=%3d (hdr dY=%.1f)\n",
               bg[i], additive_u8(bg[i], s, a), da[i], s * a);
    }

    /* 解析真值(含取整)：正常 102 / 51 / 0；叠加 LDR 102 / 102 / 0 */
    check("normal_black_dY_102", dn[0] == 102);
    check("normal_gray_dY_51", dn[1] == 51);
    check("normal_white_dY_0", dn[2] == 0);
    check("additive_black_dY_102", da[0] == 102);
    check("additive_gray_dY_102", da[1] == 102);
    check("additive_white_clipped_0", da[2] == 0);

    /* 判别式：正常混合相对亮度随背景严格单调下降 */
    check("normal_monotone_decrease", dn[0] > dn[1] && dn[1] > dn[2]);

    /* 叠加在未钳位的浮点域，相对增亮恒等于 s*a=102，与 b 无关 */
    int additive_flat = 1;
    for (int i = 0; i < 3; i++)
        if (fabsf((additive_hdr(bg[i], s, a) - bg[i]) - s * a) > 1e-6f)
            additive_flat = 0;
    check("additive_float_flat", additive_flat);

    /* 灰底是两种混合的分离点：正常 51，叠加 102 */
    check("gray_separates_modes", dn[1] == 51 && da[1] == 102);

    /* 过曝上限：单颗峰 80，6 颗同点 HDR 峰 480 -> 未抑制硬钳出死白 */
    const float p1 = 80.0f;
    int unc_n4 = additive_u8(0.0f, 4 * p1, 1.0f);  /* 320 -> 255 钳位 */
    int unc_n3 = additive_u8(0.0f, 3 * p1, 1.0f);  /* 240 不钳 */
    check("overlap_n3_below_white", unc_n3 == 240);
    check("overlap_n4_hardclips_white", unc_n4 == 255);

    const int budget = (int)floorf(OVERLAP_CEIL / p1);  /* floor(235/80)=2 */
    check("count_budget_two_colocated", budget == 2);

    float h6 = 6.0f * p1;                               /* 480 */
    float g0 = exposure_gain(h6, 0.0f, OVERLAP_CEIL);
    int capped_black = clamp_u8(g0 * h6);
    printf("overlap n=6 hdr=%.0f gain=%.4f capped_peak=%d\n", h6, g0, capped_black);
    check("cap_peak_equals_235_on_black", capped_black == 235);
    check("cap_below_white_no_deadwhite", capped_black < 255);

    /* 中灰底也要保证复合后 <=235：g=(235-128)/480，终值恰为 235 */
    float g128 = exposure_gain(h6, 128.0f, OVERLAP_CEIL);
    int capped_gray = clamp_u8(128.0f + g128 * h6);
    check("cap_peak_235_on_gray", capped_gray == 235);

    printf("overexpose_ceil=%.0f\n", OVERLAP_CEIL);
    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
