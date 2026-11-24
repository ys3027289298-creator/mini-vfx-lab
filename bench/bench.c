/*
 * bench.c — 10 组爆炸（~2 万存活粒子）单帧分阶段计时 + GL 提交对照。
 *
 * 帧管线拆成 6 个可计时区间：
 *   1 emit       发射 / 寿命 / 全量清点      （emitter.c 原算法）
 *   2 field      阻力 / 涡流 / 重力积分       （field.c 原算法）
 *   3 trail      ribbon 轨迹点维护            （trail.c 原算法）
 *   4 cpu_pack   应用侧把存活粒子装配成顶点流
 *   5 upload     glBufferData + 顶点属性装配
 *   6 draw       DrawArrays 循环，末尾 glFinish 计入驱动/光栅
 *
 * submit 两版唯一差异在第 6 段：
 *   naive    : 每粒子一次 draw call（每粒子 uniform + DrawArrays）
 *   batched  : 全部粒子一次 draw call（颜色走顶点属性）
 * 1~5 段两版逐字节相同，保证对照只测提交方式。
 *
 * 另跑一条粒子数扩展性曲线（2.5k~20k），区分"粒子物理成本"与
 * "draw call 成本"各自随数量的斜率。
 */
#define _POSIX_C_SOURCE 199309L
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "particles.h"

#define WARMUP     30
#define ROUNDS     4
#define FRAMES     60
#define DT         (1.0f / 60.0f)
#define VERT_STRIDE (4 * sizeof(float))

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

/* ---------------- EGL 离屏上下文 ---------------- */
static EGLDisplay g_dpy;
static EGLSurface g_surf;
static EGLContext g_ctx;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(s, 512, NULL, b); fprintf(stderr, "shader: %s\n", b); exit(1); }
    return s;
}

static const char* VS_NAIVE =
    "attribute vec2 a_pos; uniform vec2 u_offset; uniform float u_scale;\n"
    "void main(){ gl_Position = vec4(a_pos * u_scale + u_offset, 0.0, 1.0); gl_PointSize = 4.0; }\n";
static const char* VS_BATCH =
    "attribute vec2 a_pos; attribute vec2 a_color; varying vec2 v_color;\n"
    "void main(){ gl_Position = vec4(a_pos * 0.3, 0.0, 1.0); gl_PointSize = 4.0; v_color = a_color; }\n";
static const char* FS_NAIVE =
    "precision mediump float; uniform vec4 u_color; void main(){ gl_FragColor = u_color; }\n";
static const char* FS_BATCH =
    "precision mediump float; varying vec2 v_color; void main(){ gl_FragColor = vec4(v_color, 0.2, 1.0); }\n";

static GLuint prog_naive, prog_batch;
static GLint  na_pos, na_off, na_scale, na_color;
static GLint  ba_pos, ba_color;
static GLuint vbo;
static float* vbuf;

static void gl_setup(void) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma, mi; eglInitialize(g_dpy, &ma, &mi);
    EGLConfig cfg; EGLint n;
    EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                       EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                       EGL_BLUE_SIZE, 8, EGL_NONE };
    eglChooseConfig(g_dpy, cattr, &cfg, 1, &n);
    EGLint sattr[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, sattr);
    EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, xattr);
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);

    GLuint vs = compile(GL_VERTEX_SHADER, VS_NAIVE), fs = compile(GL_FRAGMENT_SHADER, FS_NAIVE);
    prog_naive = glCreateProgram();
    glAttachShader(prog_naive, vs); glAttachShader(prog_naive, fs); glLinkProgram(prog_naive);
    glDeleteShader(vs); glDeleteShader(fs);
    na_pos = glGetAttribLocation(prog_naive, "a_pos");
    na_off = glGetUniformLocation(prog_naive, "u_offset");
    na_scale = glGetUniformLocation(prog_naive, "u_scale");
    na_color = glGetUniformLocation(prog_naive, "u_color");

    vs = compile(GL_VERTEX_SHADER, VS_BATCH); fs = compile(GL_FRAGMENT_SHADER, FS_BATCH);
    prog_batch = glCreateProgram();
    glAttachShader(prog_batch, vs); glAttachShader(prog_batch, fs); glLinkProgram(prog_batch);
    glDeleteShader(vs); glDeleteShader(fs);
    ba_pos = glGetAttribLocation(prog_batch, "a_pos");
    ba_color = glGetAttribLocation(prog_batch, "a_color");

    glGenBuffers(1, &vbo);
    vbuf = malloc((size_t)N_EMITTERS * MAXP * VERT_STRIDE);

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* 第 4 段：应用侧顶点装配，两版共用。 */
static int pack_vertices(Emitter es[N_EMITTERS]) {
    int cnt = 0;
    for (int g = 0; g < N_EMITTERS; g++) {
        Emitter* e = &es[g];
        for (int i = 0; i < e->n; i++) {
            if (!e->p[i].alive) continue;
            float shade = 1.0f - e->p[i].age / e->p[i].life;
            float* v = &vbuf[cnt * 4];
            v[0] = e->p[i].x; v[1] = e->p[i].y;
            v[2] = shade; v[3] = 0.3f + 0.5f * shade;
            cnt++;
        }
    }
    return cnt;
}

/* 计时累加器：6 个区间 + 整帧 */
typedef struct {
    double emit, field, trail, cpu_pack, upload, draw, total;
} StageTimes;

static void upload_naive(int cnt) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cnt * VERT_STRIDE, vbuf, GL_DYNAMIC_DRAW);
    glUseProgram(prog_naive);
    glEnableVertexAttribArray(na_pos);
    glVertexAttribPointer(na_pos, 2, GL_FLOAT, GL_FALSE, VERT_STRIDE, 0);
}

static void upload_batched(int cnt) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cnt * VERT_STRIDE, vbuf, GL_DYNAMIC_DRAW);
    glUseProgram(prog_batch);
    glEnableVertexAttribArray(ba_pos);
    glVertexAttribPointer(ba_pos, 2, GL_FLOAT, GL_FALSE, VERT_STRIDE, (void*)0);
    glEnableVertexAttribArray(ba_color);
    glVertexAttribPointer(ba_color, 2, GL_FLOAT, GL_FALSE, VERT_STRIDE, (void*)(2 * sizeof(float)));
}

static void draw_naive(int cnt) {
    for (int i = 0; i < cnt; i++) {
        const float* v = &vbuf[i * 4];
        glUniform2f(na_off, 0.0f, 0.0f);
        glUniform1f(na_scale, 0.3f);
        glUniform4f(na_color, v[2], v[3], 0.2f, 1.0f);
        glDrawArrays(GL_POINTS, i, 1);
    }
    glFinish();
}

static void draw_batched(int cnt) {
    glDrawArrays(GL_POINTS, 0, cnt);
    glFinish();
}

static void run_frames(Emitter es[N_EMITTERS], int batched, int frames, StageTimes* acc,
                       int* alive_out) {
    int alive = 0;
    for (int f = 0; f < frames; f++) {
        double t0 = now_ms();
        alive = stage_emit(es, DT);
        double t1 = now_ms();
        stage_field(es, DT);
        double t2 = now_ms();
        stage_trail(es, DT);
        double t3 = now_ms();
        int cnt = pack_vertices(es);
        double t4 = now_ms();
        if (batched) upload_batched(cnt); else upload_naive(cnt);
        double t5 = now_ms();
        if (batched) draw_batched(cnt); else draw_naive(cnt);
        double t6 = now_ms();
        glClear(GL_COLOR_BUFFER_BIT);

        acc->emit     += t1 - t0;
        acc->field    += t2 - t1;
        acc->trail    += t3 - t2;
        acc->cpu_pack += t4 - t3;
        acc->upload   += t5 - t4;
        acc->draw     += t6 - t5;
        acc->total    += t6 - t0;
    }
    *alive_out = alive;
}

static void report(const char* tag, StageTimes* s, int frames, int alive) {
    double tot = s->total / frames;
    double sim = (s->emit + s->field + s->trail) / frames;
    double sub = (s->cpu_pack + s->upload + s->draw) / frames;
    printf("[%s] alive=%d\n", tag, alive);
    printf("  frame %6.3f ms  fps %6.1f\n", tot, 1000.0 / tot);
    printf("  -- 模拟（CPU）--\n");
    printf("    emit     %6.3f ms  %5.1f%%\n", s->emit / frames,     100.0 * s->emit / s->total);
    printf("    field    %6.3f ms  %5.1f%%\n", s->field / frames,    100.0 * s->field / s->total);
    printf("    trail    %6.3f ms  %5.1f%%\n", s->trail / frames,    100.0 * s->trail / s->total);
    printf("  -- 提交（GL）--\n");
    printf("    cpu_pack %6.3f ms  %5.1f%%\n", s->cpu_pack / frames, 100.0 * s->cpu_pack / s->total);
    printf("    upload   %6.3f ms  %5.1f%%\n", s->upload / frames,   100.0 * s->upload / s->total);
    printf("    draw     %6.3f ms  %5.1f%%\n", s->draw / frames,     100.0 * s->draw / s->total);
    printf("  小计: 模拟 %.3f ms (%.1f%%)  提交 %.3f ms (%.1f%%)\n\n",
           sim, 100.0 * sim / tot, sub, 100.0 * sub / tot);
}

/* 扩展性曲线：稳态顶点流上只改变提交的粒子数，测 draw 段斜率。 */
static void scaling_sweep(void) {
    printf("== draw 段随粒子数的扩展性（稳态场景，每点 30 帧取均值）==\n");
    printf("%8s | %12s %12s | %10s\n", "particles", "naive draw", "batch draw", "naive/us");
    const int counts[] = { 2500, 5000, 10000, 20000 };
    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++) {
        int cnt = counts[k];
        double tn = 0, tb = 0;
        int reps = 30;
        for (int r = 0; r < reps; r++) {
            upload_naive(cnt);
            double a = now_ms(); draw_naive(cnt); tn += now_ms() - a;
            upload_batched(cnt);
            double b = now_ms(); draw_batched(cnt); tb += now_ms() - b;
            glClear(GL_COLOR_BUFFER_BIT);
        }
        printf("%8d | %9.3f ms %9.3f ms | %7.3f us\n",
               cnt, tn / reps, tb / reps, 1000.0 * tn / reps / cnt);
    }
    printf("\n");
}

/*
 * 关键隔离实验：总顶点数固定 2 万（光栅工作量不变），
 * 只改变每次 DrawArrays 携带的顶点数，即改变 draw call 数量。
 * 时间对调用数的斜率 = 单次调用固定开销；截距 = 纯光栅地板。
 * 用 naive 的 program（uniform 每调用更新一次），B=1 即原始写法。
 */
static void batchsize_sweep(Emitter es[N_EMITTERS]) {
    int cnt = pack_vertices(es);
    upload_naive(cnt);
    printf("== 固定 %d 顶点，只变 draw call 数（30 帧取均值）==\n", cnt);
    printf("%10s %10s | %10s %10s\n", "batch", "draws", "frame ms", "us/call");
    const int batches[] = { 1, 10, 100, 1000, 5000, 20000 };
    double prev_t = 0; int prev_calls = 0;
    for (size_t k = 0; k < sizeof(batches) / sizeof(batches[0]); k++) {
        int B = batches[k];
        int calls = (cnt + B - 1) / B;
        double t = 0;
        int reps = 30;
        for (int r = 0; r < reps; r++) {
            double a = now_ms();
            for (int first = 0; first < cnt; first += B) {
                int n = B;
                if (first + n > cnt) n = cnt - first;
                glUniform2f(na_off, 0.0f, 0.0f);
                glUniform1f(na_scale, 0.3f);
                glUniform4f(na_color, 1.0f, 0.5f, 0.2f, 1.0f);
                glDrawArrays(GL_POINTS, first, n);
            }
            glFinish();
            t += now_ms() - a;
            glClear(GL_COLOR_BUFFER_BIT);
        }
        t /= reps;
        double slope = prev_calls ? (t - prev_t) / (calls - prev_calls) * 1000.0 : 0.0;
        printf("%10d %10d | %10.3f %10.3f\n", B, calls, t, slope);
        prev_t = t; prev_calls = calls;
    }
    printf("\n");
}

int main(void) {
    gl_setup();
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    printf("scenario : %d explosions, capacity %d particles, dt=%.4fs\n\n",
           N_EMITTERS, N_EMITTERS * MAXP, DT);

    /* 10 个 emitter 共 8.5MB，放静态区避免撑爆 8MB 栈 */
    static Emitter es[N_EMITTERS];
    sim_init(es);

    /* 预热：推进到稳态（粒子寿命 1s，约 60 帧填满） */
    for (int f = 0; f < WARMUP; f++) {
        stage_emit(es, DT);
        stage_field(es, DT);
        stage_trail(es, DT);
        int cnt = pack_vertices(es);
        upload_naive(cnt); draw_naive(cnt);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (glGetError() != 0) { fprintf(stderr, "GL error during warmup\n"); return 1; }

    /* 交替跑两版，消除热/缓存先后偏差 */
    StageTimes tn = {0}, tb = {0};
    int alive_n = 0, alive_b = 0;
    for (int r = 0; r < ROUNDS; r++) {
        run_frames(es, 0, FRAMES, &tn, &alive_n);
        run_frames(es, 1, FRAMES, &tb, &alive_b);
    }
    int total_frames = ROUNDS * FRAMES;
    report("naive : 每粒子一个 draw call", &tn, total_frames, alive_n);
    report("batch : 单次 draw call 合批", &tb, total_frames, alive_b);

    printf("整帧耗时: %.3f -> %.3f ms (%.2fx)，帧率 %.1f -> %.1f fps\n\n",
           tn.total / total_frames, tb.total / total_frames,
           tn.total / tb.total,
           1000.0 * total_frames / tn.total,
           1000.0 * total_frames / tb.total);

    batchsize_sweep(es);
    scaling_sweep();
    return 0;
}
