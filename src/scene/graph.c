/* artful — materials written in C. An expression is a number given by the calls that make it; the program's own
 * C functions compose them. What comes of that is a list of steps, each on four floats, kept by the scene:
 * the GPU's shaders are generated from it (shaders.c, emitGraph), and here it is run, for the CPU. */
#include "scene/sceneInternal.h"

#define TWO_PI 6.28318530718f

struct ArtfGraph {
    ArtfGraphNode nodes[ARTF_GRAPH_NODES];
    uint32_t count;
    ArtfExpr outputs[ARTF_GRAPH_OUTPUTS];
    bool full;
};

ArtfGraph* artfGraphNew(void) { return ARTF_NEW(ArtfGraph); }
void artfGraphFree(ArtfGraph* g) { artfFree(g); }

static ArtfExpr add(ArtfGraph* g, uint8_t op, ArtfExpr a, ArtfExpr b, ArtfExpr c) {
    if (g->count == ARTF_GRAPH_NODES) {
        if (!g->full) artfLog("artful: a material of more than %d steps", ARTF_GRAPH_NODES);
        g->full = true;
        return 1;
    }
    ArtfGraphNode* n = &g->nodes[g->count++];
    memset(n, 0, sizeof *n);
    n->op = op;
    n->a = (uint16_t)a;
    n->b = (uint16_t)b;
    n->c = (uint16_t)c;
    return g->count;
}

ArtfExpr artfExprConst(ArtfGraph* g, float x, float y, float z, float w) {
    const ArtfExpr e = add(g, ARTF_OP_CONST, 0, 0, 0);
    if (!g->full) {
        const float v[4] = {x, y, z, w};
        memcpy(g->nodes[e - 1].v, v, sizeof v);
    }
    return e;
}
ArtfExpr artfExprNumber(ArtfGraph* g, float v) { return artfExprConst(g, v, v, v, v); }
ArtfExpr artfExprUv(ArtfGraph* g) { return add(g, ARTF_OP_UV, 0, 0, 0); }
ArtfExpr artfExprPosition(ArtfGraph* g) { return add(g, ARTF_OP_POSITION, 0, 0, 0); }
ArtfExpr artfExprNormal(ArtfGraph* g) { return add(g, ARTF_OP_NORMAL, 0, 0, 0); }
ArtfExpr artfExprToEye(ArtfGraph* g) { return add(g, ARTF_OP_TO_EYE, 0, 0, 0); }
ArtfExpr artfExprBaseColor(ArtfGraph* g) { return add(g, ARTF_OP_BASE, 0, 0, 0); }
ArtfExpr artfExprParam(ArtfGraph* g, uint32_t index) { return add(g, ARTF_OP_PARAM, index & 3, 0, 0); }
ArtfExpr artfExprTexture(ArtfGraph* g, ArtfTexture texture, ArtfExpr uv) { return add(g, ARTF_OP_TEXTURE, uv, texture, 0); }
ArtfExpr artfExprAdd(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_ADD, a, b, 0); }
ArtfExpr artfExprSub(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_SUB, a, b, 0); }
ArtfExpr artfExprMul(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_MUL, a, b, 0); }
ArtfExpr artfExprDiv(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_DIV, a, b, 0); }
ArtfExpr artfExprMin(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_MIN, a, b, 0); }
ArtfExpr artfExprMax(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_MAX, a, b, 0); }
ArtfExpr artfExprDot(ArtfGraph* g, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_DOT, a, b, 0); }
ArtfExpr artfExprFloor(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_FLOOR, a, 0, 0); }
ArtfExpr artfExprFract(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_FRACT, a, 0, 0); }
ArtfExpr artfExprAbs(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_ABS, a, 0, 0); }
ArtfExpr artfExprSqrt(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_SQRT, a, 0, 0); }
ArtfExpr artfExprSin(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_SIN, a, 0, 0); }
ArtfExpr artfExprCos(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_COS, a, 0, 0); }
ArtfExpr artfExprMix(ArtfGraph* g, ArtfExpr a, ArtfExpr b, ArtfExpr t) { return add(g, ARTF_OP_MIX, a, b, t); }
ArtfExpr artfExprIf(ArtfGraph* g, ArtfExpr condition, ArtfExpr a, ArtfExpr b) { return add(g, ARTF_OP_IF, condition, a, b); }
ArtfExpr artfExprX(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_X, a, 0, 0); }
ArtfExpr artfExprY(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_Y, a, 0, 0); }
ArtfExpr artfExprZ(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_Z, a, 0, 0); }
ArtfExpr artfExprW(ArtfGraph* g, ArtfExpr a) { return add(g, ARTF_OP_W, a, 0, 0); }
ArtfExpr artfExprVec(ArtfGraph* g, ArtfExpr x, ArtfExpr y, ArtfExpr z) { return add(g, ARTF_OP_VEC, x, y, z); }

void artfGraphSetOutput(ArtfGraph* g, ArtfGraphOutput output, ArtfExpr e) {
    if (output < ARTF_GRAPH_OUTPUTS) g->outputs[output] = e;
}

uint32_t artfSceneAddGraph(ArtfScene* s, const ArtfGraph* g) {
    ArtfGraphData d;
    memset(&d, 0, sizeof d);
    d.count = g->count;
    d.nodes = artfMalloc(((size_t)g->count + 1) * sizeof *d.nodes);
    memcpy(d.nodes, g->nodes, (size_t)g->count * sizeof *d.nodes);
    memcpy(d.outputs, g->outputs, sizeof d.outputs);
    artfVecPush(&s->graphs, d);
    return (uint32_t)s->graphs.len;
}

/* The same polynomial as the GPU's: no sine from a C library, whose bits differ from one to the next. */
static float sineOf(float x) {
    float t = x * (1.f / TWO_PI);
    t -= floorf(t + .5f);
    const float a = fabsf(t), folded = a > .25f ? .5f - a : a, u = folded * TWO_PI, u2 = u * u;
    const float p = u * (1.f + u2 * (-1.f / 6.f + u2 * (1.f / 120.f + u2 * (-1.f / 5040.f))));
    return t < 0 ? -p : p;
}

/* Which mipmap: what the coordinates a texture is read at do from one lane to the next, which are pixels side by side.
 * The least of those steps: one that wraps around says nothing. */
static int mipOf(const ArtfTextureData* t, const float* u, const float* v) {
    float least = 1e30f;
    for (int l = 0; l < 7; ++l) {
        const float du = (u[l + 1] - u[l]) * (float)t->width, dv = (v[l + 1] - v[l]) * (float)t->height;
        least = artfMinf(least, du * du + dv * dv);
    }
    uint32_t bits;
    memcpy(&bits, &least, 4);
    const int level = ((int)(bits >> 23) - 127 + 1) >> 1;
    return artfClampi(level, t->residentMip, t->mipCount - 1);
}

#define EACH(expression) for (int k = 0; k < 4; ++k) for (int l = 0; l < lanes; ++l) { const float x = a[k][l], y = b[k][l], z = c[k][l]; (void)x; (void)y; (void)z; o[k][l] = (expression); } break
#define SPREAD(component) for (int k = 0; k < 4; ++k) for (int l = 0; l < lanes; ++l) o[k][l] = a[component][l]; break

#ifdef _MSC_VER
#define ARTF_FORCE_INLINE static __forceinline
#else
#define ARTF_FORCE_INLINE static inline __attribute__((always_inline))
#endif

/* One step at a time for all lanes: what it costs to find out what to do is paid once. The same arithmetic for each
 * lane, so one lane alone gives what it would among eight. */
ARTF_FORCE_INLINE void runLanes(const ArtfScene* s, const ArtfGraphData* g, const ArtfMaterialData* m, ArtfGraphLanes* io, const int lanes) {
    float r[ARTF_GRAPH_NODES + 1][4][8];
    memset(r[0], 0, sizeof r[0]);
    for (uint32_t i = 0; i < g->count; ++i) {
        const ArtfGraphNode* n = &g->nodes[i];
        float (*o)[8] = r[i + 1];
        float (*a)[8] = r[n->a], (*b)[8] = r[n->b], (*c)[8] = r[n->c];
        switch (n->op) {
        case ARTF_OP_CONST: for (int k = 0; k < 4; ++k) for (int l = 0; l < lanes; ++l) o[k][l] = n->v[k]; break;
        case ARTF_OP_UV: for (int l = 0; l < lanes; ++l) { o[0][l] = io->u[l]; o[1][l] = io->v[l]; o[2][l] = 0; o[3][l] = 0; } break;
        case ARTF_OP_POSITION: for (int l = 0; l < lanes; ++l) { for (int k = 0; k < 3; ++k) o[k][l] = io->world[k][l]; o[3][l] = 1; } break;
        case ARTF_OP_NORMAL: for (int l = 0; l < lanes; ++l) { for (int k = 0; k < 3; ++k) o[k][l] = io->normalIn[k][l]; o[3][l] = 0; } break;
        case ARTF_OP_TO_EYE: for (int l = 0; l < lanes; ++l) { for (int k = 0; k < 3; ++k) o[k][l] = io->toEye[k][l]; o[3][l] = 0; } break;
        case ARTF_OP_BASE: for (int l = 0; l < lanes; ++l) { for (int k = 0; k < 3; ++k) o[k][l] = io->base[k][l]; o[3][l] = 1; } break;
        case ARTF_OP_PARAM: for (int k = 0; k < 4; ++k) for (int l = 0; l < lanes; ++l) o[k][l] = m->params[n->a][k]; break;
        case ARTF_OP_TEXTURE: {
            const ArtfTextureData* t = n->b && n->b < s->textures.len ? &s->textures.items[n->b] : NULL;
            const int level = !t ? 0 : lanes == 8 ? mipOf(t, a[0], a[1]) : t->residentMip;
            for (int l = 0; l < lanes; ++l) {
                float texel[4] = {1, 1, 1, 1};
                if (t) artfTexelAtLevel(t, level, a[0][l], a[1][l], texel);
                /* (bytes hold the square root of light) */
                o[0][l] = texel[2] * texel[2]; o[1][l] = texel[1] * texel[1]; o[2][l] = texel[0] * texel[0]; o[3][l] = texel[3];
            }
            break;
        }
        case ARTF_OP_DOT: for (int l = 0; l < lanes; ++l) { const float d = a[0][l] * b[0][l] + a[1][l] * b[1][l] + a[2][l] * b[2][l]; for (int k = 0; k < 4; ++k) o[k][l] = d; } break;
        case ARTF_OP_X: SPREAD(0);
        case ARTF_OP_Y: SPREAD(1);
        case ARTF_OP_Z: SPREAD(2);
        case ARTF_OP_W: SPREAD(3);
        case ARTF_OP_VEC: for (int l = 0; l < lanes; ++l) { o[0][l] = a[0][l]; o[1][l] = b[0][l]; o[2][l] = c[0][l]; o[3][l] = 1; } break;
        case ARTF_OP_ADD: EACH(x + y);
        case ARTF_OP_SUB: EACH(x - y);
        case ARTF_OP_MUL: EACH(x * y);
        case ARTF_OP_DIV: EACH(x / y);
        case ARTF_OP_MIN: EACH(y < x ? y : x);
        case ARTF_OP_MAX: EACH(x < y ? y : x);
        case ARTF_OP_FLOOR: EACH(floorf(x));
        case ARTF_OP_FRACT: EACH(x - floorf(x));
        case ARTF_OP_ABS: EACH(fabsf(x));
        case ARTF_OP_SQRT: EACH(sqrtf(artfMaxf(x, 0)));
        case ARTF_OP_SIN: EACH(sineOf(x));
        case ARTF_OP_COS: EACH(sineOf(x + 1.57079632679f));
        case ARTF_OP_MIX: EACH(x + (y - x) * z);
        case ARTF_OP_IF: EACH(x > 0 ? y : z);
        default: for (int k = 0; k < 4; ++k) for (int l = 0; l < lanes; ++l) o[k][l] = 0; break;
        }
    }
#define OUTPUT(which) (g->outputs[which] ? r[g->outputs[which]] : NULL)
    float (*color)[8] = OUTPUT(ARTF_OUT_COLOR), (*emissive)[8] = OUTPUT(ARTF_OUT_EMISSIVE), (*rough)[8] = OUTPUT(ARTF_OUT_ROUGHNESS);
    float (*metal)[8] = OUTPUT(ARTF_OUT_METALLIC), (*alpha)[8] = OUTPUT(ARTF_OUT_ALPHA), (*normal)[8] = OUTPUT(ARTF_OUT_NORMAL);
#undef OUTPUT
    for (int l = 0; l < lanes; ++l) {
        for (int k = 0; k < 3; ++k) {
            if (color) io->color[k][l] = color[k][l];
            if (emissive) io->emissive[k][l] = emissive[k][l];
        }
        if (rough) io->roughness[l] = artfClampf(rough[0][l], .045f, 1.f);
        if (metal) io->metallic[l] = artfClampf(metal[0][l], 0, 1);
        if (alpha) io->alpha[l] = alpha[0][l];
        if (normal) {
            const float length = sqrtf(normal[0][l] * normal[0][l] + normal[1][l] * normal[1][l] + normal[2][l] * normal[2][l]);
            if (length > 0)
                for (int k = 0; k < 3; ++k) io->normal[k][l] = normal[k][l] / length;
        }
    }
}

void artfGraphRunLanes(const ArtfScene* s, const ArtfGraphData* g, const ArtfMaterialData* m, ArtfGraphLanes* io) { runLanes(s, g, m, io, 8); }

void artfGraphRun(const ArtfScene* s, const ArtfGraphData* g, const ArtfMaterialData* m, const ArtfGraphInput* in, ArtfGraphResult* out) {
    ArtfGraphLanes io;
    io.u[0] = in->u;
    io.v[0] = in->v;
    for (int k = 0; k < 3; ++k) {
        io.world[k][0] = in->world[k];
        io.normalIn[k][0] = in->normal[k];
        io.toEye[k][0] = in->toEye[k];
        io.base[k][0] = in->base[k];
        io.color[k][0] = out->color[k];
        io.emissive[k][0] = out->emissive[k];
        io.normal[k][0] = out->normal[k];
    }
    io.roughness[0] = out->roughness;
    io.metallic[0] = out->metallic;
    io.alpha[0] = out->alpha;
    runLanes(s, g, m, &io, 1);
    for (int k = 0; k < 3; ++k) {
        out->color[k] = io.color[k][0];
        out->emissive[k] = io.emissive[k][0];
        out->normal[k] = io.normal[k][0];
    }
    out->roughness = io.roughness[0];
    out->metallic = io.metallic[0];
    out->alpha = io.alpha[0];
}
