#!/usr/bin/env python3
# The Metal shaderir/msl.c writes against SPIRV-Cross's, for the shaders Metal runs (tools/mkmetal.py's list): the same
# argument buffer (each resource's id, binding and kind, as mkmetal.py reads them for metal.c) and the same entry point
# (what it takes, what comes in, what goes out). The bodies differ, as two writers' do.
#
#   python tools/mkmetal.py --extract theirs      (shaders.inc's, SPIRV-Cross's)
#   artful-shaders ours --ir                      (ours)
#   python tools/msl/compare.py theirs ours

import importlib.util, os, re, sys

theirs_dir, ours_dir = sys.argv[1], sys.argv[2]
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
spec = importlib.util.spec_from_file_location("mkmetal", os.path.join(root, "mkmetal.py"))
mk = importlib.util.module_from_spec(spec)
saved = sys.argv
sys.argv = ["mkmetal", "--extract", theirs_dir]  # (it runs on being read: extracting again changes nothing)
spec.loader.exec_module(mk)
sys.argv = saved


def interface(src):
    entry = re.search(r"^(kernel|vertex|fragment) .*main0\((.*)\)$", src, re.M)
    params = sorted(re.findall(r"\[\[(\w+)(?:\((\d+)\))?\]\]", entry.group(2))) if entry else []
    part = lambda name: src.split("struct " + name)[1].split("};")[0] if "struct " + name in src else ""
    ins = sorted(re.findall(r"\[\[([^\]]+)\]\]", part("main0_in")))
    outs = sorted(re.findall(r"\[\[([^\]]+)\]\]", part("main0_out")))
    return params, ins, outs


apart = 0
for name, stage in mk.SHADERS:
    theirs = open(os.path.join(theirs_dir, name + ".metal")).read()
    path = os.path.join(ours_dir, name + ".metal")
    if not os.path.exists(path):
        print("%-18s: not written" % name)
        apart += 1
        continue
    ours = open(path).read()
    if mk.entries(theirs) != mk.entries(ours) or interface(theirs) != interface(ours):
        apart += 1
        print("%-18s: apart\n  resources %s\n        and %s\n  interface %s\n        and %s" % (name, mk.entries(theirs), mk.entries(ours), interface(theirs), interface(ours)))
print("%d of %d bind as SPIRV-Cross's" % (len(mk.SHADERS) - apart, len(mk.SHADERS)))
sys.exit(1 if apart else 0)
