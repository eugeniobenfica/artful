#!/usr/bin/env python3
# Checks the Metal that shaderir/msl.c writes, on a machine with no Metal compiler: each file through clang++
# -fsyntax-only, with tools/msl/metal_stdlib standing in for Metal's library. Vectors are clang's (ext_vector_type),
# which take swizzles as Metal's do but not constructors: floatN(...) and the like become make_floatN(...) first.
# What it finds is real (a name not declared, a type that does not fit, a statement malformed); what it passes is
# not proven: Apple's compiler is the check (CI).
#
#   artful-shaders dir --ir      (writes dir/*.metal)
#   python tools/msl/check.py dir [clang++]

import os, re, subprocess, sys, tempfile

folder = sys.argv[1]
compiler = sys.argv[2] if len(sys.argv) > 2 else "clang++"
here = os.path.dirname(os.path.abspath(__file__))
constructor = re.compile(r"\b(float|uint|int|bool)([234])\(")
failed = 0
names = sorted(n for n in os.listdir(folder) if n.endswith(".metal"))
with tempfile.TemporaryDirectory() as scratch:
    for name in names:
        source = open(os.path.join(folder, name), encoding="utf-8").read()
        source = constructor.sub(r"make_\1\2(", source)
        path = os.path.join(scratch, name + ".cpp")
        open(path, "w", encoding="utf-8").write(source)
        result = subprocess.run([compiler, "-std=c++17", "-fsyntax-only", "-Wno-unknown-attributes", "-Wno-unused-value", "-I", here, path],
                                capture_output=True, text=True)
        if result.returncode:
            failed += 1
            errors = [l for l in result.stderr.splitlines() if "error:" in l]
            print("%s: %d errors; the first: %s" % (name, len(errors), errors[0].split("error:", 1)[1].strip() if errors else result.stderr[:200]))
print("%d of %d pass" % (len(names) - failed, len(names)))
sys.exit(1 if failed else 0)
