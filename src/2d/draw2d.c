/* artful-2d — a tile of the frame: each sprite over it in order, each of its pixels covered as an edge's distance (or
 * a polygon's spans) says, sampled, lit by the tile's lights and gi, and blended over what is below. A tile is one
 * thread's, and its pixels are made in the same order whatever the threads: the same bytes. */
#include "2d/internal2d.h"

/* ---- textures ---- */

/* x within 0..n-1, as a texture that repeats has it. */
static inline int wrap(int x, int n) {
    if ((unsigned)x < (unsigned)n) return x;
    x %= n;
    return x < 0 ? x + n : x;
}

/* A texel (x and y already within the level), as premultiplied light (a normal map's: as -1..1 and its alpha). */
static inline void texel(const Texture2D* t, int level, int x, int y, float out[4]) {
    const uint32_t p = t->texels[level][(size_t)y * t->width[level] + x];
    const float a = (float)(p >> 24) * (1.f / 255.f);
    if (t->normalMap) {
        out[0] = (float)(p >> 16 & 255) * (2.f / 255.f) - 1.f;
        out[1] = (float)(p >> 8 & 255) * (2.f / 255.f) - 1.f;
        out[2] = (float)(p & 255) * (2.f / 255.f) - 1.f;
    } else {
        out[0] = artf2DSquare[p >> 16 & 255] * a;
        out[1] = artf2DSquare[p >> 8 & 255] * a;
        out[2] = artf2DSquare[p & 255] * a;
    }
    out[3] = a;
}

/* Past the edges, a texture repeats. */
static void sampleTexture(const Texture2D* t, int level, bool nearest, float u, float v, float out[4]) {
    level = artfMini(level, t->levels - 1);
    const int w = t->width[level], h = t->height[level];
    /* (far from 0, a float has no fraction left: keep u and v near it) */
    u -= floorf(u);
    v -= floorf(v);
    const float x = u * (float)w, y = v * (float)h;
    if (nearest) {
        texel(t, level, wrap((int)x, w), wrap((int)y, h), out);
        return;
    }
    const float fx = x - .5f, fy = y - .5f;
    const float x0 = floorf(fx), y0 = floorf(fy), wx = fx - x0, wy = fy - y0;
    const int ix = wrap((int)x0, w), iy = wrap((int)y0, h);
    const int ix1 = ix + 1 < w ? ix + 1 : 0, iy1 = iy + 1 < h ? iy + 1 : 0;
    float a[4], b[4], c[4], d[4];
    texel(t, level, ix, iy, a);
    texel(t, level, ix1, iy, b);
    texel(t, level, ix, iy1, c);
    texel(t, level, ix1, iy1, d);
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * wx, bottom = c[k] + (d[k] - c[k]) * wx;
        out[k] = top + (bottom - top) * wy;
    }
}

void artf2DSample(const Item2D* it, float lx, float ly, float cover, float rgba[4], float emit[3], float normal[3]) {
    const ArtfSprite2DDesc* d = &it->sp->desc;
    const float u = it->uv[0] + lx * it->uv[1], v = it->uv[2] + ly * it->uv[3];
    float t[4] = {1, 1, 1, 1};
    if (it->tex[0]) sampleTexture(it->tex[0], it->mip[0], d->nearest, u, v, t);
    const float k = d->opacity * cover;
    rgba[0] = d->color.x * t[0] * k;
    rgba[1] = d->color.y * t[1] * k;
    rgba[2] = d->color.z * t[2] * k;
    rgba[3] = t[3] * k;
    if (emit) {
        if (it->emits) {
            float e[4] = {1, 1, 1, 1};
            if (it->tex[2]) sampleTexture(it->tex[2], it->mip[2], d->nearest, u, v, e);
            const float ke = k * t[3];
            emit[0] = d->emissive.x * e[0] * ke;
            emit[1] = d->emissive.y * e[1] * ke;
            emit[2] = d->emissive.z * e[2] * ke;
        } else emit[0] = emit[1] = emit[2] = 0;
    }
    if (normal) {
        if (it->tex[1]) {
            float n[4];
            sampleTexture(it->tex[1], it->mip[1], d->nearest, u, v, n);
            /* (y up in the map, down in the frame; then turned as the sprite is) */
            const float nx = n[0], ny = -n[1], nz = artfMaxf(n[2], 0);
            const float len = sqrtf(nx * nx + ny * ny + nz * nz);
            const float s = len > 0 ? 1.f / len : 0;
            normal[0] = (it->turnCos * nx - it->turnSin * ny) * s;
            normal[1] = (it->turnSin * nx + it->turnCos * ny) * s;
            normal[2] = len > 0 ? nz * s : 1;
        } else {
            normal[0] = normal[1] = 0;
            normal[2] = 1;
        }
    }
}

/* ---- polygons ---- */

void artf2DPolygonRow(const Frame2D* f, const Item2D* it, int y, int x0, int x1, float* cover, float* crossX, int* crossDir) {
    for (int x = x0; x < x1; ++x) cover[x - x0] = 0;
    const Edge2D* edges = f->edges.items + it->edgeStart;
    for (int s = 0; s < 4; ++s) {
        const float sy = (float)y + ((float)s + .5f) * .25f;
        int n = 0;
        for (int e = 0; e < it->edgeCount; ++e) {
            const Edge2D* g = &edges[e];
            if ((g->y0 <= sy) == (g->y1 <= sy)) continue;
            const float x = g->x0 + (sy - g->y0) / (g->y1 - g->y0) * (g->x1 - g->x0);
            const int dir = g->y1 > g->y0 ? 1 : -1;
            int at = n++;
            while (at > 0 && crossX[at - 1] > x) {
                crossX[at] = crossX[at - 1];
                crossDir[at] = crossDir[at - 1];
                --at;
            }
            crossX[at] = x;
            crossDir[at] = dir;
        }
        int winding = 0;
        for (int i = 0; i + 1 < n; ++i) {
            winding += crossDir[i];
            if (!winding) continue;
            const float a = artfMaxf(crossX[i], (float)x0), b = artfMinf(crossX[i + 1], (float)x1);
            if (b <= a) continue;
            const int first = (int)a, last = artfMini((int)ceilf(b), x1);
            for (int x = first; x < last; ++x) {
                const float lo = artfMaxf((float)x, a), hi = artfMinf((float)x + 1.f, b);
                if (hi > lo) cover[x - x0] += (hi - lo) * .25f;
            }
        }
    }
}

/* ---- light ---- */

/* What reaches a point of a sprite from the tile's lights: diffuse (to be times its color) and highlight. */
static void lightAt(const Frame2D* f, const uint16_t* lamps, uint32_t lampCount, const Item2D* it, float px, float py,
                    const float n[3], float diffuse[3], float highlight[3]) {
    const ArtfSprite2DDesc* d = &it->sp->desc;
    const bool shiny = d->specular > 0;
    const float rough = artfClampf(d->roughness, .05f, 1), alpha = rough * rough, alpha2 = alpha * alpha;
    for (uint32_t i = 0; i < lampCount; ++i) {
        const Lamp2D* l = &f->lamps.items[lamps[i]];
        if (!(l->mask & d->lightMask)) continue;
        float lx, ly, lz, k;
        if (l->kind == ARTF_LIGHT2D_SUN) {
            lx = l->dirX;
            ly = l->dirY;
            lz = l->dirZ;
            k = 1;
        } else {
            const float vx = l->x - px, vy = l->y - py, d2 = vx * vx + vy * vy;
            if (d2 >= l->range2) continue;
            const float x = 1.f - d2 * l->inverseRange2;
            k = x * x;
            if (l->kind == ARTF_LIGHT2D_SPOT) {
                const float dist = sqrtf(d2);
                const float c = dist > 0 ? -(vx * l->dirX + vy * l->dirY) / dist : 1;
                if (c <= l->cosOuter) continue;
                if (c < l->cosInner) {
                    const float t = (c - l->cosOuter) / (l->cosInner - l->cosOuter);
                    k *= t * t * (3.f - 2.f * t);
                }
            }
            const float inv = 1.f / sqrtf(d2 + l->height * l->height);
            lx = vx * inv;
            ly = vy * inv;
            lz = l->height * inv;
        }
        const float ndl = n[0] * lx + n[1] * ly + n[2] * lz;
        if (ndl <= 0) continue;
        if (l->shadow) {
            k *= artf2DShadowSee(l, px, py, it->owner);
            if (k <= 0) continue;
        }
        const float w = k * ndl;
        for (int c = 0; c < 3; ++c) diffuse[c] += l->color[c] * w;
        if (shiny) {
            float hx = lx, hy = ly, hz = lz + 1.f;
            const float hl = sqrtf(hx * hx + hy * hy + hz * hz);
            const float nh = artfMaxf((n[0] * hx + n[1] * hy + n[2] * hz) / hl, 0);
            const float dd = nh * nh * (alpha2 - 1.f) + 1.f;
            const float spec = d->specular * .25f * alpha2 / (dd * dd) * w;
            for (int c = 0; c < 3; ++c) highlight[c] += l->color[c] * spec;
        }
    }
}

/* gi's light at a pixel, between its texels. */
static void giAt(const Frame2D* f, float px, float py, float out[3]) {
    const float s = 1.f / (float)f->giScale;
    const float gx = px * s - .5f, gy = py * s - .5f;
    const float fx = floorf(gx), fy = floorf(gy), wx = gx - fx, wy = gy - fy;
    const int x0 = artfClampi((int)fx, 0, f->giWidth - 1), x1 = artfClampi((int)fx + 1, 0, f->giWidth - 1);
    const int y0 = artfClampi((int)fy, 0, f->giHeight - 1), y1 = artfClampi((int)fy + 1, 0, f->giHeight - 1);
    const float* g = f->giLight;
    const size_t w = (size_t)f->giWidth;
    for (int c = 0; c < 3; ++c) {
        const float top = g[(y0 * w + x0) * 3 + c] + (g[(y0 * w + x1) * 3 + c] - g[(y0 * w + x0) * 3 + c]) * wx;
        const float bottom = g[(y1 * w + x0) * 3 + c] + (g[(y1 * w + x1) * 3 + c] - g[(y1 * w + x0) * 3 + c]) * wx;
        out[c] = top + (bottom - top) * wy;
    }
}

/* ---- a tile ---- */

void artf2DDrawTile(const Frame2D* f, uint32_t tile, TileScratch2D* scratch) {
    const int tx = (int)(tile % (uint32_t)f->tilesX), ty = (int)(tile / (uint32_t)f->tilesX);
    const int x0 = tx * TILE2D, y0 = ty * TILE2D;
    const int x1 = artfMini(x0 + TILE2D, f->width), y1 = artfMini(y0 + TILE2D, f->height);
    float* color = scratch->color;
    for (int i = 0; i < TILE2D * TILE2D; ++i)
        for (int c = 0; c < 4; ++c) color[i * 4 + c] = f->background[c];

    const uint32_t* items = f->tileItems[tile];
    const uint16_t* lamps = f->tileLamps[tile];
    const uint32_t lampCount = f->tileLampCount[tile];
    for (uint32_t n = 0; n < f->tileItemCount[tile]; ++n) {
        const Item2D* it = &f->items.items[items[n]];
        const int ax = artfMaxi(it->x0, x0), bx = artfMini(it->x1, x1);
        const int ay = artfMaxi(it->y0, y0), by = artfMini(it->y1, y1);
        const float soft = 1.f / it->zoom;
        const bool lit = it->lit;
        const bool reflects = it->owner && f->reflected;
        for (int y = ay; y < by; ++y) {
            if (it->polygon) artf2DPolygonRow(f, it, y, ax, bx, scratch->cover, scratch->crossX, scratch->crossDir);
            const float py = (float)y + .5f;
            for (int x = ax; x < bx; ++x) {
                const float px = (float)x + .5f;
                const ArtfVec2 l = artf2DApply(&it->toLocal, px, py);
                float cover = it->polygon ? artfMinf(scratch->cover[x - ax], 1) : artf2DCoverAt(it->sp, l.x, l.y, soft);
                if (cover <= 0) continue;
                float rgba[4], emit[3], normal[3];
                artf2DSample(it, l.x, l.y, cover, rgba, emit, lit ? normal : NULL);
                if (rgba[3] <= 0 && emit[0] + emit[1] + emit[2] <= 0) continue;
                float out[3];
                if (lit) {
                    float light[3] = {f->ambient[0], f->ambient[1], f->ambient[2]}, highlight[3] = {0, 0, 0};
                    if (f->giLight) {
                        float g[3];
                        giAt(f, px, py, g);
                        for (int c = 0; c < 3; ++c) light[c] += g[c];
                    }
                    lightAt(f, lamps, lampCount, it, px, py, normal, light, highlight);
                    for (int c = 0; c < 3; ++c) out[c] = rgba[c] * light[c] + highlight[c] * rgba[3] + emit[c];
                    if (reflects && rgba[3] > .5f) {
                        /* (what this wall sends on, for gi's next frame: its color, unpremultiplied, times its light) */
                        float* r = f->reflected + ((size_t)(y / f->giScale) * f->giWidth + x / f->giScale) * 4;
                        const float inv = 1.f / rgba[3];
                        for (int c = 0; c < 3; ++c) r[c] += rgba[c] * inv * light[c];
                        r[3] += 1;
                    }
                } else
                    for (int c = 0; c < 3; ++c) out[c] = rgba[c] + emit[c];
                float* dst = color + ((y - y0) * TILE2D + (x - x0)) * 4;
                const float keep = 1.f - rgba[3];
                for (int c = 0; c < 3; ++c) dst[c] = out[c] + dst[c] * keep;
                dst[3] = rgba[3] + dst[3] * keep;
            }
        }
    }

    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float* src = color + ((y - y0) * TILE2D + (x - x0)) * 4;
            const size_t at = (size_t)y * f->width + x;
            if (f->hdr) memcpy(f->hdr + at * 4, src, 16);
            else artf2DTone(f, src, f->pixels + at);
        }
}
