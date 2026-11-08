#!/usr/bin/env python3
# Writes src/metal/shaders.inc: the shaders Metal runs, in its language. They are the ones Vulkan runs, as
# tools/shaders.c saves them, put through SPIRV-Cross, which comes with the Vulkan SDK. This is a tool for whoever
# changes a shader: the library takes only what it writes.
#
#   build/artful-shaders dir
#   python tools/mkmetal.py dir path/to/spirv-cross [dir for .metal files, for Apple's compiler to check]
import os, re, struct, subprocess, sys

SHADERS = [  # name, stage
    ("cullAll", "comp"), ("sceneVertex", "vert"), ("sceneFragment", "frag"), ("sceneCutout", "frag"),
    ("overlayVertex", "vert"), ("overlayFragment", "frag"), ("traceShow", "frag"),
    ("shadowVertex", "vert"), ("shadowFragment", "frag"), ("trace", "comp"), ("traceGuides", "comp"),
    ("denoiseLight", "comp"), ("denoiseTime", "comp"), ("denoiseSpace0", "comp"), ("denoiseSpace1", "comp"), ("denoiseSpace2", "comp"), ("denoiseEnd", "comp"),
]

def named(code):
    """The same module, with each resource named after its binding: that is how they are found in what comes out."""
    words = list(struct.unpack("<%dI" % (len(code) // 4), code))
    at, bindings, firstAnnotation = 5, {}, None
    while at < len(words):
        length, op = words[at] >> 16, words[at] & 0xFFFF
        if op == 71 and words[at + 2] == 33:  # OpDecorate Binding
            bindings[words[at + 1]] = words[at + 3]
        if op in (71, 72) and firstAnnotation is None:
            firstAnnotation = at
        at += length
    names = []
    for target, binding in bindings.items():
        text = ("b%d" % binding).encode() + b"\0"
        text += b"\0" * (-len(text) % 4)
        names += [(2 + len(text) // 4) << 16 | 5, target] + list(struct.unpack("<%dI" % (len(text) // 4), text))
    words[firstAnnotation:firstAnnotation] = names
    return struct.pack("<%dI" % len(words), *words)

def entries(source):
    out = []
    for type_, name, index in re.findall(r"^\s+(.*?) (b\d+(?:Smplr)?) \[\[id\((\d+)\)\]\];", source, re.M):
        binding = int(re.match(r"b(\d+)", name).group(1))
        array = type_.startswith("array<")
        kind = 2 if name.endswith("Smplr") else 1 if "texture" in type_ or "depth" in type_ else 0
        out.append((int(index), binding, kind + (2 if array and kind else 0)))
    return out

def main():
    folder, cross = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    text = ["/* Made by tools/mkmetal.py, from the shaders Vulkan runs. Not to be edited. */\n"]
    table = []
    for name, stage in SHADERS:
        spv = os.path.join(folder, name + ".spv")
        marked = spv + ".named"
        open(marked, "wb").write(named(open(spv, "rb").read()))
        command = [cross, "--msl", "--msl-version", "30000", "--msl-argument-buffers", "--msl-argument-buffer-tier", "2", "--stage", stage, marked]
        # (Vulkan draws its maps with y the other way round from the frame; a Metal viewport cannot)
        if name == "shadowVertex": command.append("--flip-vert-y")
        source = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        os.remove(marked)
        if len(sys.argv) > 3:
            with open(os.path.join(sys.argv[3], name + ".metal"), "w") as f:
                f.write(source)
        found = entries(source)
        assert found or name == "overlayVertex", name
        text.append("static const char metalSource_%s[] =\n" % name)
        for line in source.splitlines():
            text.append('    "%s\\n"\n' % line.replace("\\", "\\\\").replace('"', '\\"'))
        text.append("    ;\n")
        text.append("static const MetalEntry metalEntries_%s[] = {%s};\n" % (name, ", ".join("{%d, %d, %d}" % e for e in found) or "{0, 0, 0}"))
        table.append('    {metalSource_%s, metalEntries_%s, %d},\n' % (name, name, len(found)))
    text.append("enum { %s, METAL_SHADERS };\n" % ", ".join("SHADER_" + n.upper() for n, _ in SHADERS))
    text.append("static const MetalShaderCode metalShaderCode[METAL_SHADERS] = {\n" + "".join(table) + "};\n")
    with open(os.path.join(root, "src", "metal", "shaders.inc"), "w", newline="\r\n") as f:
        f.write("".join(text))

main()
