/* artful — how each instance moved since the frame before, for what a renderer does after the frame: temporal
 * antialiasing follows a surface to where it was, and motion blur spreads it along the way it went. A renderer keeps
 * one of these, as a scene can be drawn by more than one. Instances are known by their handles, which stay when others
 * are removed; one that was not there a frame ago stood still. What bones and targets bend is followed by its
 * vertices: of each instance that bends, where each vertex was at the step before, from its mesh's positions. */
#include "scene/sceneInternal.h"

/* The rows of the inverse of a transform: world to object. false: it has none. */
static bool inverseOf(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float k0 = e * i - f * h, k1 = c * h - b * i, k2 = b * f - c * e;
    const float det = a * k0 + d * k1 + g * k2;
    if (det == 0) return false;
    const float inv = 1.f / det;
    out[0] = k0 * inv; out[1] = k1 * inv; out[2] = k2 * inv;
    out[4] = (f * g - d * i) * inv; out[5] = (a * i - c * g) * inv; out[6] = (c * d - a * f) * inv;
    out[8] = (d * h - e * g) * inv; out[9] = (b * g - a * h) * inv; out[10] = (a * e - b * d) * inv;
    for (int r = 0; r < 3; ++r) out[r * 4 + 3] = -(out[r * 4] * m[3] + out[r * 4 + 1] * m[7] + out[r * 4 + 2] * m[11]);
    return true;
}

static const float still[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

/* What bones and targets did to an instance's vertices since the last step: false when nothing. */
static bool poseStep(ArtfMotionPose* p, const ArtfMeshData* mesh, bool vertices, bool there) {
    if (!vertices) {
        const bool bent = there && p->version != mesh->deformVersion;
        p->version = mesh->deformVersion;
        return bent;
    }
    const uint32_t slots = mesh->positionStride;
    if (p->slots != slots) {
        artfFree(p->was);
        artfFree(p->back);
        p->was = artfMalloc((size_t)slots * 12 + 4);
        p->back = artfMalloc((size_t)slots * 12 + 4);
        p->slots = slots;
        there = false;
    }
    const bool bent = there && p->version != mesh->deformVersion;
    const float* at = mesh->positions;
    for (uint32_t i = 0; i < slots; ++i)
        for (int c = 0; c < 3; ++c) {
            const float now = at[(size_t)c * slots + i];
            if (bent) p->back[(size_t)i * 3 + c] = p->was[(size_t)i * 3 + c] - now;
            p->was[(size_t)i * 3 + c] = now;
        }
    p->version = mesh->deformVersion;
    return bent;
}

void artfMotionStep(ArtfMotion* m, const ArtfScene* s, bool vertices) {
    const size_t count = s->instances.len;
    if (m->scene != s->id) {
        /* another scene: nothing it had was here a frame ago */
        if (m->handles) memset(m->there, 0, m->handles);
        m->scene = s->id;
    }
    if (m->capacity < count) {
        m->capacity = count * 2 + 16;
        artfFree(m->motion);
        artfFree(m->bent);
        artfFree(m->back);
        m->motion = artfMalloc(m->capacity * 48);
        m->bent = artfMalloc(m->capacity);
        m->back = artfMalloc(m->capacity * sizeof *m->back);
    }
    bool moving = false;
    for (size_t i = 0; i < count; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        const uint32_t handle = s->instanceHandles.items[i];
        if (handle > m->handles) {
            const size_t room = (size_t)handle * 2 + 16;
            float* at = artfMalloc(room * 48);
            uint8_t* there = artfCalloc(room, 1);
            ArtfMotionPose* poses = artfCalloc(room, sizeof *poses);
            if (m->handles) {
                memcpy(at, m->at, m->handles * 48);
                memcpy(there, m->there, m->handles);
                memcpy(poses, m->poses, m->handles * sizeof *poses);
            }
            artfFree(m->at);
            artfFree(m->there);
            artfFree(m->poses);
            m->at = at;
            m->there = there;
            m->poses = poses;
            m->handles = room;
        }
        float* was = m->at + (size_t)(handle - 1) * 12;
        float* out = m->motion + i * 12;
        float back[12];
        if (m->there[handle - 1] && memcmp(was, d->m, 48) != 0 && inverseOf(d->m, back)) {
            /* from the world now, through the object, to the world before */
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) out[r * 4 + c] = was[r * 4] * back[c] + was[r * 4 + 1] * back[4 + c] + was[r * 4 + 2] * back[8 + c] + (c == 3 ? was[r * 4 + 3] : 0.f);
            moving = true;
        } else {
            memcpy(out, still, 48);
        }
        /* (what bends: its vertices, as they were at the step before) */
        const ArtfMeshData* mesh = &s->meshes.items[d->mesh];
        ArtfMotionPose* pose = &m->poses[handle - 1];
        /* (another mesh than at the step before, as a level of detail taken in turn: its vertices are not those) */
        const bool same = pose->mesh == d->mesh + 1;
        pose->mesh = d->mesh + 1;
        m->bent[i] = mesh->skin && poseStep(pose, mesh, vertices, m->there[handle - 1] && same);
        m->back[i] = m->bent[i] && vertices ? pose->back : NULL;
        moving |= m->bent[i] != 0;
        memcpy(was, d->m, 48);
        m->there[handle - 1] = 1;
    }
    m->count = count;
    if (moving || m->moving) ++m->version;
    m->moving = moving;
}

void artfMotionFree(ArtfMotion* m) {
    for (size_t i = 0; i < m->handles; ++i) {
        artfFree(m->poses[i].was);
        artfFree(m->poses[i].back);
    }
    artfFree(m->poses);
    artfFree(m->at);
    artfFree(m->there);
    artfFree(m->motion);
    artfFree(m->bent);
    artfFree(m->back);
    memset(m, 0, sizeof *m);
}
