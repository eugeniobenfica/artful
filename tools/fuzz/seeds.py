#!/usr/bin/env python3
# Writes seeds for tools/fuzz/fuzz.c's targets: a few small files of each kind, valid, from which libFuzzer starts.
#
#   python tools/fuzz/seeds.py dir      (dir/gltf, dir/image, dir/hdr, dir/ktx2, dir/blocks, dir/ies, dir/cube, dir/meshopt)
#
# Pictures are made by Pillow where it is there (PNG of each kind, and JPEG baseline and progressive); without it,
# PNG by hand alone. Nothing else is needed.

import base64, io, json, os, random, struct, sys, zlib

out = sys.argv[1] if len(sys.argv) > 1 else "seeds"
rng = random.Random(1)


def put(kind, name, data):
    os.makedirs(os.path.join(out, kind), exist_ok=True)
    with open(os.path.join(out, kind, name), "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


# ---- pictures ----

def png(width, height, kind, rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, kind, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


put("image", "rgba.png", png(4, 4, 6, [bytes(rng.randrange(256) for _ in range(16)) for _ in range(4)]))
put("image", "rgb.png", png(5, 3, 2, [bytes(rng.randrange(256) for _ in range(15)) for _ in range(3)]))
put("image", "grey.png", png(3, 3, 0, [bytes(rng.randrange(256) for _ in range(3)) for _ in range(3)]))
try:
    from PIL import Image
    picture = Image.new("RGB", (24, 16))
    picture.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(24 * 16)])
    for name, args in [("baseline.jpg", dict(format="JPEG", quality=80)), ("progressive.jpg", dict(format="JPEG", progressive=True)),
                       ("full.jpg", dict(format="JPEG", subsampling=0)), ("palette.png", None), ("greyAlpha.png", None), ("deep.png", None)]:
        b = io.BytesIO()
        if name == "palette.png": picture.convert("P").save(b, format="PNG")
        elif name == "greyAlpha.png": picture.convert("LA").save(b, format="PNG")
        elif name == "deep.png": picture.convert("I;16").save(b, format="PNG")
        else: picture.save(b, **args)
        put("image", name, b.getvalue())
    b = io.BytesIO()
    picture.convert("L").save(b, format="JPEG")
    put("image", "grey.jpg", b.getvalue())
except ImportError:
    pass

# ---- light from all around ----

def rgbe(r, g, b):
    m = max(r, g, b)
    if m < 1e-32: return bytes(4)
    import math
    e = math.frexp(m)[1]
    s = 256.0 / 2.0 ** e
    return bytes([int(r * s), int(g * s), int(b * s), e + 128])


flat = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n" + b"".join(rgbe(rng.random(), rng.random(), rng.random()) for _ in range(8))
put("hdr", "flat.hdr", flat)
w = 16
row = [rgbe(rng.random() * 4, rng.random(), .5) for _ in range(w)]
rle = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=1\n\n-Y 2 +X 16\n")
for _ in range(2):
    rle += bytes([2, 2, w >> 8, w & 255])
    for c in range(4):
        rle += bytes([128 + 4, row[0][c]]) + bytes([w - 4]) + bytes(px[c] for px in row[4:])
put("hdr", "rle.hdr", bytes(rle))

# ---- blocks, and KTX2 files of them ----

def ktx2(fmt, width, height, blocks):
    head = bytearray(104)
    head[:12] = bytes([0xAB, ord("K"), ord("T"), ord("X"), ord(" "), ord("2"), ord("0"), 0xBB, 13, 10, 26, 10])
    struct.pack_into("<IIIIIIIII", head, 12, fmt, 1, width, height, 0, 0, 1, 1, 0)
    struct.pack_into("<IIQQQ", head, 48, 0, 0, 0, 0, 0)
    struct.pack_into("<QQQ", head, 80, 104, len(blocks), len(blocks))
    return bytes(head) + blocks


for name, fmt, size in [("bc1", 131, 8), ("bc1a", 133, 8), ("bc3", 137, 16), ("bc7", 145, 16), ("etc2", 147, 8), ("etc2a", 151, 16), ("astc4", 157, 16), ("astc8", 171, 16)]:
    blocks = bytes(rng.randrange(256) for _ in range(size * 4))
    put("ktx2", name + ".ktx2", ktx2(fmt, 8, 8, blocks))
for which in range(15):
    put("blocks", "b%02d" % which, bytes([which]) + bytes(rng.randrange(256) for _ in range(16)))

# ---- a light's profile, a grading table ----

put("ies", "type-c.ies", "IESNA:LM-63-2002\n[TEST] seed\nTILT=NONE\n1 1000 1 5 3 1 2 0.1 0.1 0\n1 1 100\n0 22.5 45 67.5 90\n0 90 180\n"
    + "\n".join(" ".join("%d" % rng.randrange(1000) for _ in range(5)) for _ in range(3)) + "\n")
put("ies", "one.ies", "IESNA91\nTILT=NONE\n1 -1 1 3 1 1 2 0 0 0\n1 1 50\n0 45 90\n0\n100 80 10\n")
put("cube", "two.cube", "TITLE \"seed\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n" + "".join("%g %g %g\n" % (r, g, b) for b in (0, 1) for g in (0, 1) for r in (0, 1)))
put("cube", "three.cube", "LUT_3D_SIZE 3\n" + "".join("%.3f %.3f %.3f\n" % (rng.random(), rng.random(), rng.random()) for _ in range(27)))

# ---- meshopt's buffers: mode (0 vertices, 1 triangles, 2 indices), count, width, then the stream ----

for mode in range(3):
    for k in range(2):
        put("meshopt", "m%d%d" % (mode, k), bytes([mode, 12 + k * 30, 0, 3]) + bytes([0xA0 | mode << 4]) + bytes(rng.randrange(256) for _ in range(60)))

# ---- glTF ----

def glb(document, binary):
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + 8 + len(binary)) + struct.pack("<I", len(text)) + b"JSON" + text + struct.pack("<I", len(binary)) + b"BIN\0" + binary


positions = struct.pack("<12f", -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0)
normals = struct.pack("<12f", *([0, 0, 1] * 4))
uvs = struct.pack("<8f", 0, 0, 1, 0, 1, 1, 0, 1)
indices = struct.pack("<6H", 0, 1, 2, 0, 2, 3)
picture = png(2, 2, 6, [bytes([255, 0, 0, 255, 0, 255, 0, 255]), bytes([0, 0, 255, 255, 255, 255, 255, 128])])
times = struct.pack("<2f", 0, 1)
moves = struct.pack("<6f", 0, 0, 0, 0, 1, 0)
joints = struct.pack("<16B", *([0, 1, 0, 0] * 4))
weights = struct.pack("<16f", *([.5, .5, 0, 0] * 4))
inverse = struct.pack("<32f", *([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] * 2))
parts = [positions, normals, uvs, indices + b"\0\0", picture + b"\0" * (-len(picture) % 4), times, moves, joints, weights, inverse]
views, binary = [], b""
for p in parts:
    views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(p)})
    binary += p
document = {
    "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 2]}],
    "nodes": [{"mesh": 0, "skin": 0, "children": [1]}, {"translation": [0, 1, 0]}, {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1]}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 7, "WEIGHTS_0": 8}, "indices": 3, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 0}}, "normalTexture": {"index": 0},
                   "alphaMode": "MASK", "doubleSided": True,
                   "extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1, "clearcoatTexture": {"index": 0}}, "KHR_materials_sheen": {"sheenColorFactor": [1, 1, 1]}}}],
    "textures": [{"source": 0, "sampler": 0}], "samplers": [{"magFilter": 9729, "wrapS": 33648}], "images": [{"bufferView": 4, "mimeType": "image/png"}],
    "buffers": [{"byteLength": len(binary)}], "bufferViews": views,
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
                  {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"}, {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
                  {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}, {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
                  {"bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC3"}, {"bufferView": 7, "componentType": 5121, "count": 4, "type": "VEC4"},
                  {"bufferView": 8, "componentType": 5126, "count": 4, "type": "VEC4"}, {"bufferView": 9, "componentType": 5126, "count": 2, "type": "MAT4"}],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": 8}],
    "animations": [{"channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}], "samplers": [{"input": 4, "output": 5, "interpolation": "LINEAR"}]}],
}
# (accessor 4 is the times, 5 the moves: their views are 5 and 6)
put("gltf", "skinned.glb", glb(document, binary))
document["buffers"] = [{"byteLength": len(binary), "uri": "data:application/octet-stream;base64," + base64.b64encode(binary).decode()}]
put("gltf", "embedded.gltf", json.dumps(document))
plain = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
         "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}]}],
         "buffers": [{"byteLength": 60}], "bufferViews": [{"buffer": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 12}],
         "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"}]}
put("gltf", "plain.glb", glb(plain, positions + indices))
sparse = {"asset": {"version": "2.0"}, "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAA="}],
          "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 6}, {"buffer": 0, "byteOffset": 8, "byteLength": 36}],
          "accessors": [{"componentType": 5126, "type": "VEC3", "count": 3, "max": [1, 1, 0], "min": [0, 0, 0],
                         "sparse": {"count": 3, "indices": {"bufferView": 0, "componentType": 5123}, "values": {"bufferView": 1}}}],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}], "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
          "extensions": {"KHR_lights_punctual": {"lights": [{"type": "spot", "intensity": 5, "spot": {"outerConeAngle": .5}}]}}, "cameras": [{"type": "orthographic", "orthographic": {"xmag": 1, "ymag": 1, "znear": .1, "zfar": 10}}]}
put("gltf", "sparse.gltf", json.dumps(sparse))
print("seeds in", out)
