
/*
 * smoke/record.c — 十秒烟雾粒子轨迹录制器。
 *
 * 轨迹积分直接复用 field/src/field.c 里的那个 static step()
 * （用宏把它的自测 main 改名后 #include 进来），保证录到的运动
 * 与被诊断的力场模块是同一份积分代码，而不是另写的近似版本。
 *
 * 本程序额外把每一步三种力各自造成的速度增量
 *   dv_gravity / dv_drag / dv_swirl
 * 拆出来写进 CSV，供贡献占比分析。每一步还会用拆分增量重建速度，
 * 与真正的 step() 结果核对，不一致就非零退出。
 *
 * 用法:
 *   ./record OUT.csv DURATION DT SEED RATE LIFE \
 *            SPAWN_R VX_SIGMA VY0 VY_JITTER GX GY DRAG SWIRL
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* 引入仓库原生积分器，同时避开它的 main() 符号冲突 */
#define main field_selftest_main
#include "../../field/src/field.c"
#undef main

#define MAXP 1024

typedef struct {
    Part p;
    int id;
    int alive;
    float age;
} Rec;

/* ---- 确定性伪随机（splitmix64），两次录制同一 seed 必须逐位一致 ---- */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static double rand_u01(void) {
    return (rng_next() >> 11) * (1.0 / 9007199254740992.0); /* 53 bit / 2^53 */
}
static int have_spare = 0;
static double spare_gauss = 0.0;
static double rand_gauss(void) {
    if (have_spare) { have_spare = 0; return spare_gauss; }
    double u = 0.0, v = 0.0;
    while (u <= 1e-12) u = rand_u01();
    v = rand_u01();
    double mag = sqrt(-2.0 * log(u));
    spare_gauss = mag * sin(2.0 * 3.14159265358979323846 * v);
    have_spare = 1;
    return mag * cos(2.0 * 3.14159265358979323846 * v);
}

int main(int argc, char** argv) {
    if (argc != 15) {
        fprintf(stderr, "usage: %s OUT DURATION DT SEED RATE LIFE R VXSIG VY0 VYJIT GX GY DRAG SWIRL\n",
                argv[0]);
        return 2;
    }
    const char* out_path = argv[1];
    double duration = atof(argv[2]);
    double dt       = atof(argv[3]);
    rng_state       = (uint64_t)strtoull(argv[4], NULL, 10);
    double rate     = atof(argv[5]);
    double life     = atof(argv[6]);
    double spawn_r  = atof(argv[7]);
    double vx_sigma = atof(argv[8]);
    double vy0      = atof(argv[9]);
    double vy_jit   = atof(argv[10]);
    float gx = (float)atof(argv[11]);
    float gy = (float)atof(argv[12]);
    float drag  = (float)atof(argv[13]);
    float swirl = (float)atof(argv[14]);

    if (duration <= 0.0 || dt <= 0.0 || dt > 0.1 || life <= 0.0 || rate < 0.0 || drag < 0.0) {
        fprintf(stderr, "record: bad scene parameter\n");
        return 2;
    }

    int frames = (int)llround(duration / dt);

    static Rec ps[MAXP];
    int n = 0, next_id = 0;
    double rate_acc = 0.0;
    double max_verify_err = 0.0;
    long long rows = 0;

    FILE* f = fopen(out_path, "w");
    if (!f) { perror("fopen out"); return 2; }
    fprintf(f, "frame,t,id,x,y,vx,vy,"
               "dvx_g,dvy_g,dvx_d,dvy_d,dvx_s,dvy_s\n");

    for (int fi = 0; fi <= frames; fi++) {
        double t = fi * dt;

        /* 恒速发射（带小数累积器），与 emitter 模块同思路 */
        if (fi > 0) {
            rate_acc += rate * dt;
            while (rate_acc >= 1.0) {
                rate_acc -= 1.0;
                if (n < MAXP) {
                    double ang = 2.0 * 3.14159265358979323846 * rand_u01();
                    double rr = spawn_r * sqrt(rand_u01());
                    Rec* q = &ps[n++];
                    q->p.x = (float)(rr * cos(ang));
                    q->p.y = (float)(rr * sin(ang));
                    q->p.vx = (float)(vx_sigma * rand_gauss());
                    q->p.vy = (float)(vy0 + vy_jit * rand_u01());
                    q->id = next_id++;
                    q->alive = 1;
                    q->age = 0.0f;
                }
            }
        }

        for (int i = 0; i < n; i++) {
            Rec* q = &ps[i];
            if (!q->alive || q->age >= (float)life) continue;

            Part before = q->p;

            /* 三项各自对本帧速度的增量，公式与 field.c::step 严格一致 */
            float k = expf(-drag * (float)dt);
            float dvx_d = before.vx * (k - 1.0f);
            float dvy_d = before.vy * (k - 1.0f);

            float r = sqrtf(before.x * before.x + before.y * before.y);
            float dvx_s = 0.0f, dvy_s = 0.0f;
            if (r > 1e-6f && swirl != 0.0f) {
                float tx = -before.y / r;
                float ty = before.x / r;
                dvx_s = tx * swirl * (float)dt;
                dvy_s = ty * swirl * (float)dt;
            }

            float dvx_g = gx * (float)dt;
            float dvy_g = gy * (float)dt;

            /* 用仓库原生 step() 推进真实状态 */
            if (step(&q->p, (float)dt, gx, gy, drag, swirl) != 0) {
                fprintf(stderr, "record: field step rejected parameters\n");
                fclose(f);
                return 3;
            }

            /* 一致性核对：拆分增量之和必须等于真实速度变化 */
            float ex_vx = before.vx + dvx_d + dvx_s + dvx_g;
            float ex_vy = before.vy + dvy_d + dvy_s + dvy_g;
            double verr = fmax(fabs((double)ex_vx - q->p.vx),
                               fabs((double)ex_vy - q->p.vy));
            if (verr > max_verify_err) max_verify_err = verr;

            q->age += (float)dt;
            if (q->age >= (float)life) { q->alive = 0; continue; }

            fprintf(f,
                "%d,%.6f,%d,%.6f,%.6f,%.6f,%.6f,"
                "%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                fi, t, q->id, q->p.x, q->p.y, q->p.vx, q->p.vy,
                dvx_g, dvy_g, dvx_d, dvy_d, dvx_s, dvy_s);
            rows++;
        }
    }

    fclose(f);
    fprintf(stderr, "record: %s  frames=%d particles=%d rows=%lld max_dv_verify_err=%.3g\n",
            out_path, frames, next_id, rows, max_verify_err);
    if (max_verify_err > 1e-5) {
        fprintf(stderr, "record: FATAL contribution decomposition diverges from field step\n");
        return 4;
    }
    return 0;
}
