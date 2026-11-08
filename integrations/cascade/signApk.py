#!/usr/bin/env python3
"""Makes and signs an APK where no JDK is at hand, so that apksigner cannot run. Signing is APK
Signature Scheme v2, which an app for a current Android needs; openssl does the RSA.

    signApk.py pack base.apk folder out.apk   adds a folder to an APK (libraries stored, to be mapped)
    signApk.py sign in.apk out.apk            signs an APK already aligned; nothing may touch it after

The key is made once, and kept beside the output. It is for tests: a store wants a key of your own.
"""
import hashlib, os, struct, subprocess, sys, tempfile, zipfile

CHUNK = 1 << 20
RSA_PKCS1_SHA256 = 0x0103
V2_BLOCK_ID = 0x7109871A


def pack(base, folder, out):
    entries = {}
    with zipfile.ZipFile(base) as z:
        for name in z.namelist():
            entries[name] = (z.read(name), name == "resources.arsc")
    for top, _, files in os.walk(folder):
        for f in files:
            path = os.path.join(top, f)
            name = os.path.relpath(path, folder).replace(os.sep, "/")
            with open(path, "rb") as h:
                entries[name] = (h.read(), name.startswith("lib/"))
    with zipfile.ZipFile(out, "w") as z:
        for name, (data, stored) in entries.items():
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            z.writestr(info, data)


def prefixed(data):
    return struct.pack("<I", len(data)) + data


def sequence(items):
    return prefixed(b"".join(prefixed(i) for i in items))


def contentDigest(sections):
    """The scheme's digest: of each 1 MB of each section, then of those."""
    digests = []
    for section in sections:
        for at in range(0, len(section), CHUNK):
            chunk = section[at:at + CHUNK]
            digests.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(digests)) + b"".join(digests)).digest()


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, check=True, capture_output=True).stdout


def sign(source, out):
    folder = os.path.dirname(os.path.abspath(out))
    key, cert = os.path.join(folder, "test.key"), os.path.join(folder, "test.crt")
    if not os.path.exists(key):
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "3650", "-subj", "/CN=Artful test")
    certificate = openssl("x509", "-in", cert, "-outform", "DER")
    publicKey = openssl("pkey", "-in", key, "-pubout", "-outform", "DER")

    with open(source, "rb") as h:
        apk = h.read()
    eocd = apk.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise SystemExit("not a zip")
    centralAt = struct.unpack_from("<I", apk, eocd + 16)[0]
    entries, central, end = apk[:centralAt], apk[centralAt:eocd], apk[eocd:]

    # what is signed sees the end record pointing where the signing block will start: as it does here
    digest = contentDigest([entries, central, end])
    signedData = sequence([struct.pack("<I", RSA_PKCS1_SHA256) + prefixed(digest)]) + sequence([certificate]) + prefixed(b"")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "signed")
        with open(path, "wb") as h:
            h.write(signedData)
        signature = openssl("dgst", "-sha256", "-sign", key, path)
    signer = prefixed(signedData) + sequence([struct.pack("<I", RSA_PKCS1_SHA256) + prefixed(signature)]) + prefixed(publicKey)
    value = sequence([signer])

    pair = struct.pack("<I", V2_BLOCK_ID) + value
    pairs = struct.pack("<Q", len(pair)) + pair
    size = len(pairs) + 8 + 16  # (the size counts itself once, and the magic)
    block = struct.pack("<Q", size) + pairs + struct.pack("<Q", size) + b"APK Sig Block 42"

    moved = bytearray(end)
    struct.pack_into("<I", moved, 16, centralAt + len(block))
    with open(out, "wb") as h:
        h.write(entries + block + central + bytes(moved))


if __name__ == "__main__":
    if sys.argv[1] == "pack":
        pack(*sys.argv[2:5])
    else:
        sign(*sys.argv[2:4])
