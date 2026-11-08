/* artful — glTF 2.0, with a JSON reader of its own. Numbers in a file are not to be trusted: every index
 * and every length is checked before it is used, and what is wrong is logged and left out. */
#include "artful/gltf.h"
#include "scene/sceneInternal.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- what stays of a file, to play its clips ---- */

typedef struct ModelNode {
    int32_t parent;
    float t[3], r[4], s[3];    /* as the file has it */
    float pose[10];            /* as a clip has it now: t, r, s */
    bool fixed;                /* the file gave a matrix: clips do not touch it */
    ArtfMat4 local, world;
    float* weights;            /* of its mesh's targets, as the file says */
    float* weightPose;         /* as a clip has them now */
    uint32_t weightCount;
} ModelNode;

typedef struct ModelSkin {
    uint32_t* joints;          /* nodes */
    float* inverses;           /* sixteen floats each, by columns */
    uint32_t count;
} ModelSkin;

typedef struct ModelPart { ArtfInstance instance; uint32_t node; int32_t skin; } ModelPart;

enum { PATH_T, PATH_R, PATH_S, PATH_W };
enum { BLEND_LINEAR, BLEND_STEP, BLEND_CUBIC };

typedef struct ModelChannel {
    uint32_t node, keys;
    int path, blend, width;
    float* times;
    float* values;
} ModelChannel;

typedef struct ModelClip {
    char name[64];
    float duration;
    ModelChannel* channels;
    uint32_t channelCount;
} ModelClip;

typedef struct ModelData {
    ModelNode* nodes;
    uint32_t nodeCount;
    ModelSkin* skins;
    uint32_t skinCount;
    ARTF_VEC(ModelPart) parts;
    ModelClip* clips;
    uint32_t clipCount;
    ArtfMat4 base;
} ModelData;

typedef ARTF_VEC(ModelData*) ModelList;

/* ---- JSON: a tree in one array ---- */

typedef enum Kind { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } Kind;

typedef struct Json {
    Kind kind;
    const char* text;   /* a string, or an object member's key */
    uint32_t length;    /* of the string */
    const char* key;
    uint32_t keyLength;
    double number;
    uint32_t first, next; /* first child, next sibling: index + 1 */
    uint32_t count;       /* children */
} Json;

typedef struct Reader {
    const char* at;
    const char* end;
    ARTF_VEC(Json) nodes;
    int depth;
} Reader;

static void skipSpace(Reader* r) {
    while (r->at < r->end && (*r->at == ' ' || *r->at == '\n' || *r->at == '\r' || *r->at == '\t')) ++r->at;
}

static bool readString(Reader* r, const char** text, uint32_t* length) {
    if (r->at >= r->end || *r->at != '"') return false;
    const char* start = ++r->at;
    while (r->at < r->end && *r->at != '"') r->at += *r->at == '\\' ? 2 : 1;
    if (r->at >= r->end) return false;
    *text = start;
    *length = (uint32_t)(r->at - start);
    ++r->at;
    return true;
}

/* The node's index + 1; 0: the text is not JSON. */
static uint32_t readValue(Reader* r) {
    skipSpace(r);
    if (r->at >= r->end || ++r->depth > 200) return 0;
    Json j;
    memset(&j, 0, sizeof j);
    const uint32_t index = (uint32_t)r->nodes.len;
    artfVecPush(&r->nodes, j);
    const char c = *r->at;
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        const char close = object ? '}' : ']';
        ++r->at;
        uint32_t last = 0, count = 0;
        for (;;) {
            skipSpace(r);
            if (r->at < r->end && *r->at == close) { ++r->at; break; }
            const char* key = NULL;
            uint32_t keyLength = 0;
            if (object) {
                if (!readString(r, &key, &keyLength)) return 0;
                skipSpace(r);
                if (r->at >= r->end || *r->at++ != ':') return 0;
            }
            const uint32_t child = readValue(r);
            if (!child) return 0;
            r->nodes.items[child - 1].key = key;
            r->nodes.items[child - 1].keyLength = keyLength;
            if (last) r->nodes.items[last - 1].next = child;
            else r->nodes.items[index].first = child;
            last = child;
            ++count;
            skipSpace(r);
            if (r->at < r->end && *r->at == ',') ++r->at;
            else if (r->at >= r->end || *r->at != close) return 0;
        }
        r->nodes.items[index].kind = object ? J_OBJECT : J_ARRAY;
        r->nodes.items[index].count = count;
    } else if (c == '"') {
        const char* text;
        uint32_t length;
        if (!readString(r, &text, &length)) return 0;
        r->nodes.items[index].kind = J_STRING;
        r->nodes.items[index].text = text;
        r->nodes.items[index].length = length;
    } else if (r->end - r->at >= 4 && !memcmp(r->at, "true", 4)) {
        r->nodes.items[index].kind = J_BOOL;
        r->nodes.items[index].number = 1;
        r->at += 4;
    } else if (r->end - r->at >= 5 && !memcmp(r->at, "false", 5)) {
        r->nodes.items[index].kind = J_BOOL;
        r->at += 5;
    } else if (r->end - r->at >= 4 && !memcmp(r->at, "null", 4)) {
        r->at += 4;
    } else {
        char number[64];
        size_t n = 0;
        while (r->at < r->end && n + 1 < sizeof number && strchr("+-.0123456789eE", *r->at)) number[n++] = *r->at++;
        if (!n) return 0;
        number[n] = 0;
        r->nodes.items[index].kind = J_NUMBER;
        r->nodes.items[index].number = strtod(number, NULL);
    }
    --r->depth;
    return index + 1;
}

typedef struct Loader {
    ArtfScene* scene;
    Reader json;
    const char* folder;
    const uint8_t* binary;  /* a .glb's own chunk */
    size_t binarySize;
    ARTF_VEC(uint8_t*) owned;
    uint8_t** buffers;
    size_t* bufferSizes;
    uint32_t bufferCount;
    ArtfTexture* images;    /* of each image, once it was needed; 0: not yet */
    ArtfPreparedTexture** preparedImages; /* what artfPrepareModel made, until it is taken */
    ArtfPreparedMesh** preparedMeshes;
    bool preparing;         /* no scene yet */
    ArtfMaterial* materials;
    uint32_t materialCount, imageCount;
    ArtfMesh* primitives;   /* of each mesh's primitives, from primitiveStart */
    uint32_t* primitiveStart;
    uint32_t primitiveTotal; /* (those that bend come after: a node may use a mesh without its skin) */
    uint32_t instances;
    ModelData* model;
    const Json *root, *accessors, *views, *meshes, *nodes, *textures, *imageList;
} Loader;

static const Json* nodeAt(const Loader* l, uint32_t index) { return index ? &l->json.nodes.items[index - 1] : NULL; }

static const Json* member(const Loader* l, const Json* object, const char* key) {
    if (!object || object->kind != J_OBJECT) return NULL;
    const size_t length = strlen(key);
    for (const Json* j = nodeAt(l, object->first); j; j = nodeAt(l, j->next))
        if (j->keyLength == length && !memcmp(j->key, key, length)) return j;
    return NULL;
}

static const Json* element(const Loader* l, const Json* array, uint32_t index) {
    if (!array || array->kind != J_ARRAY || index >= array->count) return NULL;
    const Json* j = nodeAt(l, array->first);
    while (index-- && j) j = nodeAt(l, j->next);
    return j;
}

static double numberOf(const Loader* l, const Json* object, const char* key, double otherwise) {
    const Json* j = member(l, object, key);
    return j && (j->kind == J_NUMBER || j->kind == J_BOOL) ? j->number : otherwise;
}

/* An index in a file: -1 when there is none. */
static int64_t indexOf(const Loader* l, const Json* object, const char* key) {
    const Json* j = member(l, object, key);
    return j && j->kind == J_NUMBER && j->number >= 0 && j->number < 4e9 ? (int64_t)j->number : -1;
}

static bool stringIs(const Json* j, const char* text) { return j && j->kind == J_STRING && j->length == strlen(text) && !memcmp(j->text, text, j->length); }

static void readNumbers(const Loader* l, const Json* array, float* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const Json* j = element(l, array, i);
        if (j && j->kind == J_NUMBER) out[i] = (float)j->number;
    }
}

/* ---- bytes ---- */

static uint8_t* readFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = NULL;
    if (length >= 0) {
        data = artfMalloc((size_t)length + 1);
        *size = fread(data, 1, (size_t)length, f);
    }
    fclose(f);
    return data;
}

static int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* What a uri stands for: inline data or a file beside the model. The caller owns it. */
static uint8_t* readUri(const Loader* l, const Json* uri, size_t* size) {
    if (uri->length > 5 && !memcmp(uri->text, "data:", 5)) {
        const char* comma = memchr(uri->text, ',', uri->length);
        if (!comma) return NULL;
        const char* p = comma + 1;
        const char* end = uri->text + uri->length;
        uint8_t* out = artfMalloc((size_t)(end - p) / 4 * 3 + 4);
        size_t n = 0;
        uint32_t bits = 0;
        int have = 0;
        for (; p < end; ++p) {
            const int v = base64Value(*p);
            if (v < 0) continue;
            bits = bits << 6 | (uint32_t)v;
            have += 6;
            if (have >= 8) { have -= 8; out[n++] = (uint8_t)(bits >> have); }
        }
        *size = n;
        return out;
    }
    if (!l->folder) return NULL;
    /* (a file's name stays where the model is: no way out of the folder) */
    char path[1024];
    char name[512];
    size_t n = 0;
    for (uint32_t i = 0; i < uri->length && n + 1 < sizeof name; ++i) {
        const char c = uri->text[i];
        if (c == '%' && i + 2 < uri->length) {
            char hex[3] = {uri->text[i + 1], uri->text[i + 2], 0};
            name[n++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            name[n++] = c;
        }
    }
    name[n] = 0;
    if (strstr(name, "..") || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) {
        artfLog("artful: glTF: a file outside the model's folder: %s", name);
        return NULL;
    }
    snprintf(path, sizeof path, "%s/%s", l->folder, name);
    uint8_t* data = readFile(path, size);
    if (!data) artfLog("artful: glTF: cannot read %s", path);
    return data;
}

static bool loadBuffers(Loader* l) {
    const Json* list = member(l, l->root, "buffers");
    l->bufferCount = list && list->kind == J_ARRAY ? list->count : 0;
    l->buffers = artfCalloc(l->bufferCount + 1, sizeof *l->buffers);
    l->bufferSizes = artfCalloc(l->bufferCount + 1, sizeof *l->bufferSizes);
    for (uint32_t i = 0; i < l->bufferCount; ++i) {
        const Json* b = element(l, list, i);
        const Json* uri = member(l, b, "uri");
        if (uri && uri->kind == J_STRING) {
            size_t size = 0;
            uint8_t* data = readUri(l, uri, &size);
            if (!data) return false;
            artfVecPush(&l->owned, data);
            l->buffers[i] = data;
            l->bufferSizes[i] = size;
        } else if (!i && l->binary) {
            l->buffers[0] = (uint8_t*)l->binary;
            l->bufferSizes[0] = l->binarySize;
        }
    }
    return true;
}

/* The bytes of a buffer view; NULL if they are not all there. */
static const uint8_t* viewBytes(const Loader* l, int64_t view, size_t offset, size_t need, size_t* stride) {
    const Json* v = element(l, l->views, view < 0 ? UINT32_MAX : (uint32_t)view);
    if (!v) return NULL;
    const int64_t buffer = indexOf(l, v, "buffer");
    if (buffer < 0 || buffer >= l->bufferCount || !l->buffers[buffer]) return NULL;
    const size_t start = (size_t)numberOf(l, v, "byteOffset", 0), length = (size_t)numberOf(l, v, "byteLength", 0);
    if (start > l->bufferSizes[buffer] || length > l->bufferSizes[buffer] - start || offset > length || need > length - offset) return NULL;
    if (stride) *stride = (size_t)numberOf(l, v, "byteStride", 0);
    return l->buffers[buffer] + start + offset;
}

/* An accessor's values, once its sparse changes are made: held by the loader. */
static uint8_t* ownedCopy(const Loader* l, size_t size) {
    uint8_t* copy = artfCalloc(1, size ? size : 1);
    artfVecPush(&((Loader*)l)->owned, copy);
    return copy;
}

typedef struct Accessor {
    const uint8_t* data;
    size_t stride;
    uint32_t count, components, type, size;
    bool normalized;
} Accessor;

static bool accessorOf(const Loader* l, int64_t index, Accessor* a) {
    const Json* j = element(l, l->accessors, index < 0 ? UINT32_MAX : (uint32_t)index);
    if (!j) return false;
    const Json* sparse = member(l, j, "sparse");
    const Json* type = member(l, j, "type");
    static const struct { const char* name; uint32_t components; } types[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
    a->components = 0;
    for (size_t i = 0; i < ARTF_COUNT(types); ++i)
        if (stringIs(type, types[i].name)) a->components = types[i].components;
    a->type = (uint32_t)numberOf(l, j, "componentType", 0);
    a->size = a->type == 5120 || a->type == 5121 ? 1 : a->type == 5122 || a->type == 5123 ? 2 : a->type == 5125 || a->type == 5126 ? 4 : 0;
    a->count = (uint32_t)numberOf(l, j, "count", 0);
    a->normalized = numberOf(l, j, "normalized", 0) != 0;
    if (!a->components || !a->size || !a->count) return false;
    const size_t element_ = (size_t)a->size * a->components;
    size_t stride = 0;
    const bool hasView = member(l, j, "bufferView") != NULL;
    if (hasView) {
        /* (the whole range is checked with the stride known, which needs the view first) */
        const uint8_t* start = viewBytes(l, indexOf(l, j, "bufferView"), (size_t)numberOf(l, j, "byteOffset", 0), element_, &stride);
        if (!start) return false;
        a->stride = stride ? stride : element_;
        if (a->stride < element_) return false;
        a->data = viewBytes(l, indexOf(l, j, "bufferView"), (size_t)numberOf(l, j, "byteOffset", 0), a->stride * (a->count - 1) + element_, NULL);
        if (!a->data) return false;
    } else if (!sparse) {
        return false;
    }
    if (!sparse) return true;

    /* zeros, or what the view holds, with some elements replaced */
    if (a->count > (1u << 28)) return false;
    uint8_t* copy = ownedCopy(l, element_ * a->count);
    if (hasView)
        for (uint32_t i = 0; i < a->count; ++i) memcpy(copy + element_ * i, a->data + a->stride * i, element_);
    a->data = copy;
    a->stride = element_;
    const Json* indices = member(l, sparse, "indices");
    const Json* values = member(l, sparse, "values");
    const uint32_t changes = (uint32_t)numberOf(l, sparse, "count", 0);
    const uint32_t indexType = (uint32_t)numberOf(l, indices, "componentType", 0);
    const size_t indexSize = indexType == 5121 ? 1 : indexType == 5123 ? 2 : indexType == 5125 ? 4 : 0;
    if (!changes) return true;
    if (!indices || !values || !indexSize || changes > a->count) return false;
    const uint8_t* at = viewBytes(l, indexOf(l, indices, "bufferView"), (size_t)numberOf(l, indices, "byteOffset", 0), indexSize * changes, NULL);
    const uint8_t* from = viewBytes(l, indexOf(l, values, "bufferView"), (size_t)numberOf(l, values, "byteOffset", 0), element_ * changes, NULL);
    if (!at || !from) return false;
    for (uint32_t i = 0; i < changes; ++i) {
        uint32_t index = 0;
        memcpy(&index, at + indexSize * i, indexSize); /* (little-endian, as glTF and all that Artful runs on) */
        if (index < a->count) memcpy(copy + element_ * index, from + element_ * i, element_);
    }
    return true;
}

/* An accessor of 4 x 4 matrices of floats. */
static bool accessorMatrices(const Loader* l, int64_t index, Accessor* a) {
    const Json* j = element(l, l->accessors, index < 0 ? UINT32_MAX : (uint32_t)index);
    if (!j || !stringIs(member(l, j, "type"), "MAT4") || (uint32_t)numberOf(l, j, "componentType", 0) != 5126) return false;
    a->count = (uint32_t)numberOf(l, j, "count", 0);
    a->stride = 64;
    a->data = a->count ? viewBytes(l, indexOf(l, j, "bufferView"), (size_t)numberOf(l, j, "byteOffset", 0), (size_t)a->count * 64, NULL) : NULL;
    return a->data != NULL;
}

static float componentOf(const Accessor* a, uint32_t index, uint32_t component) {
    const uint8_t* p = a->data + a->stride * index + (size_t)a->size * component;
    switch (a->type) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5121: return a->normalized ? (float)*p / 255.f : (float)*p;
    case 5120: return a->normalized ? artfMaxf((float)(int8_t)*p / 127.f, -1.f) : (float)(int8_t)*p;
    case 5123: { uint16_t v; memcpy(&v, p, 2); return a->normalized ? (float)v / 65535.f : (float)v; }
    case 5122: { int16_t v; memcpy(&v, p, 2); return a->normalized ? artfMaxf((float)v / 32767.f, -1.f) : (float)v; }
    default: { uint32_t v; memcpy(&v, p, 4); return (float)v; }
    }
}

static uint32_t indexAt(const Accessor* a, uint32_t i) {
    const uint8_t* p = a->data + a->stride * i;
    if (a->size == 1) return *p;
    if (a->size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- textures and materials ---- */

static const uint8_t* imageBytes(Loader* l, int64_t image, uint8_t** owned, size_t* size) {
    const Json* j = element(l, l->imageList, (uint32_t)image);
    const Json* uri = member(l, j, "uri");
    if (uri && uri->kind == J_STRING) return *owned = readUri(l, uri, size);
    const Json* v = element(l, l->views, (uint32_t)indexOf(l, j, "bufferView"));
    *size = (size_t)numberOf(l, v, "byteLength", 0);
    return viewBytes(l, indexOf(l, j, "bufferView"), 0, *size, NULL);
}

static ArtfTexture textureOf(Loader* l, const Json* info) {
    if (!info) return 0;
    const Json* texture = element(l, l->textures, (uint32_t)indexOf(l, info, "index"));
    const int64_t image = indexOf(l, texture, "source");
    if (image < 0 || image >= l->imageCount) {
        if (texture) artfLog("artful: glTF: a texture with no image it can read");
        return 0;
    }
    if (l->images[image]) return l->images[image];
    if (l->preparedImages && l->preparedImages[image]) {
        l->images[image] = artfSceneAddPreparedTexture(l->scene, l->preparedImages[image]);
        l->preparedImages[image] = NULL;
        return l->images[image];
    }
    uint8_t* owned = NULL;
    size_t size = 0;
    const uint8_t* data = imageBytes(l, image, &owned, &size);
    if (data && size > 12 && !memcmp((const char*)data + 1, "KTX 20", 6)) {
        l->images[image] = artfSceneAddTextureKtx2(l->scene, data, size);
        artfFree(owned);
        return l->images[image];
    }
    int width = 0, height = 0;
    uint32_t* pixels = data ? artfImageDecode(data, size, &width, &height) : NULL;
    artfFree(owned);
    if (!pixels) {
        artfLog("artful: glTF: image %d cannot be read", (int)image);
        return 0;
    }
    l->images[image] = artfSceneAddTexture(l->scene, pixels, width, height, 0);
    artfFree(pixels);
    return l->images[image];
}

/* Bytes of a texture are in the frame's colors, and its factors in light: as Artful's are. */
static ArtfMaterial materialOf(Loader* l, const Json* m) {
    ArtfMaterialDesc d = artfMaterialDefaults();
    const Json* pbr = member(l, m, "pbrMetallicRoughness");
    float color[4] = {1, 1, 1, 1}, emissive[3] = {0, 0, 0};
    readNumbers(l, member(l, pbr, "baseColorFactor"), color, 4);
    readNumbers(l, member(l, m, "emissiveFactor"), emissive, 3);
    d.baseColor = artfVec3(color[0], color[1], color[2]);
    d.emissive = artfVec3(emissive[0], emissive[1], emissive[2]);
    d.texture = textureOf(l, member(l, pbr, "baseColorTexture"));
    d.metallic = (float)numberOf(l, pbr, "metallicFactor", 1);
    d.roughness = (float)numberOf(l, pbr, "roughnessFactor", 1);
    d.surfaceMap = textureOf(l, member(l, pbr, "metallicRoughnessTexture"));
    const Json* normal = member(l, m, "normalTexture");
    d.normalMap = textureOf(l, normal);
    d.normalScale = (float)numberOf(l, normal, "scale", 1);
    const Json* occlusion = member(l, m, "occlusionTexture");
    d.occlusionMap = textureOf(l, occlusion);
    d.occlusionStrength = (float)numberOf(l, occlusion, "strength", 1);
    d.doubleSided = numberOf(l, m, "doubleSided", 0) != 0;
    const Json* mode = member(l, m, "alphaMode");
    if (stringIs(mode, "MASK")) {
        d.alphaMode = ARTF_ALPHA_MASK;
        d.alphaCutoff = (float)numberOf(l, m, "alphaCutoff", .5);
    } else if (stringIs(mode, "BLEND")) {
        d.alphaMode = ARTF_ALPHA_BLEND;
        d.transparency = 1.f - color[3];
    }
    return artfSceneAddMaterial(l->scene, &d);
}

/* ---- meshes ---- */

/* Smooth normals, for a primitive that has none. */
static void makeNormals(ArtfVertex* v, uint32_t count, const uint32_t* indices, uint32_t indexCount) {
    for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
        ArtfVertex *a = &v[indices[i]], *b = &v[indices[i + 1]], *c = &v[indices[i + 2]];
        const ArtfVec3 n = artfVec3Cross(artfVec3(b->px - a->px, b->py - a->py, b->pz - a->pz), artfVec3(c->px - a->px, c->py - a->py, c->pz - a->pz));
        ArtfVertex* corners[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) { corners[k]->nx += n.x; corners[k]->ny += n.y; corners[k]->nz += n.z; }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const ArtfVec3 n = artfVec3Normalize(artfVec3(v[i].nx, v[i].ny, v[i].nz));
        v[i].nx = n.x;
        v[i].ny = n.y;
        v[i].nz = n.z;
    }
}

static uint32_t byteOf(float light) {
    /* (a color in a file is light; Artful's bytes hold its square root) */
    return (uint32_t)(sqrtf(artfClampf(light, 0, 1)) * 255.f + .5f);
}

static ArtfMesh meshOf(Loader* l, const Json* p, bool skinned, uint32_t slot) {
    const int64_t mode = (int64_t)numberOf(l, p, "mode", 4);
    if (l->preparing && member(l, p, "targets")) return 0; /* (what bends is made when it is added) */
    if (mode < 4 || mode > 6) {
        artfLog("artful: glTF: a primitive of points or lines is left out");
        return 0;
    }
    if (member(l, member(l, p, "extensions"), "KHR_draco_mesh_compression")) {
        artfLog("artful: glTF: Draco compression cannot be read");
        return 0;
    }
    const Json* attributes = member(l, p, "attributes");
    Accessor position, normal, uv, color, index;
    if (!accessorOf(l, indexOf(l, attributes, "POSITION"), &position) || position.components != 3) {
        artfLog("artful: glTF: a primitive without positions it can read");
        return 0;
    }
    const bool hasNormal = accessorOf(l, indexOf(l, attributes, "NORMAL"), &normal) && normal.components == 3 && normal.count >= position.count;
    const bool hasUv = accessorOf(l, indexOf(l, attributes, "TEXCOORD_0"), &uv) && uv.components == 2 && uv.count >= position.count;
    const bool hasColor = accessorOf(l, indexOf(l, attributes, "COLOR_0"), &color) && color.components >= 3 && color.count >= position.count;

    const uint32_t count = position.count;
    ArtfVertex* v = artfCalloc(count, sizeof *v);
    for (uint32_t i = 0; i < count; ++i) {
        v[i].px = componentOf(&position, i, 0);
        v[i].py = componentOf(&position, i, 1);
        v[i].pz = componentOf(&position, i, 2);
        if (hasNormal) {
            v[i].nx = componentOf(&normal, i, 0);
            v[i].ny = componentOf(&normal, i, 1);
            v[i].nz = componentOf(&normal, i, 2);
        }
        if (hasUv) {
            v[i].u = componentOf(&uv, i, 0);
            v[i].v = componentOf(&uv, i, 1);
        }
        if (hasColor) {
            const float a = color.components == 4 ? componentOf(&color, i, 3) : 1.f;
            v[i].color = (uint32_t)(artfClampf(a, 0, 1) * 255.f + .5f) << 24 | byteOf(componentOf(&color, i, 0)) << 16 | byteOf(componentOf(&color, i, 1)) << 8 | byteOf(componentOf(&color, i, 2));
        }
    }

    const bool indexed = accessorOf(l, indexOf(l, p, "indices"), &index);
    const uint32_t listed = indexed ? index.count : count;
    uint32_t* triangles = artfMalloc(((size_t)listed + 1) * 12);
    uint32_t n = 0;
#define AT(i) (indexed ? indexAt(&index, i) : (i))
    if (mode == 4) {
        for (uint32_t i = 0; i + 2 < listed; i += 3) { triangles[n++] = AT(i); triangles[n++] = AT(i + 1); triangles[n++] = AT(i + 2); }
    } else if (mode == 5) {
        for (uint32_t i = 0; i + 2 < listed; ++i) { triangles[n++] = AT(i + (i & 1)); triangles[n++] = AT(i + 1 - (i & 1)); triangles[n++] = AT(i + 2); }
    } else {
        for (uint32_t i = 1; i + 1 < listed; ++i) { triangles[n++] = AT(i); triangles[n++] = AT(i + 1); triangles[n++] = AT(0); }
    }
#undef AT
    /* (artfSceneAddMesh leaves out triangles with indices past the vertices; normals must, too) */
    uint32_t good = 0;
    for (uint32_t i = 0; i + 2 < n; i += 3)
        if (triangles[i] < count && triangles[i + 1] < count && triangles[i + 2] < count) {
            triangles[good++] = triangles[i];
            triangles[good++] = triangles[i + 1];
            triangles[good++] = triangles[i + 2];
        }
    if (!hasNormal) makeNormals(v, count, triangles, good);
    /* what each target adds */
    const Json* targetList = member(l, p, "targets");
    const uint32_t targetCount = targetList && targetList->kind == J_ARRAY ? artfMini((int)targetList->count, 256) : 0;
    ArtfMorphTarget* targets = artfCalloc(targetCount + 1, sizeof *targets);
    for (uint32_t t = 0; t < targetCount; ++t) {
        const Json* target = element(l, targetList, t);
        Accessor moved;
        for (int kind = 0; kind < 2; ++kind) {
            if (!accessorOf(l, indexOf(l, target, kind ? "NORMAL" : "POSITION"), &moved) || moved.count < count || moved.components != 3) continue;
            float* to = artfMalloc((size_t)count * 12);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t c = 0; c < 3; ++c) to[i * 3 + c] = componentOf(&moved, i, c);
            if (kind) targets[t].normals = to;
            else targets[t].positions = to;
        }
    }
    Accessor joints, weights;
    ArtfMesh mesh = 0;
    ArtfVertexSkin* skin = NULL;
    if (skinned && accessorOf(l, indexOf(l, attributes, "JOINTS_0"), &joints) && accessorOf(l, indexOf(l, attributes, "WEIGHTS_0"), &weights) && joints.components == 4 && weights.components == 4 && joints.count >= count && weights.count >= count) {
        skin = artfCalloc(count, sizeof *skin);
        joints.normalized = false;
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t k = 0; k < 4; ++k) {
                skin[i].joints[k] = (uint16_t)componentOf(&joints, i, k);
                skin[i].weights[k] = componentOf(&weights, i, k);
            }
        Accessor joints1, weights1;
        if (accessorOf(l, indexOf(l, attributes, "JOINTS_1"), &joints1) && accessorOf(l, indexOf(l, attributes, "WEIGHTS_1"), &weights1) && joints1.components == 4 && weights1.components == 4 && joints1.count >= count && weights1.count >= count) {
            joints1.normalized = false;
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t k = 0; k < 4; ++k) {
                    skin[i].joints[4 + k] = (uint16_t)componentOf(&joints1, i, k);
                    skin[i].weights[4 + k] = componentOf(&weights1, i, k);
                }
        }
    }
    if (skin || targetCount) {
        mesh = artfSceneAddMorphMesh(l->scene, v, count, triangles, good, skin, targets, targetCount);
    } else if (l->preparing) {
        l->preparedMeshes[slot] = artfPrepareMesh(v, count, triangles, good, true);
    } else {
        mesh = artfSceneAddMesh(l->scene, v, count, triangles, good);
    }
    for (uint32_t t = 0; t < targetCount; ++t) {
        artfFree((void*)targets[t].positions);
        artfFree((void*)targets[t].normals);
    }
    artfFree(targets);
    artfFree(skin);
    artfFree(v);
    artfFree(triangles);
    return mesh;
}

/* ---- nodes ---- */

static ArtfMat4 fromPose(const float* p) {
    const ArtfQuat rotation = {p[3], p[4], p[5], p[6]};
    const ArtfTransform trs = artfTransformTrs(artfVec3(p[0], p[1], p[2]), rotation, artfVec3(p[7], p[8], p[9]));
    ArtfMat4 m = artfMat4Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) m.m[c * 4 + r] = trs.m[r * 4 + c];
    return m;
}

static ArtfTransform toTransform(const ArtfMat4* world) {
    ArtfTransform t;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) t.m[r * 4 + c] = world->m[c * 4 + r];
    return t;
}

/* Worlds of all nodes, from their locals; a parent may come after its child, so each asks for its parent's. */
static void worldOf(ModelData* d, uint32_t i, uint8_t* done, int depth) {
    if (done[i]) return;
    ModelNode* n = &d->nodes[i];
    done[i] = 1;
    if (!n->fixed) n->local = fromPose(n->pose);
    if (n->parent >= 0 && depth < 512) {
        worldOf(d, (uint32_t)n->parent, done, depth + 1);
        n->world = artfMat4Mul(d->nodes[n->parent].world, n->local);
    } else {
        n->world = artfMat4Mul(d->base, n->local);
    }
}

/* All of a model as its nodes are now: instances where their nodes are, skins by their bones. */
static void applyPose(ArtfScene* s, ModelData* d) {
    uint8_t* done = artfCalloc(d->nodeCount + 1, 1);
    for (uint32_t i = 0; i < d->nodeCount; ++i) worldOf(d, i, done, 0);
    artfFree(done);
    ARTF_VEC(ArtfTransform) joints = {0};
    for (size_t i = 0; i < d->parts.len; ++i) {
        const ModelPart* part = &d->parts.items[i];
        const ModelNode* node = &d->nodes[part->node];
        if (node->weightCount) artfInstanceSetMorphWeights(s, part->instance, node->weightPose, node->weightCount);
        if (part->skin < 0) {
            const ArtfTransform t = toTransform(&d->nodes[part->node].world);
            artfInstanceSetTransform(s, part->instance, &t);
            continue;
        }
        /* (a skinned mesh goes by its bones alone, wherever its own node is) */
        const ModelSkin* skin = &d->skins[part->skin];
        artfVecReserve(&joints, skin->count + 1);
        for (uint32_t j = 0; j < skin->count; ++j) {
            ArtfMat4 inverse;
            memcpy(inverse.m, skin->inverses + (size_t)j * 16, 64);
            const ArtfMat4 bone = artfMat4Mul(d->nodes[skin->joints[j]].world, inverse);
            joints.items[j] = toTransform(&bone);
        }
        const ArtfTransform identity = artfTransformIdentity();
        artfInstanceSetTransform(s, part->instance, &identity);
        artfInstanceSetJoints(s, part->instance, joints.items, skin->count);
    }
    artfVecFree(&joints);
}

static void placeNode(Loader* l, const Json* node, uint32_t index, int depth) {
    if (!node || depth > 256) return;
    const int64_t mesh = indexOf(l, node, "mesh");
    const int64_t skin = indexOf(l, node, "skin");
    const bool skinned = skin >= 0 && skin < l->model->skinCount;
    const Json* m = element(l, l->meshes, mesh < 0 ? UINT32_MAX : (uint32_t)mesh);
    if (m) {
        const ArtfTransform t = artfTransformIdentity();
        const Json* primitives = member(l, m, "primitives");
        const uint32_t count = primitives && primitives->kind == J_ARRAY ? primitives->count : 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = l->primitiveStart[mesh] + i + (skinned ? l->primitiveTotal : 0);
            const Json* p = element(l, primitives, i);
            if (l->primitives[slot] == UINT32_MAX && l->preparedMeshes && l->preparedMeshes[slot]) {
                l->primitives[slot] = artfSceneAddPreparedMesh(l->scene, l->preparedMeshes[slot]);
                l->preparedMeshes[slot] = NULL;
            }
            if (l->primitives[slot] == UINT32_MAX) l->primitives[slot] = meshOf(l, p, skinned, slot);
            if (!l->primitives[slot]) continue;
            const int64_t material = indexOf(l, p, "material");
            const ArtfMaterial used = material >= 0 && material < l->materialCount ? l->materials[material] : 0;
            const ArtfInstance instance = artfSceneAddInstance(l->scene, l->primitives[slot], used, &t);
            if (instance) {
                const bool bends = skinned && l->scene->meshes.items[l->primitives[slot] - 1].skin;
                const ModelPart part = {instance, index, bends ? (int32_t)skin : -1};
                artfVecPush(&l->model->parts, part);
                ++l->instances;
            }
        }
    }
    const Json* children = member(l, node, "children");
    for (const Json* c = children && children->kind == J_ARRAY ? nodeAt(l, children->first) : NULL; c; c = nodeAt(l, c->next))
        if (c->kind == J_NUMBER) placeNode(l, element(l, l->nodes, (uint32_t)c->number), (uint32_t)c->number, depth + 1);
}

/* ---- what plays ---- */

static void readNodes(Loader* l, ModelData* d) {
    d->nodeCount = l->nodes && l->nodes->kind == J_ARRAY ? l->nodes->count : 0;
    d->nodes = artfCalloc(d->nodeCount + 1, sizeof *d->nodes);
    for (uint32_t i = 0; i < d->nodeCount; ++i) d->nodes[i].parent = -1;
    uint32_t i = 0;
    for (const Json* j = d->nodeCount ? nodeAt(l, l->nodes->first) : NULL; j; j = nodeAt(l, j->next), ++i) {
        ModelNode* n = &d->nodes[i];
        const float start[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        memcpy(n->pose, start, sizeof start);
        readNumbers(l, member(l, j, "translation"), n->pose, 3);
        readNumbers(l, member(l, j, "rotation"), n->pose + 3, 4);
        readNumbers(l, member(l, j, "scale"), n->pose + 7, 3);
        memcpy(n->t, n->pose, 12);
        memcpy(n->r, n->pose + 3, 16);
        memcpy(n->s, n->pose + 7, 12);
        /* what its mesh is turned into: the node says, or else the mesh; none is all zeros */
        const Json* m = element(l, l->meshes, indexOf(l, j, "mesh") < 0 ? UINT32_MAX : (uint32_t)indexOf(l, j, "mesh"));
        const Json* first = element(l, member(l, m, "primitives"), 0);
        const Json* targets = member(l, first, "targets");
        if (targets && targets->kind == J_ARRAY && targets->count) {
            n->weightCount = artfMini((int)targets->count, 256);
            n->weights = artfCalloc(n->weightCount, 4);
            n->weightPose = artfCalloc(n->weightCount, 4);
            const Json* given = member(l, j, "weights");
            readNumbers(l, given ? given : member(l, m, "weights"), n->weights, (int)n->weightCount);
            memcpy(n->weightPose, n->weights, (size_t)n->weightCount * 4);
        }
        const Json* matrix = member(l, j, "matrix");
        if (matrix) {
            n->local = artfMat4Identity();
            readNumbers(l, matrix, n->local.m, 16);
            n->fixed = true;
        }
        const Json* kids = member(l, j, "children");
        for (const Json* c = kids && kids->kind == J_ARRAY ? nodeAt(l, kids->first) : NULL; c; c = nodeAt(l, c->next))
            if (c->kind == J_NUMBER && c->number >= 0 && c->number < d->nodeCount && (uint32_t)c->number != i) d->nodes[(uint32_t)c->number].parent = (int32_t)i;
    }
}

static void readSkins(Loader* l, ModelData* d) {
    const Json* list = member(l, l->root, "skins");
    d->skinCount = list && list->kind == J_ARRAY ? list->count : 0;
    d->skins = artfCalloc(d->skinCount + 1, sizeof *d->skins);
    uint32_t i = 0;
    for (const Json* j = d->skinCount ? nodeAt(l, list->first) : NULL; j; j = nodeAt(l, j->next), ++i) {
        const Json* joints = member(l, j, "joints");
        const uint32_t count = joints && joints->kind == J_ARRAY ? joints->count : 0;
        ModelSkin* skin = &d->skins[i];
        skin->joints = artfCalloc(count + 1, 4);
        skin->inverses = artfMalloc(((size_t)count + 1) * 64);
        skin->count = count;
        uint32_t k = 0;
        for (const Json* n = count ? nodeAt(l, joints->first) : NULL; n; n = nodeAt(l, n->next), ++k) skin->joints[k] = n->kind == J_NUMBER && n->number >= 0 && n->number < d->nodeCount ? (uint32_t)n->number : 0;
        Accessor a;
        const bool has = accessorMatrices(l, indexOf(l, j, "inverseBindMatrices"), &a);
        for (uint32_t b = 0; b < count; ++b) {
            ArtfMat4 m = artfMat4Identity();
            if (has && b < a.count) memcpy(m.m, a.data + a.stride * b, 64);
            memcpy(skin->inverses + (size_t)b * 16, m.m, 64);
        }
    }
}

static void readClips(Loader* l, ModelData* d) {
    const Json* list = member(l, l->root, "animations");
    d->clipCount = list && list->kind == J_ARRAY ? list->count : 0;
    d->clips = artfCalloc(d->clipCount + 1, sizeof *d->clips);
    uint32_t i = 0;
    for (const Json* j = d->clipCount ? nodeAt(l, list->first) : NULL; j; j = nodeAt(l, j->next), ++i) {
        ModelClip* clip = &d->clips[i];
        const Json* name = member(l, j, "name");
        if (name && name->kind == J_STRING) memcpy(clip->name, name->text, artfMini((int)name->length, (int)sizeof clip->name - 1));
        const Json* channels = member(l, j, "channels");
        const Json* samplers = member(l, j, "samplers");
        const uint32_t count = channels && channels->kind == J_ARRAY ? channels->count : 0;
        clip->channels = artfCalloc(count + 1, sizeof *clip->channels);
        for (const Json* c = count ? nodeAt(l, channels->first) : NULL; c; c = nodeAt(l, c->next)) {
            const Json* target = member(l, c, "target");
            const Json* path = member(l, target, "path");
            const int64_t node = indexOf(l, target, "node");
            const Json* sampler = element(l, samplers, (uint32_t)indexOf(l, c, "sampler"));
            const int kind = stringIs(path, "translation") ? PATH_T : stringIs(path, "rotation") ? PATH_R : stringIs(path, "scale") ? PATH_S : stringIs(path, "weights") ? PATH_W : -1;
            if (kind < 0) continue;
            Accessor input, output;
            if (node < 0 || node >= d->nodeCount || !sampler || !accessorOf(l, indexOf(l, sampler, "input"), &input) || !accessorOf(l, indexOf(l, sampler, "output"), &output)) continue;
            const Json* mode = member(l, sampler, "interpolation");
            const int blend = stringIs(mode, "STEP") ? BLEND_STEP : stringIs(mode, "CUBICSPLINE") ? BLEND_CUBIC : BLEND_LINEAR;
            const int width = kind == PATH_R ? 4 : kind == PATH_W ? (int)d->nodes[node].weightCount : 3;
            const uint32_t need = input.count * (blend == BLEND_CUBIC ? 3u : 1u);
            if (input.components != 1 || !width) continue;
            /* (the weights of a key are that many numbers, one after the other) */
            if (kind == PATH_W ? output.components != 1 || output.count < need * (uint32_t)width : (int)output.components != width || output.count < need) continue;
            ModelChannel* ch = &clip->channels[clip->channelCount++];
            ch->node = (uint32_t)node;
            ch->keys = input.count;
            ch->path = kind;
            ch->blend = blend;
            ch->width = width;
            ch->times = artfMalloc((size_t)input.count * 4);
            ch->values = artfMalloc((size_t)need * (size_t)width * 4);
            for (uint32_t k = 0; k < input.count; ++k) {
                ch->times[k] = componentOf(&input, k, 0);
                clip->duration = artfMaxf(clip->duration, ch->times[k]);
            }
            for (uint32_t k = 0; k < need; ++k)
                for (int w = 0; w < width; ++w) ch->values[(size_t)k * (size_t)width + (size_t)w] = kind == PATH_W ? componentOf(&output, k * (uint32_t)width + (uint32_t)w, 0) : componentOf(&output, k, (uint32_t)w);
        }
    }
}

static void modelFree(ModelData* d) {
    for (uint32_t i = 0; i < d->skinCount; ++i) {
        artfFree(d->skins[i].joints);
        artfFree(d->skins[i].inverses);
    }
    for (uint32_t i = 0; i < d->clipCount; ++i) {
        for (uint32_t c = 0; c < d->clips[i].channelCount; ++c) {
            artfFree(d->clips[i].channels[c].times);
            artfFree(d->clips[i].channels[c].values);
        }
        artfFree(d->clips[i].channels);
    }
    for (uint32_t i = 0; i < d->nodeCount; ++i) {
        artfFree(d->nodes[i].weights);
        artfFree(d->nodes[i].weightPose);
    }
    artfFree(d->nodes);
    artfFree(d->skins);
    artfFree(d->clips);
    artfVecFree(&d->parts);
    artfFree(d);
}

static void modelsFree(void* list) {
    ModelList* m = list;
    for (size_t i = 0; i < m->len; ++i) modelFree(m->items[i]);
    artfVecFree(m);
    artfFree(m);
}

static ModelData* modelAt(const ArtfScene* s, ArtfModel m) {
    const ModelList* list = s->models;
    if (m && list && m <= list->len) return list->items[m - 1];
    artfLog("artful: model %u is not there", m);
    return NULL;
}

/* sin(t a) / sin(a), from cos(a), by its series: a sine would give other bytes on another C library. Here cos(a) is
 * not below 0, where sixteen terms are within 2e-6. */
static float slerpShare(float t, float x) {
    float r = 1.f;
    for (int i = 16; i > 0; --i) {
        const float u = 1.f / (float)(i * (2 * i + 1)), v = (float)i / (float)(2 * i + 1);
        r = 1.f + (u * t * t - v) * (x - 1.f) * r;
    }
    return t * r;
}

void artfQuatSlerp(const float* a, const float* b, float t, float* out) {
    float x = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sign = 1.f;
    if (x < 0) { x = -x; sign = -1.f; } /* (the short way) */
    x = artfMinf(x, 1.f);
    const float from = slerpShare(1.f - t, x), to = slerpShare(t, x) * sign;
    float length = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = a[i] * from + b[i] * to;
        length += out[i] * out[i];
    }
    if (length > 0) {
        const float inverse = 1.f / sqrtf(length);
        for (int i = 0; i < 4; ++i) out[i] *= inverse;
    }
}

/* A channel at a time, into out. */
static void sampleChannel(const ModelChannel* c, float time, float* out) {
    const int w = c->width;
    const bool cubic = c->blend == BLEND_CUBIC;
    const float* v = c->values;
#define VALUE(k) (cubic ? v + ((size_t)(k) * 3 + 1) * (size_t)w : v + (size_t)(k) * (size_t)w)
    if (c->keys == 1 || time <= c->times[0]) { memcpy(out, VALUE(0), (size_t)w * 4); return; }
    if (time >= c->times[c->keys - 1]) { memcpy(out, VALUE(c->keys - 1), (size_t)w * 4); return; }
    uint32_t lo = 0, hi = c->keys - 1;
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (c->times[mid] <= time) lo = mid;
        else hi = mid;
    }
    const float span = c->times[hi] - c->times[lo], t = span > 0 ? (time - c->times[lo]) / span : 0;
    const float *a = VALUE(lo), *b = VALUE(hi);
    if (c->blend == BLEND_STEP) {
        memcpy(out, a, (size_t)w * 4);
    } else if (cubic) {
        const float t2 = t * t, t3 = t2 * t;
        const float* outgoing = v + ((size_t)lo * 3 + 2) * (size_t)w;
        const float* incoming = v + ((size_t)hi * 3) * (size_t)w;
        for (int i = 0; i < w; ++i)
            out[i] = (2 * t3 - 3 * t2 + 1) * a[i] + (t3 - 2 * t2 + t) * span * outgoing[i] + (-2 * t3 + 3 * t2) * b[i] + (t3 - t2) * span * incoming[i];
    } else if (c->path == PATH_R) {
        artfQuatSlerp(a, b, t, out);
    } else {
        for (int i = 0; i < w; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    }
#undef VALUE
    if (c->path == PATH_R) {
        const float length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (length > 0)
            for (int i = 0; i < 4; ++i) out[i] /= length;
    }
}

void artfAnimationSet(ArtfScene* s, ArtfModel m, uint32_t clip, float time) {
    ModelData* d = modelAt(s, m);
    if (!d) return;
    if (clip >= d->clipCount) {
        artfLog("artful: model %u has no clip %u", m, clip);
        return;
    }
    const ModelClip* c = &d->clips[clip];
    if (c->duration > 0) time -= floorf(time / c->duration) * c->duration;
    for (uint32_t i = 0; i < d->nodeCount; ++i) {
        ModelNode* n = &d->nodes[i];
        memcpy(n->pose, n->t, 12);
        memcpy(n->pose + 3, n->r, 16);
        memcpy(n->pose + 7, n->s, 12);
        if (n->weightCount) memcpy(n->weightPose, n->weights, (size_t)n->weightCount * 4);
    }
    for (uint32_t i = 0; i < c->channelCount; ++i) {
        const ModelChannel* ch = &c->channels[i];
        ModelNode* n = &d->nodes[ch->node];
        sampleChannel(ch, time, ch->path == PATH_W ? n->weightPose : n->pose + (ch->path == PATH_T ? 0 : ch->path == PATH_R ? 3 : 7));
    }
    applyPose(s, d);
}

void artfModelSetTransform(ArtfScene* s, ArtfModel m, const ArtfTransform* transform) {
    ModelData* d = modelAt(s, m);
    if (!d) return;
    d->base = artfMat4Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) d->base.m[c * 4 + r] = transform->m[r * 4 + c];
    applyPose(s, d);
}

uint32_t artfModelInstanceCount(const ArtfScene* s, ArtfModel m) { const ModelData* d = modelAt(s, m); return d ? (uint32_t)d->parts.len : 0; }
uint32_t artfModelClipCount(const ArtfScene* s, ArtfModel m) { const ModelData* d = modelAt(s, m); return d ? d->clipCount : 0; }
const char* artfModelClipName(const ArtfScene* s, ArtfModel m, uint32_t clip) { const ModelData* d = modelAt(s, m); return d && clip < d->clipCount ? d->clips[clip].name : ""; }
float artfModelClipDuration(const ArtfScene* s, ArtfModel m, uint32_t clip) { const ModelData* d = modelAt(s, m); return d && clip < d->clipCount ? d->clips[clip].duration : 0; }

static bool openJson(Loader* l, const char* text, size_t size) {
    l->json.at = text;
    l->json.end = text + size;
    const uint32_t root = readValue(&l->json);
    if (!root || l->json.nodes.items[root - 1].kind != J_OBJECT) {
        artfLog("artful: glTF: the file's JSON cannot be read");
        return 0;
    }
    l->root = nodeAt(l, root);
    const Json* version = member(l, member(l, l->root, "asset"), "version");
    if (!version || version->kind != J_STRING || !version->length || version->text[0] != '2') {
        artfLog("artful: glTF: only version 2 can be read");
        return 0;
    }
    /* A file that says it cannot be read without something is not read without it. */
    const Json* required = member(l, l->root, "extensionsRequired");
    if (required && required->kind == J_ARRAY)
        for (const Json* e = nodeAt(l, required->first); e; e = nodeAt(l, e->next))
            if (!stringIs(e, "KHR_mesh_quantization")) {
                artfLog("artful: glTF: the file requires %.*s, which cannot be read here", e->kind == J_STRING ? (int)e->length : 0, e->text);
                return 0;
            }
    if (!loadBuffers(l)) return 0;
    l->accessors = member(l, l->root, "accessors");
    l->views = member(l, l->root, "bufferViews");
    l->meshes = member(l, l->root, "meshes");
    l->nodes = member(l, l->root, "nodes");
    l->textures = member(l, l->root, "textures");
    l->imageList = member(l, l->root, "images");
    l->imageCount = l->imageList && l->imageList->kind == J_ARRAY ? l->imageList->count : 0;
    const uint32_t meshCount = l->meshes && l->meshes->kind == J_ARRAY ? l->meshes->count : 0;
    l->primitiveStart = artfCalloc(meshCount + 1, 4);
    uint32_t total = 0, i = 0;
    for (const Json* m = meshCount ? nodeAt(l, l->meshes->first) : NULL; m; m = nodeAt(l, m->next)) {
        l->primitiveStart[i++] = total;
        const Json* p = member(l, m, "primitives");
        total += p && p->kind == J_ARRAY ? p->count : 0;
    }
    l->primitiveTotal = total;
    return true;
}

static uint32_t loadJson(Loader* l, const ArtfTransform* transform) {
    ModelData* model = ARTF_NEW(ModelData);
    l->model = model;
    model->base = artfMat4Identity();
    if (transform)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) model->base.m[c * 4 + r] = transform->m[r * 4 + c];
    readNodes(l, model);
    readSkins(l, model);
    readClips(l, model);

    l->images = artfCalloc(l->imageCount + 1, sizeof *l->images);
    const Json* materials = member(l, l->root, "materials");
    l->materialCount = materials && materials->kind == J_ARRAY ? materials->count : 0;
    l->materials = artfCalloc(l->materialCount + 1, sizeof *l->materials);
    uint32_t i = 0;
    for (const Json* m = l->materialCount ? nodeAt(l, materials->first) : NULL; m; m = nodeAt(l, m->next)) l->materials[i++] = materialOf(l, m);

    const uint32_t total = l->primitiveTotal;
    l->primitives = artfMalloc(((size_t)total * 2 + 1) * sizeof *l->primitives);
    memset(l->primitives, 0xFF, ((size_t)total * 2 + 1) * sizeof *l->primitives);

    const Json* scenes = member(l, l->root, "scenes");
    const int64_t chosen = indexOf(l, l->root, "scene");
    const Json* scene = element(l, scenes, chosen < 0 ? 0 : (uint32_t)chosen);
    const Json* roots = member(l, scene, "nodes");
    if (roots && roots->kind == J_ARRAY) {
        for (const Json* n = nodeAt(l, roots->first); n; n = nodeAt(l, n->next))
            if (n->kind == J_NUMBER) placeNode(l, element(l, l->nodes, (uint32_t)n->number), (uint32_t)n->number, 0);
    } else if (l->nodes && l->nodes->kind == J_ARRAY) {
        /* (no scene: what no node holds as a child) */
        uint8_t* child = artfCalloc(l->nodes->count + 1, 1);
        for (const Json* n = nodeAt(l, l->nodes->first); n; n = nodeAt(l, n->next)) {
            const Json* kids = member(l, n, "children");
            for (const Json* c = kids && kids->kind == J_ARRAY ? nodeAt(l, kids->first) : NULL; c; c = nodeAt(l, c->next))
                if (c->kind == J_NUMBER && c->number >= 0 && c->number < l->nodes->count) child[(uint32_t)c->number] = 1;
        }
        i = 0;
        for (const Json* n = nodeAt(l, l->nodes->first); n; n = nodeAt(l, n->next), ++i)
            if (!child[i]) placeNode(l, n, i, 0);
        artfFree(child);
    }
    applyPose(l->scene, model);
    return l->instances;
}

/* The JSON and the buffers, whichever way they came. */
static bool loaderOpen(Loader* l, const void* data, size_t size) {
    const uint8_t* d = data;
    const char* text = data;
    size_t textSize = size;
    if (size >= 20 && !memcmp(d, "glTF", 4)) {
        /* a header, then chunks: JSON, and maybe the binary buffer */
        uint32_t first, kind;
        memcpy(&first, d + 12, 4);
        memcpy(&kind, d + 16, 4);
        if (kind != 0x4E4F534Au || first > size - 20) {
            artfLog("artful: glTF: not a binary glTF that can be read");
            return false;
        }
        text = (const char*)d + 20;
        textSize = first;
        const size_t next = 20 + (size_t)first;
        if (next + 8 <= size) {
            uint32_t length;
            memcpy(&length, d + next, 4);
            memcpy(&kind, d + next + 4, 4);
            if (kind == 0x004E4942u && length <= size - next - 8) {
                l->binary = d + next + 8;
                l->binarySize = length;
            }
        }
    }
    return openJson(l, text, textSize);
}

static void loaderClose(Loader* l) {
    for (size_t i = 0; i < l->owned.len; ++i) artfFree(l->owned.items[i]);
    artfVecFree(&l->owned);
    artfVecFree(&l->json.nodes);
    artfFree(l->buffers);
    artfFree(l->bufferSizes);
    artfFree(l->images);
    artfFree(l->materials);
    artfFree(l->primitives);
    artfFree(l->primitiveStart);
    if (l->preparedImages)
        for (uint32_t i = 0; i < l->imageCount; ++i) artfPreparedTextureFree(l->preparedImages[i]);
    if (l->preparedMeshes)
        for (uint32_t i = 0; i < l->primitiveTotal; ++i) artfPreparedMeshFree(l->preparedMeshes[i]);
    artfFree(l->preparedImages);
    artfFree(l->preparedMeshes);
}

/* What was loaded goes to the scene, which keeps it for its clips. */
static ArtfModel loaderFinish(ArtfScene* s, Loader* l, uint32_t made) {
    ArtfModel handle = 0;
    if (l->model) {
        if (!s->models) {
            s->models = ARTF_NEW(ModelList);
            s->modelsFree = modelsFree;
        }
        ModelList* list = s->models;
        artfVecPush(list, l->model);
        handle = made ? (ArtfModel)list->len : 0;
    }
    loaderClose(l);
    return handle;
}

ArtfModel artfSceneLoadModelMemory(ArtfScene* s, const void* data, size_t size, const char* folder, const ArtfTransform* transform) {
    Loader loader, *l = &loader;
    memset(l, 0, sizeof *l);
    l->scene = s;
    l->folder = folder;
    return loaderFinish(s, l, loaderOpen(l, data, size) ? loadJson(l, transform) : 0);
}

struct ArtfPreparedModel {
    Loader loader;
    uint8_t* data;
    char folder[1024];
};

/* The slow part: images and the meshes that nodes place whole. What bends is made when it is added: it has no levels. */
static void prepareAll(Loader* l) {
    l->preparing = true;
    l->preparedImages = artfCalloc(l->imageCount + 1, sizeof *l->preparedImages);
    l->preparedMeshes = artfCalloc((size_t)l->primitiveTotal + 1, sizeof *l->preparedMeshes);
    for (uint32_t i = 0; i < l->imageCount; ++i) {
        uint8_t* owned = NULL;
        size_t size = 0;
        const uint8_t* data = imageBytes(l, i, &owned, &size);
        if (data && !(size > 12 && !memcmp((const char*)data + 1, "KTX 20", 6))) {
            int width = 0, height = 0;
            uint32_t* pixels = artfImageDecode(data, size, &width, &height);
            if (pixels) l->preparedImages[i] = artfPrepareTexture(pixels, width, height, 0);
            artfFree(pixels);
        }
        artfFree(owned);
    }
    const uint32_t nodes = l->nodes && l->nodes->kind == J_ARRAY ? l->nodes->count : 0;
    for (const Json* n = nodes ? nodeAt(l, l->nodes->first) : NULL; n; n = nodeAt(l, n->next)) {
        const int64_t mesh = indexOf(l, n, "mesh");
        const Json* m = element(l, l->meshes, mesh < 0 ? UINT32_MAX : (uint32_t)mesh);
        if (!m || indexOf(l, n, "skin") >= 0) continue;
        const Json* primitives = member(l, m, "primitives");
        const uint32_t count = primitives && primitives->kind == J_ARRAY ? primitives->count : 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = l->primitiveStart[mesh] + i;
            if (!l->preparedMeshes[slot]) meshOf(l, element(l, primitives, i), false, slot);
        }
    }
    l->preparing = false;
}

ArtfPreparedModel* artfPrepareModelMemory(const void* data, size_t size, const char* folder) {
    ArtfPreparedModel* p = ARTF_NEW(ArtfPreparedModel);
    p->data = artfMalloc(size + 1);
    memcpy(p->data, data, size);
    snprintf(p->folder, sizeof p->folder, "%s", folder ? folder : "");
    p->loader.folder = folder ? p->folder : NULL;
    if (!loaderOpen(&p->loader, p->data, size)) {
        artfPreparedModelFree(p);
        return NULL;
    }
    prepareAll(&p->loader);
    return p;
}

void artfPreparedModelFree(ArtfPreparedModel* p) {
    if (!p) return;
    loaderClose(&p->loader);
    artfFree(p->data);
    artfFree(p);
}

ArtfModel artfSceneAddPreparedModel(ArtfScene* s, ArtfPreparedModel* p, const ArtfTransform* transform) {
    if (!p) return 0;
    Loader* l = &p->loader;
    l->scene = s;
    const ArtfModel made = loaderFinish(s, l, loadJson(l, transform));
    artfFree(p->data);
    artfFree(p);
    return made;
}

uint32_t artfSceneLoadGltfMemory(ArtfScene* s, const void* data, size_t size, const char* folder, const ArtfTransform* transform) {
    const ArtfModel m = artfSceneLoadModelMemory(s, data, size, folder, transform);
    return m ? artfModelInstanceCount(s, m) : 0;
}

static void folderOf(const char* path, char* folder, size_t size) {
    snprintf(folder, size, "%s", path);
    char* slash = strrchr(folder, '/');
    char* back = strrchr(folder, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) *slash = 0;
    else strcpy(folder, ".");
}

ArtfModel artfSceneLoadModel(ArtfScene* s, const char* path, const ArtfTransform* transform) {
    size_t size = 0;
    uint8_t* data = readFile(path, &size);
    if (!data) {
        artfLog("artful: glTF: cannot read %s", path);
        return 0;
    }
    char folder[1024];
    folderOf(path, folder, sizeof folder);
    const ArtfModel made = artfSceneLoadModelMemory(s, data, size, folder, transform);
    artfFree(data);
    return made;
}

ArtfPreparedModel* artfPrepareModel(const char* path) {
    size_t size = 0;
    uint8_t* data = readFile(path, &size);
    if (!data) {
        artfLog("artful: glTF: cannot read %s", path);
        return NULL;
    }
    char folder[1024];
    folderOf(path, folder, sizeof folder);
    ArtfPreparedModel* made = artfPrepareModelMemory(data, size, folder);
    artfFree(data);
    return made;
}

uint32_t artfSceneLoadGltf(ArtfScene* s, const char* path, const ArtfTransform* transform) {
    const ArtfModel m = artfSceneLoadModel(s, path, transform);
    return m ? artfModelInstanceCount(s, m) : 0;
}
