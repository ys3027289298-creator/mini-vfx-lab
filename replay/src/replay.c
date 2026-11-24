
/* replay: 粒子特效的录制 / 存档 / 回放。
 *
 * 模拟由 emitter（速率 + 爆发发射、寿命、容量丢弃）与 field（重力、阻力、
 * 涡流）组合而成，所有随机性来自一个显式状态的 xorshift32 RNG。
 *
 * 存档格式两个版本：
 *   v1（缺陷版）：检查点只存 rate/计数器和存活粒子，漏存 RNG 种子与状态、
 *                 发射器累积量 rate_acc、当前力场参数、粒子池空洞（死槽位）。
 *   v2（修复版）：检查点补齐上述全部状态，另存池容量 n 与发射/丢弃计数。
 *
 * 子命令：
 *   replay record <v1|v2> <save> <framelog>
 *   replay play   <v1|v2> <save> <framelog> [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define MAXP 64
#define FRAMES 90
#define DT 0.016666667f            /* 60 fps 固定步长 */
#define RATE 10.0f                 /* 恒定速率发射（每秒粒子数） */
#define CP_FRAME 20                /* 检查点落在第 20 帧之后 */
#define MAGIC "VFXSAVE"

typedef struct {
    float x, y, vx, vy;
    float age, life;
    int alive;
} P;

typedef struct {
    P p[MAXP];
    int n;                       /* 已使用槽位数（含死槽位，空洞不压缩） */
    float rate_acc;              /* 速率发射的分数累积量 */
    int emitted, dropped;
} Emitter;

typedef struct {
    float gx, gy, drag, swirl;
} Field;

typedef struct {
    Emitter em;
    Field field;
    uint32_t rng;                /* RNG 完整状态，种子即初始状态 */
    int frame;
} Sim;

typedef enum { EV_FIELD = 1, EV_BURST = 2 } EvType;
typedef struct {
    int frame;
    EvType type;
    float a, b, c, d;            /* FIELD: gx,gy,drag,swirl；BURST: count */
} Event;

/* ---------------- RNG：显式状态的 xorshift32，结果完全由状态决定 ---------------- */

static uint32_t rng_next(Sim* s) {
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

static float rng_float(Sim* s, float lo, float hi) {
    return lo + (hi - lo) * (rng_next(s) / 4294967296.0f);
}

/* ---------------- 模拟 ---------------- */

static void sim_init(Sim* s, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->rng = seed ? seed : 1u;
    s->field = (Field){ 0.0f, -9.8f, 0.5f, 3.0f };
}

static int em_spawn(Sim* s, float life) {
    Emitter* e = &s->em;
    P np;
    /* 初速与寿命抖动全部走 RNG：漏存种子时回放结果从这里开始分叉 */
    np.x = 0.0f;
    np.y = 0.0f;
    np.vx = rng_float(s, -2.0f, 2.0f);
    np.vy = rng_float(s, 4.0f, 8.0f);
    np.age = 0.0f;
    np.life = life * rng_float(s, 0.8f, 1.2f);
    np.alive = 1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) { e->p[i] = np; e->emitted++; return i; }
    }
    if (e->n >= MAXP) { e->dropped++; return -1; }
    e->p[e->n] = np;
    e->emitted++;
    return e->n++;
}

/* 力场积分，语义与 field 模块一致：指数阻力 -> 涡流 -> 重力 -> 位移 */
static void field_step(P* p, float dt, const Field* f) {
    float k = expf(-f->drag * dt);
    p->vx *= k;
    p->vy *= k;
    float r = sqrtf(p->x * p->x + p->y * p->y);
    if (r > 1e-6f && f->swirl != 0.0f) {
        float tx = -p->y / r, ty = p->x / r;
        p->vx += tx * f->swirl * dt;
        p->vy += ty * f->swirl * dt;
    }
    p->vx += f->gx * dt;
    p->vy += f->gy * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
}

/* 推进一帧：先老化/积分已有粒子，再处理本帧事件（改力场、爆发），最后速率发射 */
static void sim_step(Sim* s, const Event* ev, int nev) {
    Emitter* e = &s->em;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        e->p[i].age += DT;
        if (e->p[i].age >= e->p[i].life) { e->p[i].alive = 0; continue; }
        field_step(&e->p[i], DT, &s->field);
    }

    for (int i = 0; i < nev; i++) {
        if (ev[i].frame != s->frame) continue;
        if (ev[i].type == EV_FIELD) {
            s->field = (Field){ ev[i].a, ev[i].b, ev[i].c, ev[i].d };
        } else if (ev[i].type == EV_BURST) {
            int count = (int)ev[i].a;
            for (int b = 0; b < count; b++) em_spawn(s, 0.9f);
        }
    }

    e->rate_acc += RATE * DT;
    int want = (int)e->rate_acc;
    e->rate_acc -= (float)want;
    for (int i = 0; i < want; i++) em_spawn(s, 0.9f);

    s->frame++;
}

/* 录制场景的脚本化事件：力场在第 10、40 帧切换，爆发在第 0、60 帧 */
static int build_events(Event* ev) {
    int n = 0;
    ev[n++] = (Event){ 10, EV_FIELD, 0.0f, -4.0f, 1.5f, 6.0f };
    ev[n++] = (Event){ 40, EV_FIELD, 2.0f, -9.8f, 0.5f, 3.0f };
    ev[n++] = (Event){ 0,  EV_BURST, 8.0f, 0, 0, 0 };
    ev[n++] = (Event){ 60, EV_BURST, 12.0f, 0, 0, 0 };
    return n;
}

/* ---------------- 帧日志：全量池子逐槽位输出，死槽位也保留 ---------------- */

static void log_frame(FILE* fp, const Sim* s) {
    fprintf(fp, "frame %d n %d emitted %d dropped %d rate_acc %.9g field %.9g %.9g %.9g %.9g rng %u\n",
            s->frame, s->em.n, s->em.emitted, s->em.dropped, s->em.rate_acc,
            s->field.gx, s->field.gy, s->field.drag, s->field.swirl, s->rng);
    for (int i = 0; i < s->em.n; i++) {
        const P* p = &s->em.p[i];
        fprintf(fp, "  slot %d alive %d x %.9g y %.9g vx %.9g vy %.9g age %.9g life %.9g\n",
                i, p->alive, p->x, p->y, p->vx, p->vy, p->age, p->life);
    }
}

/* ---------------- 存档 ---------------- */

static void save_events(FILE* fp, const Event* ev, int nev) {
    fprintf(fp, "events %d\n", nev);
    for (int i = 0; i < nev; i++) {
        fprintf(fp, "ev %d %d %.9g %.9g %.9g %.9g\n",
                ev[i].frame, (int)ev[i].type, ev[i].a, ev[i].b, ev[i].c, ev[i].d);
    }
}

static int save_v1(const char* path, const Sim* s, const Event* ev, int nev) {
    /* 缺陷版：只存 n/计数器和存活粒子。
     * 漏存：rng（种子/状态）、rate_acc、field、池中的死槽位（空洞）。 */
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s v1\n", MAGIC);
    fprintf(fp, "cp %d\n", s->frame);
    fprintf(fp, "meta %d %d %d\n", s->em.n, s->em.emitted, s->em.dropped);
    int alive = 0;
    for (int i = 0; i < s->em.n; i++) if (s->em.p[i].alive) alive++;
    fprintf(fp, "alive %d\n", alive);
    for (int i = 0; i < s->em.n; i++) {
        const P* p = &s->em.p[i];
        if (!p->alive) continue;   /* 空洞被丢弃，回放时无法复原槽位布局 */
        fprintf(fp, "p %.9g %.9g %.9g %.9g %.9g %.9g\n",
                p->x, p->y, p->vx, p->vy, p->age, p->life);
    }
    save_events(fp, ev, nev);
    fclose(fp);
    return 0;
}

static int save_v2(const char* path, const Sim* s, const Event* ev, int nev) {
    /* 修复版：补齐所有影响后续结果的状态。 */
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s v2\n", MAGIC);
    fprintf(fp, "cp %d\n", s->frame);
    fprintf(fp, "rng %u\n", s->rng);                              /* 种子/RNG 状态 */
    fprintf(fp, "emitter %d %d %d %.9g\n",
            s->em.n, s->em.emitted, s->em.dropped, s->em.rate_acc); /* 含累积量 */
    fprintf(fp, "field %.9g %.9g %.9g %.9g\n",
            s->field.gx, s->field.gy, s->field.drag, s->field.swirl); /* 力场参数 */
    for (int i = 0; i < s->em.n; i++) {                           /* 池子全量含空洞 */
        const P* p = &s->em.p[i];
        fprintf(fp, "slot %d %d %.9g %.9g %.9g %.9g %.9g %.9g\n",
                i, p->alive, p->x, p->y, p->vx, p->vy, p->age, p->life);
    }
    save_events(fp, ev, nev);
    fclose(fp);
    return 0;
}

/* 读一行并要求前缀匹配；匹配成功返回 1，文件结束返回 0，格式错误返回 -1 */
static int read_line(FILE* fp, char* buf, size_t cap) {
    if (!fgets(buf, (int)cap, fp)) return 0;
    return 1;
}

static int load(const char* path, Sim* s, Event* ev, int* nev, int* version,
                uint32_t replay_seed, int has_seed) {
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    char buf[512];
    int ver = 0, cp = 0, got_meta = 0, got_emitter = 0;
    *nev = 0;
    memset(s, 0, sizeof(*s));

    while (read_line(fp, buf, sizeof(buf)) == 1) {
        char tag[32];
        if (sscanf(buf, "%31s", tag) != 1) continue;

        if (strcmp(tag, MAGIC) == 0) {
            char vstr[8];
            if (sscanf(buf, "%*s %7s", vstr) != 1) { fclose(fp); return -2; }
            if (strcmp(vstr, "v1") == 0) ver = 1;
            else if (strcmp(vstr, "v2") == 0) ver = 2;
            else { fclose(fp); return -2; }
        } else if (strcmp(tag, "cp") == 0) {
            if (sscanf(buf, "cp %d", &cp) != 1) { fclose(fp); return -2; }
        } else if (strcmp(tag, "meta") == 0) {
            int n, emitted, dropped;
            if (sscanf(buf, "meta %d %d %d", &n, &emitted, &dropped) != 3) { fclose(fp); return -2; }
            s->em.n = n; s->em.emitted = emitted; s->em.dropped = dropped;
            got_meta = 1;
        } else if (strcmp(tag, "alive") == 0) {
            int alive;
            if (sscanf(buf, "alive %d", &alive) != 1) { fclose(fp); return -2; }
            /* v1：存活粒子紧凑追加到池子头部，死槽位布局永久丢失 */
            for (int i = 0; i < alive; i++) {
                if (read_line(fp, buf, sizeof(buf)) != 1) { fclose(fp); return -2; }
                P p = { 0 };
                if (sscanf(buf, "p %f %f %f %f %f %f",
                           &p.x, &p.y, &p.vx, &p.vy, &p.age, &p.life) != 6) {
                    fclose(fp); return -2;
                }
                p.alive = 1;
                s->em.p[i] = p;
            }
            s->em.n = alive;
        } else if (strcmp(tag, "rng") == 0) {
            if (sscanf(buf, "rng %u", &s->rng) != 1) { fclose(fp); return -2; }
        } else if (strcmp(tag, "emitter") == 0) {
            if (sscanf(buf, "emitter %d %d %d %f",
                       &s->em.n, &s->em.emitted, &s->em.dropped, &s->em.rate_acc) != 4) {
                fclose(fp); return -2;
            }
            got_emitter = 1;
        } else if (strcmp(tag, "field") == 0) {
            if (sscanf(buf, "field %f %f %f %f",
                       &s->field.gx, &s->field.gy, &s->field.drag, &s->field.swirl) != 4) {
                fclose(fp); return -2;
            }
        } else if (strcmp(tag, "slot") == 0) {
            int i, alive;
            P p = { 0 };
            if (sscanf(buf, "slot %d %d %f %f %f %f %f %f",
                       &i, &alive, &p.x, &p.y, &p.vx, &p.vy, &p.age, &p.life) != 8) {
                fclose(fp); return -2;
            }
            if (i < 0 || i >= MAXP) { fclose(fp); return -2; }
            p.alive = alive;
            s->em.p[i] = p;
        } else if (strcmp(tag, "events") == 0) {
            if (sscanf(buf, "events %d", nev) != 1 || *nev < 0 || *nev > 1024) {
                fclose(fp); return -2;
            }
            for (int i = 0; i < *nev; i++) {
                if (read_line(fp, buf, sizeof(buf)) != 1) { fclose(fp); return -2; }
                int f, t;
                Event e = { 0 };
                if (sscanf(buf, "ev %d %d %f %f %f %f",
                           &f, &t, &e.a, &e.b, &e.c, &e.d) != 6) {
                    fclose(fp); return -2;
                }
                e.frame = f; e.type = (EvType)t;
                ev[i] = e;
            }
        }
    }
    fclose(fp);

    if (!ver || (!got_meta && !got_emitter)) return -2;
    s->frame = cp;

    if (ver == 1) {
        /* 漏存状态的回填默认值：
         * rate_acc/field 被 memset 清零；池子空洞已无法恢复。
         * RNG 没有存档：命令行给了种子就用，否则用时间/地址熵现取，
         * 因此两次回放拿到不同 RNG 流。 */
        s->em.rate_acc = 0.0f;
        s->field = (Field){ 0.0f, 0.0f, 0.0f, 0.0f };
        if (has_seed) {
            s->rng = replay_seed ? replay_seed : 1u;
        } else {
            uint32_t ent = (uint32_t)(time(NULL))
                         ^ (uint32_t)(uintptr_t)&s
                         ^ (uint32_t)getpid();
            s->rng = ent ? ent : 1u;
        }
    } else if (s->rng == 0u) {
        s->rng = 1u;   /* xorshift 的 0 是不动点，防御性处理 */
    }

    *version = ver;
    return 0;
}

/* 丢弃检查点之前（含检查点帧）的事件，回放只处理未来帧 */
static int filter_events(Event* ev, int nev, int cp) {
    int w = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].frame > cp) ev[w++] = ev[i];
    }
    return w;
}

/* ---------------- 子命令 ---------------- */

static int run_record(int ver, const char* savepath, const char* logpath) {
    Sim s;
    sim_init(&s, 0x12345678u);
    Event ev[16];
    int nev = build_events(ev);

    FILE* log = fopen(logpath, "w");
    if (!log) { perror("framelog"); return 1; }

    for (int f = 0; f < FRAMES; f++) {
        sim_step(&s, ev, nev);
        log_frame(log, &s);
        if (f == CP_FRAME) {
            if (ver == 1) {
                if (save_v1(savepath, &s, ev, nev) != 0) { perror("save"); return 1; }
            } else {
                if (save_v2(savepath, &s, ev, nev) != 0) { perror("save"); return 1; }
            }
        }
    }
    fclose(log);
    printf("recorded %d frames, checkpoint saved at frame %d, save v%d -> %s\n",
           FRAMES, s.frame, ver, savepath);
    return 0;
}

/* 受控诊断：检查点前制造池空洞，对比 v1/v2 还原出的槽位布局。
 * 第 0 帧爆发 4 个粒子，其中槽位 1 给短寿命，在检查点前死亡留下死槽位。 */
static int run_hole_test(const char* dir) {
    char path[512];
    Sim live;
    sim_init(&live, 0xABCDEF01u);
    Event noev[1] = { 0 };

    /* 手动在槽位 0..3 放置粒子，槽位 1 寿命只够活 5 帧 */
    for (int i = 0; i < 4; i++) {
        P np = { (float)i, 0.0f, 0.0f, 1.0f, 0.0f, i == 1 ? 0.05f : 5.0f, 1 };
        live.em.p[i] = np;
    }
    live.em.n = 4;
    live.em.emitted = 4;

    /* 手动老化 10 个步长（不走速率发射，避免新粒子补位干扰）：
     * 槽位 1 的 age 越过 life 后被标记死亡，在池中留下空洞；0/2/3 仍活。 */
    for (int f = 0; f < 10; f++) {
        for (int i = 0; i < live.em.n; i++) {
            P* p = &live.em.p[i];
            if (!p->alive) continue;
            p->age += DT;
            if (p->age >= p->life) { p->alive = 0; continue; }
            field_step(p, DT, &live.field);
        }
        live.frame++;
    }

    snprintf(path, sizeof(path), "%s/hole_v1.save", dir);
    save_v1(path, &live, noev, 0);
    snprintf(path, sizeof(path), "%s/hole_v2.save", dir);
    save_v2(path, &live, noev, 0);

    Sim r1, r2;
    Event ev1[1], ev2[1];
    int n1, n2, v1, v2;
    snprintf(path, sizeof(path), "%s/hole_v1.save", dir);
    if (load(path, &r1, ev1, &n1, &v1, 999u, 1) != 0) { fprintf(stderr, "v1 load fail\n"); return 1; }
    snprintf(path, sizeof(path), "%s/hole_v2.save", dir);
    if (load(path, &r2, ev2, &n2, &v2, 999u, 1) != 0) { fprintf(stderr, "v2 load fail\n"); return 1; }

    printf("live pool at checkpoint: n=%d\n", live.em.n);
    for (int i = 0; i < live.em.n; i++)
        printf("  live slot %d alive=%d x=%.3g\n", i, live.em.p[i].alive, live.em.p[i].x);

    printf("\nv1 restore (compacted, holes dropped): n=%d\n", r1.em.n);
    for (int i = 0; i < r1.em.n; i++)
        printf("  v1 slot %d alive=%d x=%.3g\n", i, r1.em.p[i].alive, r1.em.p[i].x);

    printf("\nv2 restore (full pool with holes): n=%d\n", r2.em.n);
    for (int i = 0; i < r2.em.n; i++)
        printf("  v2 slot %d alive=%d x=%.3g\n", i, r2.em.p[i].alive, r2.em.p[i].x);

    /* 判定：v2 必须逐槽位还原（含死槽位），v1 必然破坏布局 */
    int v2_ok = (r2.em.n == live.em.n);
    if (v2_ok) {
        for (int i = 0; i < live.em.n; i++) {
            if (r2.em.p[i].alive != live.em.p[i].alive ||
                r2.em.p[i].x != live.em.p[i].x) { v2_ok = 0; break; }
        }
    }
    int v1_broken = (r1.em.n != live.em.n) ||
                    (r1.em.p[1].alive == 1 && r1.em.p[1].x == live.em.p[1].x);
    printf("\nhole_layout: v1_broken=%d v2_exact=%d\n", v1_broken, v2_ok);
    return (v2_ok && v1_broken) ? 0 : 1;
}

static int run_play(const char* savepath, const char* logpath,
                    uint32_t seed, int has_seed) {
    Sim s;
    Event ev[16];
    int nev, ver;
    int rc = load(savepath, &s, ev, &nev, &ver, seed, has_seed);
    if (rc != 0) { fprintf(stderr, "load failed: %d\n", rc); return 1; }
    nev = filter_events(ev, nev, s.frame);

    FILE* log = fopen(logpath, "w");
    if (!log) { perror("framelog"); return 1; }

    while (s.frame < FRAMES) {
        sim_step(&s, ev, nev);
        log_frame(log, &s);
    }
    fclose(log);
    printf("replayed v%d save from checkpoint frame %d, rng at start %u -> %s\n",
           ver, s.frame, s.rng, logpath);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "hole-test") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s hole-test <workdir>\n", argv[0]); return 2; }
        return run_hole_test(argv[2]);
    }
    if (argc < 5) {
        fprintf(stderr,
                "usage:\n"
                "  %s record <v1|v2> <save> <framelog>\n"
                "  %s play   <v1|v2> <save> <framelog> [seed]\n"
                "  %s hole-test <workdir>\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    int ver;
    if (strcmp(argv[2], "v1") == 0) ver = 1;
    else if (strcmp(argv[2], "v2") == 0) ver = 2;
    else { fprintf(stderr, "version must be v1 or v2\n"); return 2; }

    if (strcmp(argv[1], "record") == 0) {
        if (argc != 5) { fprintf(stderr, "record takes 3 args\n"); return 2; }
        return run_record(ver, argv[3], argv[4]);
    }
    if (strcmp(argv[1], "play") == 0) {
        uint32_t seed = 0;
        int has_seed = 0;
        if (argc >= 6) { seed = (uint32_t)strtoul(argv[5], NULL, 0); has_seed = 1; }
        return run_play(argv[3], argv[4], seed, has_seed);
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
