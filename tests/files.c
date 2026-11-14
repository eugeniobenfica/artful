/* artful-tests: glTF, its extensions and compressed blocks. */
#include "tests.h"

/* ---- blocks ---- */

/* A KTX2 file of one level, of that format, holding these blocks. */
static size_t makeKtx2(uint8_t* out, uint32_t format, uint32_t width, uint32_t height, const uint8_t* blocks, size_t bytes) {
    static const uint8_t mark[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 13, 10, 26, 10};
    memset(out, 0, 104);
    memcpy(out, mark, 12);
    put32le(out + 12, format);
    put32le(out + 16, 1);
    put32le(out + 20, width);
    put32le(out + 24, height);
    put32le(out + 36, 1);
    put32le(out + 40, 1);
    put32le(out + 80, 104);
    put32le(out + 88, (uint32_t)bytes);
    put32le(out + 96, (uint32_t)bytes);
    memcpy(out + 104, blocks, bytes);
    return 104 + bytes;
}

void testBlocks(void) {
    /* what is made and turned back stays close to what it was */
    enum { SIDE = 32 };
    static uint32_t px[SIDE * SIDE];
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) px[y * SIDE + x] = (uint32_t)(255 - x * 4) << 24 | (uint32_t)(x * 8) << 16 | (uint32_t)(y * 8) << 8 | (uint32_t)((x + y) * 4);
    for (int kind = 0; kind < 4; ++kind) {
        ArtfScene* s = artfSceneNew();
        static uint32_t opaque[SIDE * SIDE];
        for (int i = 0; i < SIDE * SIDE; ++i) opaque[i] = px[i] | 0xFF000000u;
        const ArtfTexture id = artfSceneAddTexture(s, kind & 1 ? px : opaque, SIDE, SIDE, 0);
        const ArtfTextureData* t = &s->textures.items[id];
        uint32_t format, offsets[16];
        size_t size;
        uint8_t* blocks = artfTextureEncode(t, kind >= 2, &format, &size, offsets);
        CHECK(size < (size_t)SIDE * SIDE * 4 * 2 / 3 * 2);
        ArtfTextureData back;
        memset(&back, 0, sizeof back);
        back.width = t->width;
        back.height = t->height;
        back.mipCount = t->mipCount;
        back.format = format;
        back.blocks = blocks;
        memcpy(back.blockOffsets, offsets, sizeof offsets);
        artfTextureDecode(&back);
        int worst = 0;
        for (int i = 0; i < SIDE * SIDE; ++i)
            for (int shift = 0; shift < 32; shift += 8) worst = artfMaxi(worst, abs(CHANNEL(back.pixels[i], shift) - CHANNEL(t->pixels[i], shift)));
        printf("blocks %d: the worst a channel strayed: %d\n", kind, worst);
        CHECK(worst < 40);
        artfFree(back.pixels);
        artfFree(blocks);
        artfSceneFree(s);
    }

    /* a file: read, and what is wrong in it refused */
    static uint8_t ktx[104 + 4096];
    static uint8_t blocks[4096];
    uint32_t state = 7;
    for (int i = 0; i < 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        blocks[i] = (uint8_t)(state >> 24);
    }
    artfSetLog(countLog, &logged);
    logged = 0;
    ArtfScene* s = artfSceneNew();
    const size_t size = makeKtx2(ktx, 137, 64, 64, blocks, 4096);
    CHECK(artfSceneAddTextureKtx2(s, ktx, size) != 0);
    CHECK(artfSceneAddTextureKtx2(s, ktx, 100) == 0);
    CHECK(artfSceneAddTextureKtx2(s, "not this", 8) == 0);
    for (size_t cut = 0; cut < 110; ++cut) artfSceneAddTextureKtx2(s, ktx, cut);
    for (size_t i = 12; i < 104; ++i) {
        static uint8_t bad[104 + 4096];
        memcpy(bad, ktx, size);
        bad[i] ^= 0xFF;
        artfSceneAddTextureKtx2(s, bad, size);
    }
    CHECK(logged > 2);
    artfSetLog(NULL, NULL);
    artfSceneFree(s);

    /* The GPU knows these formats better than this code: random blocks, each format it takes, drawn by both.
     * Random blocks reach every mode, so what is wrong in a decoder shows as noise that does not match. */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    static const struct { uint32_t format, bytes; } formats[] = {{131, 8}, {137, 16}, {147, 8}, {151, 16}};
    for (size_t f = 0; f < ARTF_COUNT(formats); ++f) {
        s = plainScene();
        const size_t bytes = (size_t)(64 / 4) * (64 / 4) * formats[f].bytes;
        const size_t fileSize = makeKtx2(ktx, formats[f].format, 64, 64, blocks, bytes);
        ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTextureKtx2(s, ktx, fileSize), 0, 1, artfVec3(0, 0, 0)};
        addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 2);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 12);
        printf("format %u: pixels that differ by more than 12 of 255: %.2f%%\n", formats[f].format, far * 100);
        CHECK(far < .01);
        artfSceneFree(s);
    }
    /* plain textures compressed as they go up look like themselves */
    artfRendererFree(gpu);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.compressTextures = true;
    gpu = artfRendererNew(&o);
    s = richScene();
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) < .02);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

/* Blocks of noise, turned into pixels, hashed. The hashes are of what was checked, block by block, against a decoder of
 * someone else's: 65,536 blocks of BC7 alike, and 46,000 of ASTC within one of 255 (it rounds a step sooner). */
void testBlockDecoders(void) {
    uint32_t state = 12345;
    uint64_t bc7 = 1469598103934665603ull, astc = bc7;
    uint32_t out[144];
    int drawn = 0;
    for (int i = 0; i < 200000; ++i) {
        uint8_t b[16];
        for (int k = 0; k < 16; ++k) {
            state = state * 1664525u + 1013904223u;
            b[k] = (uint8_t)(state >> 24);
        }
        artfDecodeBc7(b, out);
        for (int k = 0; k < 16; ++k) bc7 = (bc7 ^ out[k]) * 1099511628211ull;
        artfDecodeAstc(b, 6, 6, out);
        if (out[0] != 0xFFFF00FFu) ++drawn;
        for (int k = 0; k < 36; ++k) astc = (astc ^ out[k]) * 1099511628211ull;
    }
    printf("blocks: bc7 %016llx, astc %016llx, %d of ASTC's are legal\n", (unsigned long long)bc7, (unsigned long long)astc, drawn);
    CHECK(bc7 == 0x173e39b598bebd9dull);
    CHECK(astc == 0x6bdc1b46124e7596ull);
}

void testGltfSparse(void) {
    static const char text[] = "{\"asset\":{\"version\":\"2.0\"},%s\"buffers\":[{\"byteLength\":44,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36}],\"accessors\":[{\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0],\"sparse\":{\"count\":3,\"indices\":{\"bufferView\":0,\"componentType\":5123},\"values\":{\"bufferView\":1}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    char file[1200];
    int size = snprintf(file, sizeof file, text, "");
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    /* the three points came from the changes alone: a triangle with an area */
    CHECK(s->meshes.len >= 1 && s->meshes.items[0].vertexCount == 3 && s->meshes.items[0].radius > .4f);
    artfSceneFree(s);
    s = artfSceneNew();
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_nothing_here\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 0);
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_mesh_quantization\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    artfSceneFree(s);
}

/* KHR_lights_punctual, cameras, KHR_materials_emissive_strength and KHR_texture_transform. */
void testGltfExtensions(void) {
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_texture_transform\"],\"extensions\":{\"KHR_ligh"
        "ts_punctual\":{\"lights\":[{\"type\":\"point\",\"intensity\":20,\"range\":10,\"color\":[1,0.5,0.25]},{\"type\""
        ":\"directional\",\"intensity\":3},{\"type\":\"spot\",\"intensity\":5,\"spot\":{\"innerConeAngle\":0.2,\"outerC"
        "oneAngle\":0.5}}]}},\"buffers\":[{\"byteLength\":84,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAA"
        "AAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAA/AAAAAAAAAAAAAAA/\"}],\"bu"
        "fferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":"
        "24},{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24}],\"accessors\":[{\"bufferView\":0,\"componentType\":512"
        "6,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5126,\"ty"
        "pe\":\"VEC2\",\"count\":3},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC2\",\"count\":3}],\"material"
        "s\":[{\"emissiveFactor\":[1,1,1],\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4}}"
        ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"extensions\":{\"KHR_texture_transform\":{\"offs"
        "et\":[0.5,0],\"scale\":[2,2],\"texCoord\":1}}}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0"
        ",\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],\"cameras\":[{\"type\":\"perspective\",\"perspective\":"
        "{\"yfov\":0.7,\"znear\":0.05}}],\"nodes\":[{\"mesh\":0},{\"translation\":[0,2,0],\"extensions\":{\"KHR_lights_"
        "punctual\":{\"light\":0}}},{\"extensions\":{\"KHR_lights_punctual\":{\"light\":1}}},{\"translation\":[1,1,1],\""
        "extensions\":{\"KHR_lights_punctual\":{\"light\":2}}},{\"translation\":[0,0,5],\"camera\":0}],\"scenes\":[{\"n"
        "odes\":[0,1,2,3,4]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    /* the second set of coordinates, moved by half and doubled: (0.5, 0) and (0, 0.5) become (1.5, 0) and (0.5, 1) */
    float most = 0, least = 9;
    const ArtfMeshData* mesh = &s->meshes.items[0];
    for (uint32_t i = 0; i < mesh->vertexCount; ++i) {
        most = artfMaxf(most, artfHalfToFloat(mesh->vertices[i].u));
        least = artfMinf(least, artfHalfToFloat(mesh->vertices[i].u));
    }
    CHECK(most == 1.5f && least == .5f);
    CHECK(s->materials.items[s->materials.len - 1].emissive[0] == 4.f);
    /* the point light, its range kept; the sun, down -z; the spot, a cone */
    CHECK(s->lightOn[0] && s->lights[0].position[1] == 2.f && s->lights[0].radius == 10.f);
    CHECK(s->lights[0].color[0] > s->lights[0].color[1] && s->lights[0].color[1] > s->lights[0].color[2]);
    CHECK(s->sunColor.x == 3.f && s->toSun.z > .99f);
    CHECK(s->lightOn[1] && s->lights[1].spotCut > .87f && s->lights[1].spotCut < .88f);
    ArtfCamera eye;
    CHECK(artfModelCameraCount(s, m) == 1 && artfModelCamera(s, m, 0, &eye));
    CHECK(eye.position.z == 5.f && eye.target.z == 4.f && eye.up.y == 1.f && eye.fovY == .7f);
    CHECK(!artfModelCamera(s, m, 1, &eye));
    /* lights and cameras go with the model */
    const ArtfTransform moved = artfTransformAt(3, 0, 0, 1);
    artfModelSetTransform(s, m, &moved);
    CHECK(s->lights[0].position[0] == 3.f && artfModelCamera(s, m, 0, &eye) && eye.position.x == 3.f);
    artfSceneFree(s);
}

/* ---- EXT_meshopt_compression: encoders as meshoptimizer's streams are laid out, to check the decoders against ---- */

typedef struct Bytes { uint8_t* p; size_t n, cap; } Bytes;
static void bytePut(Bytes* b, uint8_t v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p = realloc(b->p, b->cap);
    }
    b->p[b->n++] = v;
}
static void vbytePut(Bytes* b, uint32_t v) {
    do {
        const uint8_t group = (uint8_t)(v & 127);
        v >>= 7;
        bytePut(b, (uint8_t)(group | (v ? 128 : 0)));
    } while (v);
}

/* Vertices: each group of sixteen bytes in the fewest bits that hold it, as the encoder chooses. */
static void encodeVertices(Bytes* out, const uint8_t* v, size_t count, size_t stride) {
    bytePut(out, 0xA0);
    uint8_t last[256];
    memcpy(last, v, stride);
    size_t block = 8192 / stride & ~(size_t)15;
    if (block > 256) block = 256;
    for (size_t first = 0; first < count; first += block) {
        const size_t n = count - first < block ? count - first : block, aligned = (n + 15) & ~(size_t)15;
        for (size_t k = 0; k < stride; ++k) {
            uint8_t deltas[256] = {0};
            uint8_t p = last[k];
            for (size_t i = 0; i < n; ++i) {
                const uint8_t now = v[(first + i) * stride + k], d = (uint8_t)(now - p);
                deltas[i] = (uint8_t)((d << 1) ^ (uint8_t)((int8_t)d >> 7)); /* zig-zag */
                p = now;
            }
            last[k] = p;
            const size_t groups = aligned / 16, headerAt = out->n;
            for (size_t h = 0; h < (groups + 3) / 4; ++h) bytePut(out, 0);
            for (size_t g = 0; g < groups; ++g) {
                const uint8_t* d = deltas + g * 16;
                int best = 3;
                size_t bestSize = 16;
                for (int bits = 0; bits < 3; ++bits) {
                    const int width = bits == 0 ? 0 : bits == 1 ? 2 : 4, all = (1 << width) - 1;
                    size_t size = (size_t)(16 * width / 8);
                    bool fits = true;
                    for (int i = 0; i < 16; ++i) {
                        if (width == 0) fits &= d[i] == 0;
                        else size += d[i] >= all;
                    }
                    if (fits && size < bestSize) { best = bits; bestSize = size; }
                }
                out->p[headerAt + g / 4] |= (uint8_t)(best << (g % 4 * 2));
                if (best == 3) {
                    for (int i = 0; i < 16; ++i) bytePut(out, d[i]);
                } else if (best > 0) {
                    const int width = best == 1 ? 2 : 4, all = (1 << width) - 1, each = 8 / width;
                    for (int i = 0; i < 16; i += each) {
                        uint8_t byte = 0;
                        for (int j = 0; j < each; ++j) byte = (uint8_t)(byte << width | (d[i + j] >= all ? all : d[i + j]));
                        bytePut(out, byte);
                    }
                    for (int i = 0; i < 16; ++i)
                        if (d[i] >= all) bytePut(out, d[i]);
                }
            }
        }
    }
    /* the first vertex, at the end */
    const size_t tail = stride < 32 ? 32 : stride;
    for (size_t i = 0; i < tail - stride; ++i) bytePut(out, 0);
    for (size_t i = 0; i < stride; ++i) bytePut(out, v[i]);
}

/* Triangles: an edge met lately when there is one, a new vertex or one met lately or a free one; else three free. */
static void encodeTriangles(Bytes* out, const uint16_t* t, size_t count) {
    uint32_t edges[16][2], vertices[16], edge = 0, vertex = 0, next = 0, last = 0;
    memset(edges, 0xFF, sizeof edges);
    memset(vertices, 0xFF, sizeof vertices);
    Bytes codes = {0}, data = {0};
#define PUSH_V(x, adv) (vertices[vertex] = (x), vertex = (vertex + (adv)) & 15)
#define PUSH_E(x, y) (edges[edge][0] = (x), edges[edge][1] = (y), edge = (edge + 1) & 15)
    for (size_t i = 0; i < count; i += 3) {
        const uint32_t a = t[i], b = t[i + 1], c = t[i + 2];
        int fe = -1;
        for (int k = 0; k < 16 && fe < 0; ++k)
            if (edges[(edge - 1 - k) & 15][0] == a && edges[(edge - 1 - k) & 15][1] == b) fe = k;
        if (fe >= 0) {
            int fec = -1;
            if (c == next) fec = 0;
            for (int k = 1; k < 13 && fec < 0; ++k)
                if (vertices[(vertex - 1 - k) & 15] == c) fec = k;
            if (fec < 0) fec = c == last - 1 ? 13 : c == last + 1 ? 14 : 15;
            bytePut(&codes, (uint8_t)(fe << 4 | fec));
            if (fec == 0) ++next;
            if (fec == 15) {
                const int32_t d = (int32_t)(c - last);
                vbytePut(&data, (((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u)));
            }
            if (fec >= 13) last = c;
            PUSH_V(c, fec == 0 || fec >= 13);
            PUSH_E(c, b);
            PUSH_E(a, c);
        } else {
            /* three free: 0xff, and a byte of fifteens */
            bytePut(&codes, 0xFF);
            bytePut(&data, 0xFF);
            const uint32_t abc[3] = {a, b, c};
            for (int k = 0; k < 3; ++k) {
                const int32_t d = (int32_t)(abc[k] - last);
                vbytePut(&data, (((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u)));
                last = abc[k];
            }
            PUSH_V(a, 1);
            PUSH_V(b, 1);
            PUSH_V(c, 1);
            PUSH_E(b, a);
            PUSH_E(c, b);
            PUSH_E(a, c);
        }
    }
#undef PUSH_V
#undef PUSH_E
    bytePut(out, 0xE1);
    for (size_t i = 0; i < codes.n; ++i) bytePut(out, codes.p[i]);
    for (size_t i = 0; i < data.n; ++i) bytePut(out, data.p[i]);
    for (int i = 0; i < 16; ++i) bytePut(out, 0); /* (the table: not used here) */
    free(codes.p);
    free(data.p);
}

static void encodeIndexSequence(Bytes* out, const uint16_t* t, size_t count) {
    bytePut(out, 0xD1);
    uint32_t last[2] = {0, 0};
    for (size_t i = 0; i < count; ++i) {
        /* (alternating between the two lasts, to use both) */
        const uint32_t which = i & 1;
        const int32_t d = (int32_t)(t[i] - last[which]);
        last[which] = t[i];
        vbytePut(out, ((((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u))) << 1 | which);
    }
    for (int i = 0; i < 4; ++i) bytePut(out, 0);
}

static void base64Put(char* out, const uint8_t* p, size_t n) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = digits[v >> 18 & 63];
        out[o++] = digits[v >> 12 & 63];
        out[o++] = i + 1 < n ? digits[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? digits[v & 63] : '=';
    }
    out[o] = 0;
}

/* A hill of 9 x 9 vertices, as a plain file and as one compressed with EXT_meshopt_compression (triangles in one
 * primitive, the same indices as a sequence in another): both draw the same. Then the filters, alone. And the
 * materials' extensions, and an orthographic camera. */
void testGltfMeshopt(void) {
    enum { SIDE = 9, VERTS = SIDE * SIDE, TRIS = (SIDE - 1) * (SIDE - 1) * 2 };
    static float positions[VERTS * 3];
    static uint16_t indices[TRIS * 3];
    for (int z = 0; z < SIDE; ++z)
        for (int x = 0; x < SIDE; ++x) {
            float* p = positions + (z * SIDE + x) * 3;
            p[0] = (float)x * .25f - 1.f;
            p[1] = (float)((x - 4) * (x - 4) + (z - 4) * (z - 4)) * -.02f;
            p[2] = (float)z * .25f - 1.f;
        }
    int n = 0;
    for (int z = 0; z + 1 < SIDE; ++z)
        for (int x = 0; x + 1 < SIDE; ++x) {
            const uint16_t a = (uint16_t)(z * SIDE + x), b = (uint16_t)(a + SIDE);
            /* (the second starts on an edge of the first) */
            indices[n++] = a; indices[n++] = b; indices[n++] = (uint16_t)(b + 1);
            indices[n++] = a; indices[n++] = (uint16_t)(b + 1); indices[n++] = (uint16_t)(a + 1);
        }
    Bytes v = {0}, tri = {0}, seq = {0};
    encodeVertices(&v, (const uint8_t*)positions, VERTS, 12);
    encodeTriangles(&tri, indices, TRIS * 3);
    encodeIndexSequence(&seq, indices, TRIS * 3);
    /* the decoders, straight */
    static float gotPositions[VERTS * 3];
    static uint16_t gotTriangles[TRIS * 3], gotSequence[TRIS * 3];
    CHECK(artfMeshoptDecodeVertices((uint8_t*)gotPositions, VERTS, 12, v.p, v.n) == 0 && !memcmp(gotPositions, positions, sizeof positions));
    CHECK(artfMeshoptDecodeTriangles((uint8_t*)gotTriangles, TRIS * 3, 2, tri.p, tri.n) == 0 && !memcmp(gotTriangles, indices, sizeof indices));
    CHECK(artfMeshoptDecodeIndices((uint8_t*)gotSequence, TRIS * 3, 2, seq.p, seq.n) == 0 && !memcmp(gotSequence, indices, sizeof indices));
    CHECK(artfMeshoptDecodeVertices((uint8_t*)gotPositions, VERTS, 12, v.p, v.n - 1) != 0); /* (cut short: refused) */
    /* both files: plain, and with the compressed views (their fallback buffer holds nothing) */
    const size_t plainSize = sizeof positions + sizeof indices;
    uint8_t* plain = malloc(plainSize);
    memcpy(plain, positions, sizeof positions);
    memcpy(plain + sizeof positions, indices, sizeof indices);
    const size_t packedSize = v.n + tri.n + seq.n;
    uint8_t* packed = malloc(packedSize + 8);
    memcpy(packed, v.p, v.n);
    memcpy(packed + v.n, tri.p, tri.n);
    memcpy(packed + v.n + tri.n, seq.p, seq.n);
    char* b64Plain = malloc(plainSize * 2 + 8);
    char* b64Packed = malloc(packedSize * 2 + 8);
    base64Put(b64Plain, plain, plainSize);
    base64Put(b64Packed, packed, packedSize);
    char* json = malloc(plainSize * 4 + packedSize * 4 + 8192);
    const char* mesh = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1},{\"attributes\":{\"POSITION\":0},\"indices\":2}]}],"
                       "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0";
    const char* accessors = "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":81,\"max\":[1,0,1],\"min\":[-1,-0.64,-1]},"
                            "{\"bufferView\":1,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":384},{\"bufferView\":2,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":384}]";
    ArtfScene* scenes[2];
    for (int k = 0; k < 2; ++k) {
        if (!k)
            sprintf(json, "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":%u,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
                          "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":972,\"byteStride\":12},{\"buffer\":0,\"byteOffset\":972,\"byteLength\":768},"
                          "{\"buffer\":0,\"byteOffset\":972,\"byteLength\":768}],%s,%s}",
                    (unsigned)plainSize, b64Plain, accessors, mesh);
        else
            sprintf(json, "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"EXT_meshopt_compression\"],\"extensionsUsed\":[\"EXT_meshopt_compression\"],"
                          "\"buffers\":[{\"byteLength\":%u,\"uri\":\"data:application/octet-stream;base64,%s\"},{\"byteLength\":1740,\"extensions\":{\"EXT_meshopt_compression\":{\"fallback\":true}}}],"
                          "\"bufferViews\":[{\"buffer\":1,\"byteOffset\":0,\"byteLength\":972,\"byteStride\":12,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"byteStride\":12,\"count\":81,\"mode\":\"ATTRIBUTES\"}}},"
                          "{\"buffer\":1,\"byteOffset\":972,\"byteLength\":768,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"byteStride\":2,\"count\":384,\"mode\":\"TRIANGLES\"}}},"
                          "{\"buffer\":1,\"byteOffset\":972,\"byteLength\":768,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"byteStride\":2,\"count\":384,\"mode\":\"INDICES\"}}}],%s,%s}",
                    (unsigned)packedSize, b64Packed, (unsigned)v.n, (unsigned)v.n, (unsigned)tri.n, (unsigned)(v.n + tri.n), (unsigned)seq.n, accessors, mesh);
        scenes[k] = plainScene();
        setCamera(scenes[k], 0, 2, 2.5f, 0, -.3f, 0);
        artfSceneSetSun(scenes[k], artfVec3(-.4f, -1, -.2f), artfVec3(1, 1, 1));
        artfSceneSetAmbient(scenes[k], artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
        CHECK(artfSceneLoadModelMemory(scenes[k], json, strlen(json), NULL, NULL) != 0);
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, scenes[0], a);
    drawInto(cpu, scenes[1], b);
    CHECK(differentShare(a, b, 0) == 0 && meanOf(a, W / 2, H / 2, 8) > 20);
    artfRendererFree(cpu);
    artfSceneFree(scenes[0]);
    artfSceneFree(scenes[1]);
    artfFree(a);
    artfFree(b);
    free(json); free(b64Plain); free(b64Packed); free(plain); free(packed); free(v.p); free(tri.p); free(seq.p);

    /* the filters: an octahedron's normal, a quaternion, a float from a mantissa and an exponent */
    int8_t oct[4] = {0, 127, 127, 0}; /* (up: u 0, v 1, and the one length) */
    artfMeshoptFilter((uint8_t*)oct, 1, 4, 1);
    CHECK(oct[0] == 0 && oct[1] == 127 && oct[2] == 0);
    int16_t quat[4] = {0, 0, 0, 3 | (32767 & ~3)}; /* (the identity: w left out, as index 3 says; x, y, z nothing) */
    quat[3] = (int16_t)(32767 & ~3) | 3;
    artfMeshoptFilter((uint8_t*)quat, 1, 8, 2);
    CHECK(quat[3] == 32767 && quat[0] == 0 && quat[1] == 0 && quat[2] == 0);
    uint32_t e = (uint32_t)(-2 & 255) << 24 | 6u; /* 6 * 2^-2 */
    artfMeshoptFilter((uint8_t*)&e, 1, 4, 3);
    float ef;
    memcpy(&ef, &e, 4);
    CHECK(ef == 1.5f);

    /* materials: lit by nothing, an index of 1.2, light that goes through; and an orthographic camera */
    static const float corners[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    char b64[64];
    base64Put(b64, (const uint8_t*)corners, sizeof corners);
    char file[2048];
    snprintf(file, sizeof file,
             "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_materials_unlit\",\"KHR_materials_ior\",\"KHR_materials_transmission\"],"
             "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,%s\"}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],"
             "\"materials\":[{\"extensions\":{\"KHR_materials_unlit\":{}}},{\"extensions\":{\"KHR_materials_ior\":{\"ior\":1.2}}},"
             "{\"extensions\":{\"KHR_materials_transmission\":{\"transmissionFactor\":0.8},\"KHR_materials_ior\":{\"ior\":1.33}}}],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0},{\"attributes\":{\"POSITION\":0},\"material\":1},"
             "{\"attributes\":{\"POSITION\":0},\"material\":2}]}],"
             "\"cameras\":[{\"type\":\"orthographic\",\"orthographic\":{\"xmag\":2,\"ymag\":1.5,\"znear\":0.1,\"zfar\":50}}],"
             "\"nodes\":[{\"mesh\":0},{\"translation\":[0,0,5],\"camera\":0}],\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0}",
             b64);
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, strlen(file), NULL, NULL);
    CHECK(m != 0 && s->materials.len >= 3);
    const ArtfMaterialData* made = &s->materials.items[s->materials.len - 3];
    CHECK(made[0].flags & ARTF_MATERIAL_UNLIT);
    CHECK(fabsf(made[1].specular - .2066f) < .001f && !(made[1].flags & ARTF_MATERIAL_UNLIT));
    CHECK(made[2].flags & ARTF_MATERIAL_BLEND && fabsf(made[2].opacity - .2f) < 1e-5f && fabsf(made[2].ior - 1.33f) < 1e-5f);
    ArtfCamera eye;
    CHECK(artfModelCameraCount(s, m) == 1 && artfModelCamera(s, m, 0, &eye));
    /* orthographic, twice ymag high, from where the camera is, as far as zfar */
    CHECK(eye.orthoHeight == 3.f && fabsf(eye.near_ - .1f) < 1e-6f && eye.far_ == 50.f);
    CHECK(fabsf(eye.position.z - 5.f) < 1e-4f && eye.target.z < eye.position.z);
    artfSceneFree(s);
}

/* glTF's extensions past the core: what they say, and their textures. */
void testGltfMore(void) {
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_materials_specular\",\"KHR_materials_volume\",\"KHR_materials_iridescence\",\"KHR_materials_anisotropy\"],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\","
        "\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],"
        "\"images\":[{\"uri\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1,\"clearcoatTexture\":{\"index\":0},\"clearcoatRoughnessTexture\":{\"index\":0},"
        "\"clearcoatNormalTexture\":{\"index\":0}},\"KHR_materials_sheen\":{\"sheenColorFactor\":[1,1,1],\"sheenRoughnessFactor\":0.5,\"sheenColorTexture\":{\"index\":0},"
        "\"sheenRoughnessTexture\":{\"index\":0}},\"KHR_materials_specular\":{\"specularFactor\":0.5,\"specularColorFactor\":[1,0.5,0.25]},"
        "\"KHR_materials_volume\":{\"attenuationColor\":[0.5,1,0.25],\"attenuationDistance\":2},\"KHR_materials_iridescence\":{\"iridescenceFactor\":0.75,"
        "\"iridescenceIor\":1.4,\"iridescenceThicknessMaximum\":500},\"KHR_materials_anisotropy\":{\"anisotropyStrength\":0.6,\"anisotropyRotation\":1.5707963}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL) != 0);
    const ArtfMaterialMore* m = &s->materialsMore.items[s->materialsMore.len - 1];
    CHECK(m->coatMap && m->coatMap == m->coatRoughnessMap && m->coatMap == m->coatNormalMap && m->coatMap == m->sheenMap && m->coatMap == m->sheenRoughnessMap);
    CHECK(m->sheenRoughness == .5f && m->specularColor[0] == .5f && m->specularColor[1] == .25f && m->specularColor[2] == .125f);
    CHECK(m->iridescence == .75f && fabsf(m->iridescenceIor - 1.4f) < 1e-6f && m->iridescenceThickness == 500.f && fabsf(m->anisotropy - .6f) < 1e-6f);
    CHECK(fabsf(m->anisotropyDirection[0]) < 1e-3f && fabsf(m->anisotropyDirection[1] - 1.f) < 1e-3f);
    CHECK(fabsf(m->absorb[0] - .5f) < 1e-3f && m->absorb[1] == 0 && fabsf(m->absorb[2] - 1.f) < 1e-3f);
    const uint32_t all = ARTF_MORE_COAT | ARTF_MORE_SHEEN | ARTF_MORE_SPECULAR | ARTF_MORE_ANISOTROPY | ARTF_MORE_IRIDESCENCE | ARTF_MORE_MAPS | ARTF_MORE_VOLUME;
    CHECK(m->flags == all && s->moreMaterials == 1 && s->layeredMaterials == 1);
    artfSceneFree(s);
}

void testGltfTargets(void) {
    static const char file[] = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":80,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAQAAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":3},{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3}],\"meshes\":[{\"weights\":[1],\"primitives\":[{\"attributes\":{\"POSITION\":1},\"indices\":0,\"targets\":[{\"POSITION\":2}]}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    artfScenePosesApply(s);
    /* the top went from 1 to 4, as the mesh's own weight says */
    ArtfVertex v[3];
    artfMeshUnpack(&s->meshes.items[s->instances.items[0].mesh], v);
    float top = 0;
    for (int i = 0; i < 3; ++i) top = artfMaxf(top, v[i].py);
    CHECK(fabsf(top - 4.f) < .02f);
    artfSceneFree(s);
}

/* A .glb made here: two triangles of a square, with colors, under a node that moves and scales it. */
static size_t makeGlb(uint8_t* out, const char* extra, bool breakIt) {
    static const float positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    char json[1400];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"children\":[1],\"translation\":[1,0,0]},{\"mesh\":0,\"scale\":[2,2,2]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,1,1]%s}],"
        "\"buffers\":[{\"byteLength\":60}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":%d}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}",
        extra, breakIt ? 4000 : 12);
    while (n & 3) json[n++] = ' ';
    memcpy(out, "glTF", 4);
    put32le(out + 4, 2);
    put32le(out + 8, (uint32_t)(12 + 8 + n + 8 + 60));
    put32le(out + 12, (uint32_t)n);
    put32le(out + 16, 0x4E4F534Au);
    memcpy(out + 20, json, (size_t)n);
    uint8_t* bin = out + 20 + n;
    put32le(bin, 60);
    put32le(bin + 4, 0x004E4942u);
    memcpy(bin + 8, positions, 48);
    memcpy(bin + 56, indices, 12);
    return (size_t)(20 + n + 8 + 60);
}

void testGltf(void) {
    static uint8_t glb[4096];
    size_t size = makeGlb(glb, "}", false);
    ArtfScene* s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1);
    {
        /* the same, made apart from the scene */
        ArtfPreparedModel* prepared = artfPrepareModelMemory(glb, size, NULL);
        CHECK(prepared != NULL);
        ArtfScene* other = artfSceneNew();
        const ArtfModel model = artfSceneAddPreparedModel(other, prepared, NULL);
        CHECK(model && artfModelInstanceCount(other, model) == 1);
        CHECK(other->meshes.len == s->meshes.len);
        artfSceneFree(other);
        artfPreparedModelFree(artfPrepareModelMemory(glb, size, NULL)); /* (or left behind) */
    }
    ArtfVec3 center;
    float radius;
    CHECK(artfSceneBounds(s, &center, &radius));
    CHECK(near_((int)(center.x * 100.f), 100, 2) && near_((int)(center.y * 100.f), 0, 2));
    /* the node's scale came through, and its parent's move: what is at the middle is the material's color */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, a);
    const size_t at = (size_t)(H / 2) * W + W / 2 + 36;
    CHECK(near_(CHANNEL(a[at], 16), (int)(sqrtf(.5f) * 255.f), 2) && near_(CHANNEL(a[at], 8), (int)(sqrtf(.25f) * 255.f), 2));
    artfSceneFree(s);

    /* what is wrong is left out, and nothing goes past what is there */
    artfSetLog(countLog, &logged);
    logged = 0;
    s = plainScene();
    size = makeGlb(glb, "}", true);
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1); /* (only its indices are gone: all vertices, no triangles) */
    artfSceneFree(s);
    s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, "{\"asset\":{\"version\":\"1.0\"}}", 28, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltfMemory(s, "{not json", 9, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltf(s, "no such file.glb", NULL) == 0);
    CHECK(logged >= 3);
    /* every cut of a good file, and every byte broken in turn: no crash, whatever comes out */
    size = makeGlb(glb, "}", false);
    for (size_t cut = 0; cut < size; cut += 7) artfSceneLoadGltfMemory(s, glb, cut, NULL, NULL);
    static uint8_t bad[4096];
    for (size_t i = 0; i < size; i += 3) {
        memcpy(bad, glb, size);
        bad[i] ^= 0x5A;
        artfSceneLoadGltfMemory(s, bad, size, NULL, NULL);
    }
    artfSceneFree(s);
    artfSetLog(NULL, NULL);

    /* an image: a PNG of 2 x 1 pixels, stored, one red and one half blue */
    static const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 20, 'I', 'D', 'A', 'T', 0x78, 1, 1, 9, 0, 0xF6, 0xFF, 0, 255, 0, 0, 255, 0, 0, 255, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0};
    int w = 0, h = 0;
    uint32_t* pixels = artfImageDecode(png, sizeof png, &w, &h);
    CHECK(pixels && w == 2 && h == 1);
    if (pixels) CHECK(pixels[0] == 0xFFFF0000u && pixels[1] == 0x800000FFu);
    artfFree(pixels);
    CHECK(!artfImageDecode("not a picture", 13, &w, &h));

    artfFree(a);
    artfRendererFree(r);
}
