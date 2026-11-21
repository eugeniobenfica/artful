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
        "\"sheenRoughnessTexture\":{\"index\":0}},\"KHR_materials_specular\":{\"specularFactor\":0.5,\"specularColorFactor\":[1,0.5,0.25],"
        "\"specularTexture\":{\"index\":0},\"specularColorTexture\":{\"index\":0}},"
        "\"KHR_materials_volume\":{\"attenuationColor\":[0.5,1,0.25],\"attenuationDistance\":2},\"KHR_materials_iridescence\":{\"iridescenceFactor\":0.75,"
        "\"iridescenceIor\":1.4,\"iridescenceThicknessMaximum\":500},\"KHR_materials_anisotropy\":{\"anisotropyStrength\":0.6,\"anisotropyRotation\":1.5707963}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL) != 0);
    const ArtfMaterialMore* m = &s->materialsMore.items[s->materialsMore.len - 1];
    CHECK(m->coatMap && m->coatMap == m->coatRoughnessMap && m->coatMap == m->coatNormalMap && m->coatMap == m->sheenMap && m->coatMap == m->sheenRoughnessMap);
    CHECK(m->specularMap == m->coatMap && m->specularColorMap == m->coatMap);
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
    CHECK(near_(CHANNEL(a[at], 16), shownByte(.5), 2) && near_(CHANNEL(a[at], 8), shownByte(.25), 2));
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

/* What fuzzing found (tools/fuzz): each, as libFuzzer made it smallest, read again under the sanitizers' build. A glTF
 * whose node bends a mesh whose primitives are not objects read past what had been prepared; a JPEG whose DC predictor
 * ran past an int; a light's profile whose count of tilt angles was any number; a KTX2 file of 64 bytes that said its
 * blocks took 4 GB, which were made before the file was asked whether it held them; a node that was twice its own child,
 * placed 2^256 times (now once, as a tree places each node). */
void testFuzzFindings(void) {
    static const uint8_t gltf[105] = {
        0x7B, 0x22, 0x61, 0x73, 0x73, 0x65, 0x74, 0x22, 0x3A, 0x7B, 0x22, 0x76, 0x65, 0x72, 0x73, 0x69,
        0x6F, 0x6E, 0x22, 0x3A, 0x22, 0x32, 0x22, 0x7D, 0x2C, 0x22, 0x6D, 0x65, 0x73, 0x68, 0x65, 0x73,
        0x22, 0x3A, 0x5B, 0x7B, 0x22, 0x70, 0x72, 0x69, 0x6D, 0x69, 0x74, 0x69, 0x76, 0x65, 0x73, 0x22,
        0x3A, 0x5B, 0x30, 0x2C, 0x32, 0x5D, 0x7D, 0x5D, 0x2C, 0x22, 0x6E, 0x6F, 0x64, 0x65, 0x73, 0x22,
        0x3A, 0x5B, 0x7B, 0x22, 0x6D, 0x65, 0x73, 0x68, 0x22, 0x3A, 0x30, 0x2C, 0x22, 0x73, 0x6B, 0x69,
        0x6E, 0x22, 0x3A, 0x30, 0x2C, 0x7D, 0x5D, 0x2C, 0x22, 0x73, 0x6B, 0x69, 0x6E, 0x73, 0x22, 0x3A,
        0x5B, 0x7B, 0x22, 0x22, 0x3A, 0x38, 0x7D, 0x5D, 0x7D};
    static const uint8_t jpeg[58] = {
        0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0xEE, 0xFF, 0x18, 0x01, 0x01, 0x11, 0x00, 0xFF,
        0xC4, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00, 0xFF, 0x10, 0x00, 0xFF, 0xD9, 0x00, 0x00, 0x00, 0x01, 0x03, 0x12, 0x21,
        0xFF, 0xDA, 0x00, 0x08, 0x01, 0x09, 0x83, 0x3F, 0xFF, 0xD9};
    static const uint8_t ies[16] = {
        0x54, 0x49, 0x4C, 0x54, 0x3D, 0x49, 0x4E, 0x43, 0x4C, 0x55, 0x44, 0x45, 0x0A, 0x31, 0x2D, 0x37};
    static const uint8_t ktx2[104] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A, 0x9D, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0xB0, 0x1D, 0x2D, 0xE2, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x6C, 0x6C, 0x01, 0x1F, 0xD8, 0x0A, 0x21, 0x2C, 0x1C, 0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x9C, 0xB0, 0x1D, 0x2D, 0xE2, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x9C, 0xB0, 0x1D, 0x2D, 0xE2};
    ArtfScene* s = artfSceneNew();
    artfSceneLoadModelMemory(s, gltf, sizeof gltf, NULL, NULL);
    int w = 0, h = 0, v = 0;
    artfFree(artfImageDecode(jpeg, sizeof jpeg, &w, &h));
    CHECK(artfIesDecode(ies, sizeof ies, &v, &h) == NULL);
    CHECK(artfSceneAddTextureKtx2(s, ktx2, sizeof ktx2) == 0);
    static const char cycle[] = "{\"asset\": {\"version\": \"20\"}, \"scenes\": [{\"nodes\": [0, 2]}], \"nodes\": [{\"n\": [0], \"children\": [0, 0]}]}";
    const double start = artfNowMs();
    artfSceneLoadModelMemory(s, cycle, sizeof cycle - 1, NULL, NULL);
    CHECK(artfNowMs() - start < 1000);
    /* (CI's fuzzing, image and glTF: a PNG whose palette is empty appended nothing to nothing, memcpy with a null
     * pointer) */
    static const uint8_t emptyPalette[45] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,                                                       /* signature */
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                                   /* IHDR, palette */
        0x00, 0x00, 0x00, 0x00, 0x50, 0x4C, 0x54, 0x45, 0x00, 0x00, 0x00, 0x00};                                /* PLTE, empty */
    artfFree(artfImageDecode(emptyPalette, sizeof emptyPalette, &w, &h));
    artfSceneFree(s);
}

/* ---- Basis Universal (KHR_texture_basisu) ---- */

/* etc1s.ktx2: basisu 1.60 of a 30 x 22 picture with alpha, with mipmaps; its reference RGBA32, hashed: 0xE16291DE */
static const uint8_t basisEtc1s[1203] = {
    171, 75, 84, 88, 32, 50, 48, 187, 13, 10, 26, 10, 0, 0, 0, 0, 1, 0, 0, 0, 30, 0, 0, 0, 22, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 1, 0, 0, 0, 200, 0, 0, 0, 60, 0, 0, 0, 4, 1, 0, 0, 36, 0, 0, 0, 40, 1,
    0, 0, 0, 0, 0, 0, 219, 2, 0, 0, 0, 0, 0, 0, 57, 4, 0, 0, 0, 0, 0, 0, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 24, 4, 0, 0, 0, 0, 0, 0, 33, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 4, 0, 0, 0, 0, 0, 0,
    13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 4, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 4, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0, 0,
    2, 0, 56, 0, 163, 1, 2, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 63, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255,
    255, 255, 64, 0, 63, 15, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 31, 0, 0, 0, 75, 84, 88, 119, 114, 105,
    116, 101, 114, 0, 66, 97, 115, 105, 115, 32, 85, 110, 105, 118, 101, 114, 115, 97, 108, 32, 49, 46, 54, 48,
    0, 0, 75, 0, 93, 0, 207, 0, 0, 0, 53, 1, 0, 0, 95, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0,
    62, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 17, 0, 0, 0, 17, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 6, 0, 0, 0, 6, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 32, 192, 35, 138, 1, 0, 2, 161, 79, 57, 248, 110, 104, 115,
    72, 10, 4, 120, 0, 64, 0, 66, 48, 240, 45, 99, 84, 199, 229, 18, 199, 26, 8, 240, 8, 98, 0, 4, 97, 88, 29, 81,
    15, 70, 33, 226, 36, 208, 61, 8, 192, 4, 0, 0, 0, 4, 65, 0, 68, 126, 233, 149, 171, 228, 107, 176, 60, 58, 23,
    70, 43, 234, 255, 4, 60, 100, 182, 97, 81, 83, 143, 154, 223, 77, 30, 115, 220, 68, 88, 74, 155, 106, 229, 177,
    185, 164, 47, 76, 10, 113, 210, 16, 167, 48, 251, 40, 215, 103, 86, 118, 162, 101, 113, 51, 34, 82, 146, 235,
    238, 203, 204, 50, 19, 201, 244, 222, 191, 136, 132, 164, 136, 120, 68, 212, 136, 48, 198, 208, 90, 211, 123,
    223, 24, 227, 83, 213, 118, 63, 142, 22, 143, 102, 218, 85, 127, 238, 252, 208, 112, 76, 229, 222, 163, 182,
    42, 130, 169, 1, 151, 158, 246, 184, 27, 46, 200, 156, 174, 52, 219, 182, 184, 229, 101, 99, 43, 184, 169, 81,
    211, 60, 111, 52, 120, 82, 237, 166, 132, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 226, 10, 72, 28, 65, 24, 128,
    191, 127, 110, 38, 236, 45, 11, 153, 187, 42, 133, 7, 38, 36, 253, 20, 87, 164, 9, 252, 202, 70, 8, 164, 216,
    75, 52, 129, 36, 4, 37, 47, 112, 69, 30, 96, 84, 68, 209, 70, 59, 11, 193, 1, 81, 206, 179, 80, 108, 180, 28,
    177, 177, 83, 123, 223, 65, 75, 177, 80, 120, 120, 120, 248, 162, 40, 138, 9, 0, 228, 149, 199, 2, 26, 209,
    220, 74, 62, 46, 187, 162, 93, 94, 224, 189, 214, 208, 7, 84, 85, 67, 8, 193, 214, 195, 121, 192, 123, 44, 89,
    3, 2, 182, 213, 17, 178, 199, 234, 240, 173, 239, 58, 91, 5, 16, 168, 176, 8, 157, 30, 84, 192, 192, 90, 219,
    56, 190, 243, 121, 121, 17, 242, 160, 142, 155, 11, 102, 90, 0, 13, 45, 81, 156, 194, 199, 14, 220, 3, 208,
    127, 79, 0, 44, 213, 127, 152, 16, 145, 30, 254, 79, 116, 50, 21, 45, 216, 26, 6, 141, 45, 246, 128, 23, 17,
    17, 215, 110, 183, 193, 205, 160, 154, 53, 199, 249, 181, 90, 222, 165, 148, 210, 206, 85, 127, 129, 217, 159,
    6, 129, 124, 27, 179, 176, 187, 49, 165, 52, 147, 65, 235, 178, 76, 9, 198, 0, 227, 50, 165, 180, 238, 189,
    143, 39, 95, 223, 204, 205, 253, 48, 60, 139, 156, 213, 93, 223, 243, 51, 117, 247, 155, 25, 241, 52, 230, 220,
    24, 99, 204, 232, 113, 229, 230, 75, 145, 155, 48, 185, 58, 221, 57, 14, 225, 122, 232, 92, 211, 148, 101, 89,
    206, 142, 90, 60, 84, 92, 66, 230, 96, 255, 211, 197, 239, 194, 252, 58, 124, 221, 152, 163, 173, 205, 15, 119,
    75, 167, 221, 237, 191, 255, 34, 0, 193, 156, 128, 1, 0, 132, 129, 0, 40, 13, 105, 149, 136, 132, 31, 25, 25,
    8, 233, 4, 183, 4, 196, 21, 16, 132, 97, 32, 128, 250, 46, 57, 146, 15, 112, 41, 56, 136, 84, 16, 116, 87, 65,
    23, 145, 150, 123, 154, 64, 163, 201, 33, 24, 10, 7, 113, 130, 68, 16, 137, 45, 98, 25, 216, 226, 36, 117, 128,
    18, 251, 87, 49, 168, 89, 181, 127, 64, 251, 250, 74, 107, 104, 42, 0, 152, 0, 0, 0, 0, 0, 0, 64, 0, 1, 174,
    115, 86, 2, 78, 30, 86, 21, 30, 147, 60, 82, 33, 7, 150, 139, 167, 67, 185, 121, 1, 117, 79, 210, 76, 15, 166,
    31, 215, 178, 52, 210, 47, 18, 58, 92, 49, 0, 150, 117, 100, 226, 148, 110, 107, 64, 46, 70, 189, 247, 104,
    255, 206, 198, 126, 103, 247, 81, 99, 138, 110, 99, 91, 155, 135, 111, 66, 123, 232, 189, 66, 155, 5, 225, 88,
    5, 66, 252, 248, 38, 53, 45, 121, 94, 194, 236, 112, 18, 160, 8, 2, 129, 49, 195, 19, 70, 118, 193, 41, 53,
    28, 252, 178, 243, 31, 189, 22, 243, 234, 87, 92, 211, 127, 240, 244, 1, 206, 230, 0, 73, 180, 23, 161, 120,
    101, 181, 231, 104, 219, 88, 139, 78, 39, 221, 42, 206, 119, 23, 147, 53, 121, 232, 215, 78, 225, 120, 191,
    253, 153, 128, 164, 26, 45, 85, 84, 61, 69, 37, 187, 97, 74, 33, 165, 127, 148, 135, 143, 201, 21, 40, 147,
    3, 190, 73, 11, 0,
};
/* uastc.ktx2: basisu 1.60 of a 30 x 22 picture with alpha, with mipmaps; its reference RGBA32, hashed: 0x4545B383 */
static const uint8_t basisUastc[1382] = {
    171, 75, 84, 88, 32, 50, 48, 187, 13, 10, 26, 10, 0, 0, 0, 0, 1, 0, 0, 0, 30, 0, 0, 0, 22, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 2, 0, 0, 0, 200, 0, 0, 0, 44, 0, 0, 0, 244, 0, 0, 0, 36, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 92, 2, 0, 0, 0, 0, 0, 0, 10, 3, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0,
    0, 0, 0, 147, 1, 0, 0, 0, 0, 0, 0, 201, 0, 0, 0, 0, 0, 0, 0, 192, 0, 0, 0, 0, 0, 0, 0, 74, 1, 0, 0, 0, 0, 0,
    0, 73, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 49, 1, 0, 0, 0, 0, 0, 0, 25, 0, 0, 0, 0, 0, 0, 0, 16, 0,
    0, 0, 0, 0, 0, 0, 24, 1, 0, 0, 0, 0, 0, 0, 25, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 44, 0, 0, 0, 0,
    0, 0, 0, 2, 0, 40, 0, 166, 1, 2, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 127, 3, 0, 0, 0, 0, 0, 0, 0, 0,
    255, 255, 255, 255, 31, 0, 0, 0, 75, 84, 88, 119, 114, 105, 116, 101, 114, 0, 66, 97, 115, 105, 115, 32, 85,
    110, 105, 118, 101, 114, 115, 97, 108, 32, 49, 46, 54, 48, 0, 0, 40, 181, 47, 253, 32, 16, 129, 0, 0, 55, 83,
    20, 51, 183, 126, 240, 1, 0, 0, 0, 0, 0, 0, 0, 0, 40, 181, 47, 253, 32, 16, 129, 0, 0, 0, 193, 209, 182, 233,
    109, 83, 236, 89, 119, 152, 187, 152, 187, 152, 187, 40, 181, 47, 253, 32, 64, 1, 2, 0, 120, 182, 236, 141,
    168, 158, 60, 83, 165, 147, 103, 114, 47, 58, 36, 27, 143, 216, 126, 211, 184, 21, 112, 59, 153, 223, 136, 150,
    95, 93, 129, 120, 15, 152, 132, 149, 43, 251, 186, 221, 229, 142, 187, 19, 81, 80, 80, 80, 15, 18, 100, 5, 148,
    85, 187, 117, 251, 103, 219, 253, 61, 46, 46, 46, 40, 181, 47, 253, 32, 192, 1, 6, 0, 192, 52, 108, 148, 145,
    21, 16, 226, 0, 245, 0, 248, 64, 189, 179, 95, 68, 148, 250, 3, 170, 15, 152, 133, 7, 253, 1, 253, 0, 254, 220,
    35, 192, 181, 42, 100, 117, 202, 32, 125, 240, 127, 244, 63, 248, 59, 206, 20, 192, 131, 105, 250, 206, 192,
    136, 118, 148, 221, 128, 204, 213, 221, 251, 187, 160, 150, 203, 217, 16, 13, 120, 7, 202, 40, 200, 36, 251,
    2, 191, 67, 228, 132, 122, 239, 185, 14, 252, 83, 104, 223, 117, 206, 254, 69, 255, 69, 176, 84, 75, 72, 236,
    6, 180, 199, 119, 191, 177, 111, 199, 76, 203, 8, 79, 74, 5, 211, 218, 29, 84, 85, 251, 93, 238, 254, 161, 244,
    252, 86, 132, 21, 27, 229, 162, 72, 212, 72, 104, 215, 253, 78, 254, 78, 254, 78, 180, 197, 219, 8, 41, 195,
    212, 132, 17, 187, 168, 35, 185, 19, 185, 19, 196, 194, 90, 225, 118, 194, 212, 112, 200, 254, 92, 119, 108,
    103, 108, 103, 15, 186, 11, 227, 220, 21, 152, 85, 219, 163, 221, 255, 95, 80, 122, 122, 40, 181, 47, 253, 96,
    0, 2, 1, 24, 0, 144, 237, 208, 36, 128, 124, 0, 33, 34, 51, 117, 103, 186, 155, 254, 206, 15, 40, 129, 10, 100,
    52, 64, 168, 134, 128, 66, 254, 153, 164, 70, 111, 49, 196, 201, 16, 100, 173, 129, 80, 96, 252, 147, 254, 181,
    239, 216, 223, 10, 39, 145, 10, 249, 7, 64, 216, 81, 234, 114, 252, 148, 253, 182, 239, 79, 184, 131, 7, 48,
    33, 65, 14, 83, 241, 65, 254, 69, 204, 244, 118, 33, 99, 15, 242, 220, 2, 193, 96, 16, 165, 48, 200, 81, 234,
    114, 252, 202, 216, 162, 189, 106, 96, 64, 122, 3, 148, 16, 182, 48, 216, 81, 250, 72, 233, 72, 33, 68, 92,
    0, 237, 202, 204, 217, 221, 174, 170, 127, 119, 79, 196, 97, 217, 64, 94, 170, 254, 65, 156, 102, 136, 144,
    208, 114, 114, 68, 5, 91, 91, 16, 202, 25, 106, 0, 220, 0, 237, 204, 49, 220, 50, 15, 70, 143, 216, 200, 158,
    170, 204, 170, 242, 103, 254, 127, 46, 72, 104, 143, 182, 143, 215, 14, 1, 171, 254, 239, 12, 102, 238, 184,
    125, 73, 104, 116, 148, 26, 201, 124, 115, 32, 126, 32, 255, 33, 255, 254, 35, 254, 19, 15, 152, 102, 215, 116,
    15, 168, 48, 149, 55, 102, 254, 245, 249, 220, 94, 79, 40, 111, 213, 216, 235, 169, 254, 217, 199, 103, 82,
    229, 248, 252, 124, 207, 70, 13, 211, 252, 153, 168, 254, 253, 63, 103, 84, 169, 253, 212, 126, 33, 148, 8,
    4, 80, 38, 128, 230, 97, 251, 131, 253, 181, 239, 216, 223, 79, 42, 143, 9, 100, 98, 13, 255, 103, 244, 237,
    170, 68, 236, 116, 127, 194, 38, 49, 26, 145, 3, 216, 149, 49, 199, 65, 233, 114, 251, 148, 253, 33, 227, 215,
    131, 238, 129, 193, 246, 17, 165, 32, 199, 65, 233, 114, 251, 15, 178, 6, 4, 48, 159, 13, 255, 83, 119, 12,
    17, 35, 194, 236, 118, 74, 40, 129, 179, 242, 224, 220, 191, 7, 113, 2, 147, 16, 198, 48, 248, 91, 183, 237,
    10, 87, 18, 31, 248, 55, 35, 118, 87, 187, 154, 255, 206, 211, 21, 221, 106, 248, 242, 38, 33, 236, 196, 176,
    169, 253, 127, 127, 0, 64, 236, 48, 126, 129, 192, 118, 34, 6, 16, 69, 100, 136, 169, 204, 253, 143, 55, 133,
    10, 68, 240, 81, 169, 134, 30, 115, 255, 169, 38, 207, 111, 19, 39, 221, 136, 60, 202, 135, 36, 90, 242, 248,
    249, 249, 188, 22, 0, 10, 129, 177, 215, 216, 45, 122, 217, 113, 253, 147, 254, 198, 239, 233, 207, 79, 56,
    143, 7, 48, 123, 114, 15, 83, 9, 80, 255, 69, 236, 116, 127, 33, 228, 123, 41, 237, 132, 96, 94, 49, 216, 97,
    235, 131, 252, 165, 254, 202, 214, 178, 160, 106, 2, 122, 123, 33, 182, 48, 216, 81, 234, 115, 252, 76, 234,
    144, 85, 68, 128, 118, 238, 2, 0, 84, 85, 168, 170, 253, 255, 79, 214, 108, 217, 32, 198, 187, 255, 65, 128,
    153, 137, 228, 228, 114, 122, 100, 4, 251, 38, 16, 14, 92, 105, 0, 237, 0, 237, 220, 50, 220, 50, 79, 196, 131,
    216, 200, 130, 187, 205, 200, 34, 153, 255, 145, 208, 104, 114, 79, 66, 143, 215, 12, 157, 186, 255, 15, 31,
    153, 239, 130, 209, 72, 104, 148, 147, 58, 14, 109, 63, 84, 125, 52, 239, 50, 239, 254, 18, 255, 2, 207, 55,
    128, 214, 118, 243, 187, 19, 149, 239, 153, 255, 249, 185, 63, 23, 15, 42, 109, 213, 186, 13, 186, 255, 217,
    23, 152, 83, 249, 249, 220, 62, 143, 215, 13, 211, 252, 175, 187, 255, 253, 39, 151, 85, 169, 253, 212, 126,
    19, 6, 11, 210, 7, 11, 2, 224, 18, 32, 73, 184, 238, 238, 110, 0, 15, 22, 139, 9, 100, 184, 254, 255, 103, 4,
    252, 171, 200, 246, 246, 118, 130, 36, 161, 245, 144, 37, 34, 148, 81, 234, 114, 252, 114, 252, 114, 252, 49,
    162, 189, 145, 254, 11, 99, 8, 48, 216, 81, 251, 81, 251, 81, 251, 15, 52, 11, 4, 48, 97, 254, 255, 83, 173,
    255, 17, 73, 110, 110, 110, 2, 38, 97, 149, 242, 66, 35, 190, 19, 197, 32, 248, 32, 248, 32, 248, 19, 196, 228,
    32, 188, 212, 96, 129, 147, 5, 79, 210, 104, 104, 104, 0, 19, 151, 43, 150, 143, 207, 34, 73, 146, 164, 75,
    172, 255, 255, 127, 0,
};

/* (each level's pixels, hashed: FNV-1a of the packed values) */
static uint32_t basisHash(const uint32_t* pixels, size_t count) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; ++i) h = (h ^ pixels[i]) * 16777619u;
    return h;
}

/* UASTC's 19 modes as ASTC's: of each, blocks of random bits after its code; every one UASTC reads gives the same 16
 * texels as ASTC 4 x 4. */
void testBasisAstc(void) {
    static const uint8_t codes[19][2] = {{0x1, 4}, {0x35, 6}, {0x1D, 5}, {0x3, 5}, {0x13, 5}, {0xB, 5}, {0x1B, 5}, {0x7, 5}, {0x17, 5}, {0xF, 5},
                                         {0x2, 3}, {0x0, 2}, {0x6, 3}, {0x1F, 5}, {0xD, 5}, {0x5, 7}, {0x15, 6}, {0x25, 6}, {0x9, 4}};
    uint32_t state = 12345u, read = 0, made = 0, apart = 0, modesRead = 0;
    for (int mode = 0; mode < 19; ++mode) {
        uint32_t readHere = 0;
        for (int n = 0; n < 4000; ++n) {
            uint8_t block[16], astc[16];
            for (int i = 0; i < 16; ++i) {
                state = state * 1664525u + 1013904223u;
                block[i] = (uint8_t)(state >> 24);
            }
            block[0] = (uint8_t)((block[0] & ~((1u << codes[mode][1]) - 1)) | codes[mode][0]);
            uint32_t direct[16], back[16];
            if (!artfBasisUastc(block, 16, 4, 4, direct)) continue;
            ++read, ++readHere;
            if (!artfBasisAstc(block, 16, astc)) continue;
            ++made;
            artfDecodeAstc(astc, 4, 4, back);
            apart += memcmp(direct, back, sizeof direct) != 0;
        }
        modesRead += readHere > 0;
    }
    printf("UASTC as ASTC: %u blocks read in %u modes, %u made, %u with texels apart\n", read, modesRead, made, apart);
    CHECK(modesRead == 19 && read > 30000 && made == read && apart == 0);
}

/* KTX2 files of Basis Universal's, ETC1S (with alpha's own slices) and UASTC with Zstandard: every level as the
 * reference transcoder makes it, to the bit; a texture with its own mipmaps; and what is cut short or spoiled refused,
 * or read, without reading past it. */
void testBasis(void) {
    static const struct { const uint8_t* data; size_t size; uint32_t hash; } files[2] = {{basisEtc1s, sizeof basisEtc1s, 0xE16291DEu}, {basisUastc, sizeof basisUastc, 0x4545B383u}};
    for (int f = 0; f < 2; ++f) {
        uint32_t* pixels = NULL;
        uint32_t offsets[16];
        CHECK(artfBasisLevels(files[f].data, files[f].size, &pixels, offsets));
        if (pixels) CHECK(basisHash(pixels, 867) == files[f].hash); /* (30 x 22, 15 x 11, 7 x 5, 3 x 2, 1 x 1) */
        CHECK(offsets[4] == 866);
        artfFree(pixels);
        /* as blocks a GPU takes: UASTC as ASTC 4 x 4, ETC1S (its alpha's slices left out: ETC1 has none) as ETC1; the
         * blocks turned back into pixels are the same, to the bit */
        uint8_t* plain = artfMalloc(files[f].size);
        memcpy(plain, files[f].data, files[f].size);
        if (!f) {
            const uint32_t sgd = (uint32_t)plain[64] | (uint32_t)plain[65] << 8 | (uint32_t)plain[66] << 16 | (uint32_t)plain[67] << 24;
            for (int level = 0; level < 5; ++level) memset(plain + sgd + 20 + level * 20 + 12, 0, 8); /* (alpha's offset and length) */
        }
        uint32_t* direct = NULL;
        uint32_t directOffsets[16];
        CHECK(artfBasisLevels(plain, files[f].size, &direct, directOffsets));
        ArtfTextureData blocks;
        memset(&blocks, 0, sizeof blocks);
        blocks.width = 30, blocks.height = 22, blocks.mipCount = 5;
        CHECK(artfBasisBlocks(plain, files[f].size, &blocks.format, &blocks.blocks, &blocks.blockBytes, blocks.blockOffsets));
        CHECK(blocks.format == (f ? 157u : 147u) && blocks.blockBytes == (size_t)(48 + 12 + 4 + 1 + 1) * (f ? 16 : 8));
        if (blocks.blocks && direct) {
            artfTextureDecode(&blocks);
            size_t apart = 0;
            for (int i = 0; i < 867; ++i) apart += blocks.pixels[i] != direct[i];
            printf("Basis %s as %s blocks: %zu of 867 texels apart\n", f ? "UASTC" : "ETC1S", f ? "ASTC" : "ETC1", apart);
            CHECK(apart == 0);
        }
        artfFree(blocks.blocks);
        artfFree(blocks.pixels);
        artfFree(direct);
        if (!f) CHECK(!artfBasisBlocks(files[f].data, files[f].size, &blocks.format, &blocks.blocks, &blocks.blockBytes, blocks.blockOffsets)); /* (with alpha: pixels) */
        /* in a scene: UASTC's blocks kept, ETC1S with alpha as pixels */
        ArtfScene* s = artfSceneNew();
        const ArtfTexture t = artfSceneAddTextureKtx2(s, files[f].data, files[f].size);
        CHECK(t != 0);
        if (t) CHECK(s->textures.items[t].mipCount == 5 && s->textures.items[t].format == (f ? 157u : 0u) && (f ? s->textures.items[t].blocks != NULL : s->textures.items[t].pixels != NULL));
        artfSceneFree(s);
        s = artfSceneNew();
        const ArtfTexture u = artfSceneAddTextureKtx2(s, plain, files[f].size);
        if (u) CHECK(s->textures.items[u].format == (f ? 157u : 147u));
        artfSceneFree(s);
        artfFree(plain);
        /* every block a byte spoiled: turned or refused, never past the block */
        uint8_t* spoiled = artfMalloc(files[f].size);
        for (size_t i = 0; i < files[f].size; i += 3) {
            memcpy(spoiled, files[f].data, files[f].size);
            spoiled[i] ^= 0xA5;
            uint8_t* out = NULL;
            uint32_t format, offs[16];
            size_t bytes;
            if (artfBasisBlocks(spoiled, files[f].size, &format, &out, &bytes, offs)) artfFree(out);
        }
        artfFree(spoiled);
        uint8_t* copy = artfMalloc(files[f].size);
        int refused = 0;
        for (size_t cut = 0; cut < files[f].size; ++cut) {
            refused += !artfBasisLevels(files[f].data, cut, &pixels, offsets);
            artfFree(pixels);
        }
        CHECK(refused == (int)files[f].size);
        for (size_t i = 0; i < files[f].size; ++i) {
            memcpy(copy, files[f].data, files[f].size);
            copy[i] ^= 0x5A;
            if (artfBasisLevels(copy, files[f].size, &pixels, offsets)) artfFree(pixels);
        }
        artfFree(copy);
    }
}
