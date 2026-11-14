/* artful — lights' shapes and profiles: a tube or a rectangle in place of a point, and how much light goes each way,
 * as a lamp's IES file says. */
#include "scene/sceneInternal.h"

#include <stdlib.h>

void artfLightSetShape(ArtfScene* s, ArtfLight l, ArtfVec3 along, ArtfVec3 facing, float length, float width) {
    if (!l || l > ARTF_MAX_LIGHTS) return;
    float* d = s->lightShape[l - 1];
    memset(d, 0, 8 * sizeof *d);
    const ArtfVec3 f = artfVec3Normalize(facing);
    const bool rectangle = width > 0 && artfVec3Dot(f, f) > .5f;
    /* (a rectangle's length is square to the way it faces) */
    const ArtfVec3 u = artfVec3Normalize(rectangle ? artfVec3Sub(along, artfVec3Scale(f, artfVec3Dot(along, f))) : along);
    if (!(length > 0) || !(artfVec3Dot(u, u) > .5f)) return;
    d[0] = u.x;
    d[1] = u.y;
    d[2] = u.z;
    d[3] = length * .5f;
    if (rectangle) {
        d[4] = f.x;
        d[5] = f.y;
        d[6] = f.z;
        d[7] = width * .5f;
    }
}

void artfLightSetProfile(ArtfScene* s, ArtfLight l, const float* table, int vertical, int horizontal) {
    if (!l || l > ARTF_MAX_LIGHTS) return;
    float** p = &s->lightProfile[l - 1];
    ++s->profileVersion;
    if (!table || vertical < 1 || horizontal < 1) {
        artfFree(*p);
        *p = NULL;
        return;
    }
    if (!*p) *p = artfMalloc(ARTF_PROFILE_FLOATS * sizeof(float));
    /* bilinear, by the table's own steps: from 0 to 180 degrees with both ends, and round from 0 to 360 */
    for (int j = 0; j < ARTF_PROFILE_H; ++j) {
        const float y = (float)j * (float)horizontal / (float)ARTF_PROFILE_H;
        const int j0 = artfMini((int)y, horizontal - 1), j1 = (j0 + 1) % horizontal;
        const float ty = y - (float)j0;
        for (int i = 0; i < ARTF_PROFILE_V; ++i) {
            const float x = (float)i * (float)(vertical - 1) / (float)(ARTF_PROFILE_V - 1);
            const int i0 = artfMini((int)x, artfMaxi(vertical - 2, 0)), i1 = artfMini(i0 + 1, vertical - 1);
            const float tx = x - (float)i0;
            const float a = table[j0 * vertical + i0] + (table[j0 * vertical + i1] - table[j0 * vertical + i0]) * tx;
            const float b = table[j1 * vertical + i0] + (table[j1 * vertical + i1] - table[j1 * vertical + i0]) * tx;
            (*p)[j * ARTF_PROFILE_V + i] = artfMaxf(a + (b - a) * ty, 0);
        }
    }
}

/* ---- IES files (ANSI/IES LM-63) ---- */

/* What is between two of a sorted list's angles at a, linearly; outside them, otherwise. */
static float between(const float* angles, const float* values, int count, float a, float otherwise) {
    if (count == 1) return values[0];
    if (a < angles[0] - 1e-3f || a > angles[count - 1] + 1e-3f) return otherwise;
    int k = 0;
    while (k < count - 2 && a > angles[k + 1]) ++k;
    const float span = angles[k + 1] - angles[k], t = span > 0 ? artfClampf((a - angles[k]) / span, 0, 1) : 0.f;
    return values[k] + (values[k + 1] - values[k]) * t;
}

float* artfIesDecode(const void* data, size_t size, int* vertical, int* horizontal) {
    if (!data || size < 8) return NULL;
    /* a copy that ends, with its numbers after the line that says TILT */
    char* text = artfMalloc(size + 1);
    memcpy(text, data, size);
    text[size] = 0;
    char* at = NULL;
    for (char* line = text; line && *line;) {
        while (*line == ' ' || *line == '\t') ++line;
        if (!strncmp(line, "TILT=", 5)) {
            at = line + 5;
            break;
        }
        line = strchr(line, '\n');
        if (line) ++line;
    }
    float* out = NULL;
    double* numbers = NULL;
    if (!at) goto done;
    const bool tilted = !strncmp(at, "INCLUDE", 7);
    at = strchr(at, '\n');
    if (!at) goto done;
    /* all the numbers, apart by spaces, commas or lines */
    size_t count = 0, room = 0;
    for (char* p = at; *p;) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') { ++p; continue; }
        char* end;
        const double v = strtod(p, &end);
        if (end == p) goto done;
        if (count == room) {
            room = room ? room * 2 : 1024;
            double* grown = artfMalloc(room * sizeof *grown);
            if (count) memcpy(grown, numbers, count * sizeof *grown);
            artfFree(numbers);
            numbers = grown;
        }
        numbers[count++] = v;
        p = end;
    }
    size_t k = 0;
    if (tilted) {
        /* (the lamp's tilt: its geometry, how many angles, the angles and their factors, which are left out) */
        if (count < 2) goto done;
        const size_t n = (size_t)numbers[1];
        k = 2 + 2 * n;
    }
    if (k + 13 > count) goto done;
    const int nv = (int)numbers[k + 3], nh = (int)numbers[k + 4];
    k += 13;
    if (nv < 1 || nh < 1 || nv > 4096 || nh > 4096 || k + (size_t)nv + (size_t)nh + (size_t)nv * (size_t)nh > count) goto done;
    float* angles = artfMalloc(((size_t)nv + nh + (size_t)nv * nh) * sizeof(float));
    float* around = angles + nv;
    float* candela = around + nh;
    for (int i = 0; i < nv; ++i) angles[i] = (float)numbers[k + (size_t)i];
    for (int j = 0; j < nh; ++j) around[j] = (float)numbers[k + (size_t)nv + (size_t)j];
    for (int i = 0; i < nv * nh; ++i) candela[i] = (float)numbers[k + (size_t)nv + (size_t)nh + (size_t)i];
    const int outV = 181, outH = nh == 1 ? 1 : 72;
    out = artfMalloc((size_t)outV * outH * sizeof(float));
    float* row = artfMalloc((size_t)nh * sizeof(float));
    const float last = around[nh - 1];
    float most = 0;
    for (int j = 0; j < outH; ++j) {
        /* round the light, folded as the file's symmetry says: a quarter, a half, or all of it */
        float phi = (float)j * 5.f;
        if (nh > 1 && last <= 90.f + 1e-3f) {
            phi = fmodf(phi, 180.f);
            phi = phi > 90.f ? 180.f - phi : phi;
        } else if (nh > 1 && last <= 180.f + 1e-3f) {
            phi = phi > 180.f ? 360.f - phi : phi;
        }
        for (int i = 0; i < outV; ++i) {
            for (int h = 0; h < nh; ++h) row[h] = between(angles, candela + (size_t)h * nv, nv, (float)i, 0.f);
            float v = between(around, row, nh, phi, -1.f);
            if (v < 0) {
                /* (past the last angle, toward the first, round) */
                const float a = phi < around[0] ? phi + 360.f : phi;
                const float span = around[0] + 360.f - last, t = span > 0 ? artfClampf((a - last) / span, 0, 1) : 0.f;
                v = row[nh - 1] + (row[0] - row[nh - 1]) * t;
            }
            out[j * outV + i] = artfMaxf(v, 0);
            most = artfMaxf(most, out[j * outV + i]);
        }
    }
    artfFree(row);
    artfFree(angles);
    if (!(most > 0)) {
        artfFree(out);
        out = NULL;
        goto done;
    }
    for (int i = 0; i < outV * outH; ++i) out[i] /= most;
    *vertical = outV;
    *horizontal = outH;
done:
    artfFree(numbers);
    artfFree(text);
    return out;
}

bool artfSceneShapedLights(const ArtfScene* s) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < ARTF_MAX_LIGHTS && seen < ARTF_FRAME_LIGHTS; ++i) {
        if (!s->lightOn[i]) continue;
        ++seen;
        if (s->lightShape[i][3] > 0 || s->lightProfile[i]) return true;
    }
    return false;
}
