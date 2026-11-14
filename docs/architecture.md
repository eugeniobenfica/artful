# How Artful works

What it does to be fast, and why. Numbers are from a Ryzen 3 3200G and its Vega 8 at 1920 x 1080.

## A scene is not a renderer

```
ArtfScene (plain memory)  ──►  ArtfRenderer (a device and a surface)
```

A scene holds meshes, textures, materials, instances, lights and a camera, and knows no backend. A
renderer draws one. The two are apart because a surface does not last: a phone takes it away, a
device is lost, a window moves to another GPU. The renderer goes and a new one comes; nothing has to
be loaded again. Each backend reads the scene as it is, and the GPU's copy is brought up to date from
version numbers, so a static scene copies nothing in a frame.

What the GPU reads is laid out for it in the scene (`ArtfInstanceData`, `ArtfMaterialData`,
`ArtfFrameData`), so that uploading is a `memcpy`.

## Meshes: all the work up front

`artfSceneAddMesh` does once what would otherwise cost in every frame (`src/scene/meshBuild.c`):

- **Triangle order for the vertex cache**: Tipsify (Sander, Nehab and Barczak, 2007), linear in the
  mesh. A test holds it below 0.85 vertices transformed per triangle, on a sphere.
- **Vertex order** by first use, so that memory is read forwards.
- **Quantizing**: 16 bytes a vertex instead of 32. Position as 16 bits of the mesh's box, normal
  folded onto an octahedron (Cigolle et al., 2014), texture coordinates as halves. The GPU's vertex
  fetch unpacks all of it for nothing. Colors are apart, 4 bytes a vertex, and only a mesh that has them
  pays for them: on the GPU they are read from a buffer by vertex index, and the others all read one white
  word, so most meshes keep their 16 bytes.
- **Meshlets** of at most 64 vertices and 124 triangles, each with a bounding sphere and a cone of its
  faces' directions. The CPU culls by these, and transforms a meshlet's vertices in the first level
  of the cache.

## What things are made of

A material says what a surface is, as glTF does: `metallic` and `roughness`. Light is Cook and Torrance's, with
Trowbridge and Reitz's distribution, Schlick's Fresnel and Smith's shadowing, in four places that must agree:
`addLight` in `kernels.inc`, the fragment shader, `trace.c` and the tracing shader. A fifth power is multiplied
out, so nothing here depends on a C library.

Lights' values have pi taken out, as they always had here: a white surface under a light of 1 shows 1. So the
distribution has no 1 / pi either. Light from all around is the sky and the ground: a diffuse surface takes it
by its normal, and a reflecting one by its mirror direction, which is what makes a metal look like one with
no environment map.

**Normal maps** need to know where u and v go on a surface. That is usually kept in each vertex, which would
take four more bytes from all meshes for the sake of a few. Here it is worked out from what is there: on the
CPU and in tracing from the triangle, once, and on the GPU from how the position and the coordinates change
between pixels. A map baked against a mesh's own tangents can differ slightly from this.

## Shadows when drawing

Tracing knows what is in the way. Drawing does not, so each light that casts looks at the scene from where it
is, and keeps how far it sees. A pixel then asks whether it is farther from the light than that.

All maps are squares of one atlas of depth, sixteen at most: `artfSceneShadowFrame` works out their matrices,
once for both backends, and a pixel's lookup is the same on both. The sun takes three, each a ball around a
longer stretch of the view; they move by whole texels, so shadows do not shimmer as the eye moves. A spot
takes one and a point light six, for the faces of a cube. All are read as nine texels around the place, each
in or out, which makes the edge soft by a texel or so.

What makes shadow maps wrong is that a surface shades itself: a texel covers a piece of it at more than one
depth. Here a pixel looks from a place lifted off its surface, by a couple of texels, more where the light
only grazes it. That moves shadows off what casts them by about as much, which is the price.

On the GPU a map is a render pass of depth alone, with all that casts in one indirect draw. The list of what
casts changes only with the scene, and only what has holes has a pixel shader. On the CPU a map is drawn in two
steps (`shadows.c`): triangles go into stripes of rows, in parallel, and each stripe fills its rows. Depth takes
the nearest, whatever the order, so threads change no byte.

## Levels of detail

With the `lod` feature, a mesh of 128 triangles or more gets up to three lighter versions, each a quarter
of the one before (`simplify.c`): edge collapse by quadric error, moving one vertex onto another so
that nothing new has to be made up. A vertex on an edge that only one triangle has stays put, which
holds borders, and seams too, as a seam is a cut in the mesh.

Each level is an ordinary mesh of the scene, so both backends draw it as they draw any other: all they
do is choose. An instance takes the lightest level whose error, projected at its distance, stays under
`artfSceneSetLodError`'s pixels. The error is measured, from each removed vertex to the surface that
replaces it, and added up along a chain of collapses: a quadric's own cost is a mean weighed by area,
and comes out far below what can be seen.

**Far away, many as one.** `scale.c` merges the instances of a cube of the world, of one material, into one mesh, from
each one's first lighter level through its transform, and makes that lighter again. Its error is what the levels had
moved, and what this moves. Once a frame, before any backend draws (`artfSceneScaleStep`), each merged mesh is drawn
past the distance where its error fits, never within its ball: what it holds is then at opacity 0, which every backend
leaves out of what is drawn, of shadows and of rays, and it is at 1. So the choice is one, made in one place, and costs
a scene that has none nothing. A choice that does not change touches nothing; one that does is an instance change, as
moving one is.

## Occlusion

With the `occlusion` feature, in two passes, on both backends (Haar and Aaltonen, 2015):

1. What showed last frame is drawn.
2. Its depth tells what it hides: a pyramid on the GPU, a farthest depth per 16 x 16 cell on the CPU.
3. Everything else in view is tested against that, as a cube around its sphere; what is not hidden is
   drawn.

What last frame got wrong costs time, never pixels: nothing that should show can be missing, even for a
frame, and a test holds it to the same bytes as drawing without. On the CPU the first pass keeps no
pixels (its depth is worked out again with the rest), so the feature costs that when nothing is hidden;
on the GPU it costs two render passes and the pyramid. And on the CPU a tile under an opaque overlay
counts as hiding everything, so an interface hides the scene as a wall does.

## Layers

What shows through breaks what makes both backends fast: on the CPU one surface a pixel, on the GPU one
draw in no order. So it is kept apart from all of that. An instance below full opacity, or of a mesh
with alpha in its colors, is a *layer*:

1. All that is solid is drawn as ever. Neither culling nor occlusion sees layers: they hide nothing.
2. `artfSceneLayers` lists those in view, the farthest first, by their centers. Both backends take this
   list, so they agree on the order. A scene with no layers does not look.
3. Each in turn takes its depth among what is there, and its nearest surface is blended on.

Step 3 is why a model fading out looks like a picture of it fading, and not like a wire frame of its
insides. It is what a layer in a compositor does, and it needs no sorting of triangles. It also means
that two layers cutting through each other do not mix: where the nearer one is, the other was drawn or
is hidden, by the order of their centers.

On the CPU layers take a last pass of geometry, after the solid ones. Tiles read the chunks in order, so
a tile finds a layer's triangles together and the layers in order, with nothing to sort. Its depth is
still in the cache from the solid pass: each layer rasterizes into it, shades what shows through the same
batches as everything else, and blends. On the GPU it is two draws a layer: depth alone, then color where
depth is equal, which is why the vertex shader's position is marked invariant. Those draws are the
CPU's part, so that part does grow, with the layers in view, and only with them.

Blending is in the frame's colors, as the overlay's is: what a page or a video editor does with the same
pixels.

## Holes and backs

A material can have holes (`ARTF_ALPHA_MASK`) or show its back. Those are solid: they write depth and hide
what is behind them. But a surface with holes cannot be told apart from one without before its texture is
read, and that does not fit what makes the plain case fast, so each backend keeps it apart.

On the CPU, rasterizing for depth asks the texture about each block of such a triangle, and a hole never
reaches the depth or the ids: all that comes after does not know. Only those triangles pay. A back is a
triangle of the other winding that is kept, with a bit that turns its normals around, and such an instance
does not use cones.

On the GPU a shader that can leave a pixel stops the GPU from testing depth before it, for everything
drawn with it. So plain instances keep their pipeline, and the culling shader sends the others to draw
commands of their own: four parts now, early and late, plain and not. A scene without such instances does
not touch them.

## Alpha

Frames are premultiplied, so a frame can be put over anything by the same blend the overlay uses. The
background's alpha is what starts a pixel: with 0, what is left is exactly what the scene covered. Layers
and the overlay then add to it, by the one rule `src + dst (1 - alpha)`, alpha included. A black hole is
light from everywhere and fills the frame.

## Four samples a pixel

`antialias` takes four samples a pixel, where Vulkan's standard puts them. Those places are multiples of
1/16 pixel, which is the grid the CPU's rasterizer is on, so its edges stay exact.

On the GPU it is plain MSAA: the samples live in the render pass and only their mean comes out, so on a
tiler they never reach memory. With occlusion, the pyramid's first level reads all four samples of each
pixel, and the early pass keeps them for the late one.

On the CPU each sample is a plane of a tile's depth and ids, rasterized in turn. Shading is what must not
grow four times, and it does not: the first plane is shaded as ever, and the others shade only pixels
whose id differs from the first's, which are the edges. The rest copy. As on the GPU, a surface is shaded
at the pixel's center even when only a sample of it is covered. The mean is taken in integers, so all
kernels still give the same bytes. A triangle that holds no pixel center is no longer dropped, if it may
hold a sample: thin things stay.

Samples are not free: 1920 x 1080 on the Ryzen, `spheres` goes from 13 to 20 ms, and `heavy`, where
triangles are all edges, from 13 to 30.

## Rays and paths

`artfSceneSetTrace` puts light followed along rays in place of triangles. It is the renderer's business, as
rasterizing is: the same scene goes in, and a frame comes out, with its alpha and under its overlay.

**Trees.** `trace.c` makes a tree of boxes for each mesh, cut by surface area in twelve bins, and one over the
instances, made again when they move. A ray goes down the top tree, is taken into an instance's object
space, and goes down that mesh's tree: so a thousand copies of a mesh cost one tree. Levels of detail have
none, as a ray does not care how far away a thing is.

**A path.** At each surface: light that reaches it directly, each point light and the sun, the same arithmetic
as `addLight` with a shadow ray added; then one bounce. For `ARTF_TRACE_RAYS` that is a mirror's, weighed by
`specular`, and what comes from all around is the rasterizer's ambient. For `ARTF_TRACE_PATHS` it is that or a
diffuse bounce, chosen at random, and what comes from all around is what a path finds when it leaves: so a
corner gets dark by itself. Past three bounces paths end at random, in proportion to what they still carry.

Holes need no special case. A surface that is only partly there lets a path through that share of the
time, and a shadow ray through that share of its light. So glass and fences work with the same code, and
layers that cut through each other are right, which drawing cannot do. With `ior` a path bends as it goes
through, by Snell's law, and turns back when it comes out too flat. Eye rays see only the fronts of what
has one side, as triangles do; a shadow's rays meet backs as well, or a wall would let the sun through from
behind.

**Samples add up.** A frame adds samples to float sums per pixel. A hash of the frame's data and the scene's
versions says when these no longer hold, and they start over. Each pixel draws its random numbers from its
position and sample index, so threads change nothing. There is no sine or cosine: a direction comes from
rejection in a ball, as C libraries do not agree on those.

**What was covered.** A path that meets nothing leaves the frame's background: sums hold light for covered
samples, and how many there were. So alpha comes out as how often something was there, edges are smooth for
nothing, and what was traced goes over the background or a black hole as a layer does.

**On the GPU**, one compute shader does all that, written out like the others (`artfShaderTrace` in `shaders.c`),
over the same arrays, sent up as they are. It needs no ray tracing extension: the driver this was written
on has none, and Vega has no such hardware, so this runs on any device that runs Artful. It is big, so it is
made the first time a scene asks. A test holds the two backends to the same picture, once enough samples
are in.

## Bones

A skinned mesh keeps its vertices at rest, with eight joints each and any targets, and each instance gets a mesh of its
own. A pose only records what was asked (`ArtfPose`) and checks that it fits the box the 16 bits of a position stand for:
each joint takes the corners of the mesh at rest, a vertex between joints is between those, and a box that is too small
grows, with room to spare. Vertices follow when something reads them. Vulkan's shader `skin` does it for what only bones
move, in a job per mesh, straight into the vertex buffer; the rest goes through `artfMeshDeform` on the CPU, as it does
for the software backend, Metal and rays, which refit their part of the BVH. 2,000 instances of 520 vertices cost 0.3 ms
to pose and 2.5 ms to draw on a Ryzen 3200G. `artfAnimationSet` samples glTF's clips at a time the program gives;
blending two is the program's.

What bends has levels too. Collapses keep the vertex they move onto, so a level's vertices are some of the whole's, and
each takes its joints, weights and targets along: a level is built from the floats the mesh was given, with its own
skin. Moving a vertex onto one that bones or targets move otherwise costs more (the weights that differ, as a twentieth
of the mesh; targets, by how far apart they move the two), so the cut between bones goes last. An instance has a copy
of each level; once a frame `scale.c` picks the one its distance calls for, by the mesh at rest, as artfMeshLevel
would, and makes it the instance's mesh, with the pose copied over. Only that copy is bent, so a far crowd bends a
quarter of the vertices or less, on the GPU's `skin` shader as on the CPU. The backends never see a level of what bends
as one: it is the instance's mesh.

## Shadows, levels and layers

Each shadow map has a key: its view and what is not an instance. Each change to an instance leaves a ball where it was and
another where it went, the last 1,024 of them. A map whose key is the same, and that no new ball reaches, is not drawn, on
either backend. The GPU's `shadowCull` goes over casters and views at once, with the test the CPU makes, and
fills each view's commands, so a map draws only what can reach it.

Levels of detail treat vertices at one place as one. A seam is two such vertices, and both sides collapse along it
together, onto a pair: the cut stays closed and the sphere of 12,000 triangles goes down to 192. What fans out around a
point, as at a pole, becomes one vertex. With `lod-fade`, a mesh on its way between levels is drawn twice, and an 8 x 8
pattern gives each pixel to one of the two, by how far along it is.

On the GPU a mesh of 512 meshlets or more goes through `meshletCull`: a command for each that stays, which the frame then
draws by a count the shaders leave. With occlusion each meshlet of each instance remembers whether it showed: those go first,
and the rest are tested against the pyramid that left, as whole instances are.

With `layers-sorted`, a layer's pixels go to a list of eight for each pixel, against what is solid. Then, on the GPU in
`layerResolve` and on the CPU at the end of a tile, each list goes over the frame in order, keeping of each instance
only its nearest. With antialiasing the CPU keeps a list a sample; the GPU keeps one a pixel, with the samples each
fragment covers, and resolves sample by sample, against the depth of what is solid at that sample.

## Materials in C

A graph is a list of steps on four floats, each naming the steps it reads: `artfExprMul` adds a step and returns its
number, and so a C function that composes calls is all a program needs in place of a language. Each backend turns the
list into what it runs. Vulkan writes it into the shaders as SPIR-V, one branch a graph, and makes the pipelines that
hold them again when the graphs change. The CPU interprets it a step at a time for eight pixels (`artfGraphRunLanes`), so
what it costs to find out what to do is paid once for all of them; a ray takes one lane of the same code. A texture read
in a graph picks its mipmap from what its coordinates do from one lane to the next, whatever the graph did to them. The
two are held to the same picture.

## Blocks

`texture.c` reads KTX2, and keeps blocks as they came. On the GPU they go up as they are, if the device takes the format;
if not they are decoded. `compressTextures` goes the other way: plain pixels are encoded to BC1, BC3 or ETC2, whichever
the device takes, leaving out normal maps. The CPU decodes on first drawing, to whole pixels, not sample by sample.
`blocks.c` holds BC7 and ASTC, in integers, with ASTC in all its 2D footprints. They were checked block by block against
a decoder of someone else's, on blocks of noise: 65,536 of BC7 came out alike, and 46,000 legal ones of ASTC within one
of 255, since that decoder rounds a step sooner. A hash of each keeps them so in the tests.

## What comes and goes

Removing a mesh gives its stretches of the GPU's buffers back to a list of spans (`spanTake`, `spanGive`), and what is
added next looks there first. A texture's slot is reused, and a generation in its handle tells the old from the new.
What is slow in adding a mesh or a texture, `artfPrepareMesh` and `artfPrepareTexture` do with no scene at all, on any
thread; the add that follows is short.

Residency is how many mipmaps of a texture the GPU holds. `artfSceneResidencyStep` runs in every draw, with the scene's
view. Manual does nothing. Assisted reports what it would do, and the program calls `artfTextureSetResidentMip`.
Automatic does that itself, within a budget. Main memory keeps all the mipmaps, so coming back costs an upload and not
a read.

Meshes have levels held the same way (`ArtfResidencyDesc.meshes`): of each, from the heaviest its instances need, at
half their distance, to its lightest, which always stays. A level not held is a mesh of another generation with no
triangles on the GPU, so the backends give its room back as they do for a mesh removed, and bring it back the same
way. What chooses levels sees a level not held as having no error: `artfMeshLevelError` is 0 for those before the first
held, and the CPU, the GPU's culling shader and Metal all read it, so all draw the next level held. Shadows go by
`artfMeshHeld`. A mesh added with a way to read it (`artfSceneAddMeshStreamed`) drops its levels from memory too, and
reading it again makes them again, the same bytes, so a picture before and after is the same.

## Taking out noise

A traced frame with `denoise` keeps, for each pixel, what its first surface was: where, which way it faced, and its
color. Light divided by that color is what is noisy, and what is smooth across a surface, so a texture stays sharp
through all this. `denoise.c` and its shaders do the same arithmetic.

In time, a pixel asks where its surface was in the frame before, with the last view and projection and how its object
moved (the guide says which instance it is, and `motion` holds, for each, from where it is to where it was). It checks
that the same surface is there: on the same plane, and facing the same way. What was gathered then has to agree with this
frame's light: with the pixel's neighbourhood, by its mean and deviation, and, for changes too small to show in a noisy
pixel, the mean of that neighbourhood with the mean of what was gathered around it. What does not is cut back to eight
frames, so a light that goes out is seen at once, on a surface that never moved.

Where all that holds, it adds to what was gathered there, up to 256 frames; if not, it starts over. So the eye and
objects can move and the picture stays. What bones bend goes by its vertices: each knows where it was a pose ago, and the
guide says how far that takes the point a ray landed on.

In space, three passes of a wavelet filter (Dammertz et al., 2010) read five taps a side, each twice as far as the one
before, weighing neighbours by distance from the eye and by the way they face, so edges hold. A pixel that has gathered
48 frames needs no help, and is left alone: what has settled is not blurred.

With four samples, the picture is half as far from one of 512 samples as without, on each backend, and a frame after
the eye moved it stays about there. The two backends differ in 0.25% of their pixels by more than 24 of 255.

## Reading files

`gltf.c` is on top of the public API, and nothing else depends on it. Its JSON reader makes one array of nodes
that point into the text, so a file costs little more than itself. A file's numbers are not trusted: each
index and each byte range is checked before use, and a test runs it on every cut of a good file and with
each byte broken in turn.

`imageDecode.c` is cascade's decoder for PNG and JPEG, copied, as linking it is what Artful must not do.

Lights (`KHR_lights_punctual`) and cameras stay with the model, so that they follow their nodes when a clip
plays or the model moves. A light of the file fades by the square of distance and Artful's end at a radius: the
range given, or where the square law would have come down to a twentieth, as bright as the square law at a
quarter of it. `KHR_texture_transform` goes into the vertices, as a material has one set of coordinates.

Loading a model reads the file on the calling thread, and then decodes its images and makes its meshes, with
their meshlets and lighter versions, on all cores, each on its own: what comes out is what one thread would have
made.

## A frame in floats

With `ArtfRendererOptions.hdr` the scene draws light as it is, not as a screen shows it, and what a lens and a film do
comes after, on the whole frame, in `post.c` on the CPU and `postShaders.inc` on the GPU: the same passes, in the same
order, with the same arithmetic, on planes laid out alike.

The CPU's tiles keep each sample's light as RGB9E5 (three nine-bit mantissas and an exponent: the same 32 bits as a
pixel of bytes, so no tile grows), with the sample's alpha and surface after it in the same buffer, and leave three
planes for the frame: light (premultiplied), depth (1 / w, of what is solid: layers write theirs after it is taken)
and a surface word: the normal folded onto an octahedron (9 + 9 bits), roughness (5), metal (4) and the share of the
pixel's light that came from all around (5). The GPU's passes draw into RGBA16F and an R32_UINT beside it, and a compute
shader gathers both, with depth taken between the solid pass and the layers', into buffers.

| Pass | Where | How |
| --- | --- | --- |
| Ambient occlusion | every pixel | Eight points in a hemisphere around the normal, turned by a 4 x 4 pattern and tested against depth; blurred over 4 x 4 by depth; taken only from the share of light that came from all around. |
| Reflections | half the pixels | The mirror ray projected once and walked as a line across the frame, 1 / w even along it, in 24 steps closer near the start; a hit is a step from in front to behind, near enough. What it shows takes the place of what the surface was lit with from all around in that direction (the picture, or the sky's two colors), by Fresnel, smoothness and the frame's edges: where it finds nothing, that light stays. Then taken up to every pixel. |
| Temporal antialiasing | every pixel | The eye moves within a pixel by Halton's (2, 3) points; each pixel goes back to where it was by its depth and the frame before's view, and what it finds there is kept within what this frame shows around it, nine to one. |
| Upscaling | the frame's pixels | With `renderScale`, in place of the above: of the four samples around a pixel's center, those within a pixel of the frame give as much as their nearness, over what it had (followed, and kept within the nearest sample's neighbours); from here on all is the frame's size, and depth and surface are read at the nearest sample. |
| Motion blur | every pixel | McGuire et al.'s: velocities, the largest of each tile of 16 and of its neighbours, then 12 points along that, each weighed by whether it is in front and moves over the pixel, or is behind while the pixel moves, or both. |
| Depth of field | every pixel | 24 points of a disc as wide as the circle of confusion; what is sharper and behind does not spread over what is in front. |
| Bloom | a pyramid | What is past the threshold, after exposure, down six halves with a tent and back up. |

Exposure and tone mapping come last, then the overlay, which on the GPU is drawn in a pass of its own over the frame.
With four samples a pixel the light is their mean, and what comes after reads the first sample's depth and surface,
on both backends. Layers in a frame in floats go by their centers, not by each pixel's depth.

**Fog that light goes through** is not a pass after the frame, but a grid before it: cells 16 pixels square, in 64
slices along the view that end at far x ((k + 1) / 64)^2, far being 4 / density, so that they are thin near the eye.
Each column goes from the eye out through its slices, each lit at its middle by the sun through its shadow maps with
Henyey-Greenstein's phase, and by the fog's color, less what the air takes (the same rational curve as the fog drawn
with the frame); each cell keeps the light added from the eye to its end, and what is left of what is behind. Every
surface reads it where it is, as it is lit (trilinear: the CPU's `artfFroxelAt`, the GPU's `froxelAt`), layers too, and
what is behind the scene reads the last slice. So smoke in front of the sky is fogged at its own depth, and the grid
costs what a quarter of the frame's cells did, without the march at each pixel. The CPU builds it on its workers after
the shadow maps, the GPU in a compute pass between them and the frame.

## Lights past sixteen

The frame holds 16 lights, as it always did: the first that are on, which may cast shadows. Those past them, up to 1024,
go into lists: for each 64 x 64 square of the frame, those whose sphere reaches the square's slice of the view (four
planes through the eye). `artfSceneLightTiles` makes them on the CPU for both backends, so both light the same pixels
with the same lights; the CPU's tiles are those squares, and the GPU's pixel shader finds its square from its place.
Tracing leaves them out. Of those that ask for shadows (`artfLightSetShadow`), the 32 that matter most each frame, by
how wide they are against how far from the eye, and in view, get maps in the atlas a quarter as wide as the others,
in rows below them: 48 such maps, one a spot and six a point light. They go first in the list, in that order, and the
shadow frame keeps, for each of those first places, where its maps are: both backends look them up as they do the
sixteen's.

## Light that bounced

`artfSceneSetIndirect` puts a grid of probes over a box. Each frame each probe sends a few rays the same random ways
(from the frame's number: the same on every processor) through the trees tracing uses; each comes back with the sky,
or a surface's own light, what the sun and lights give it (with shadow rays) and what the probes gave it the frame
before. What comes back makes a first-order spherical harmonic, which a surface reads as the mean and two thirds of
its lean along the normal (Ramamoorthi and Hanrahan), mixed with what the probe had. So light bounces once more each
frame. The probes are made on the CPU's workers for both backends, and the GPU reads them from a buffer: both draw the
same. A surface takes light from all around from the eight probes nearest a point a quarter of a cell out along its
normal, in place of sky and ground or a picture.

Each probe keeps how far it sees as well: the same rays, into an 8 x 8 map of directions on an octahedron, each
texel the mean distance and the mean of its square over a narrow lobe (the cosine to the 32nd). A point a probe cannot
see is told by Chebyshev's inequality on them, as Majercik et al.'s probes do, and takes next to nothing from it; a
probe behind the surface counts less. Inside a closed box under a bright sky, a point by the wall takes 0.1% of what
is just outside; with one mean for each of six sides it took 11%. The CPU reads the probes eight pixels at a time,
with the same arithmetic in the same order as `artfIndirectAt`, so the bytes are the same on every processor.

The GPU makes its pixel shaders with what the scene uses: probes, their maps, the fog's grid. A branch that is never
taken still holds registers, and fewer pixels run at once for it: with all of them, `--scene spheres` took 10% longer.
When one of them is turned on or off, the shaders are made again, as they are for new materials in C.

## Particles and terrain

**Emitters of many** (`parallel`) are a buffer of particles, twelve floats each, and no instances. What makes the GPU
and the CPU agree, and threads not matter, is that nothing depends on order. At each step the scene decides where the
born go: places around the buffer, one after another from where the last went, as many as are free; it knows when each
dies from its birth, without the particles, and keeps the step as eight rows of numbers. A particle's random numbers
are a hash of its birth's number. Each place then moves alone: born if the step put a birth there, then gravity, drag,
noise, the ground and spin, as `artfSwarmMove` says, which the compute shader `swarm` does too. A backend keeps its own
copy of the particles, and applies the steps it has not seen, up to 120 of them: the CPU on its workers, by stretches
of places, all steps for each; the GPU in one dispatch an emitter, each invocation a place. The same invocation turns
its particle to the eye and writes it as an instance of the quad, with the material of its age and its opacity, into
the frame's instance buffer, past the scene's, and its copy into the visible list: one indexed draw of the quad then
draws them all, through the pipeline small particles use. The CPU makes the same instances and draws them as layers,
each its own: after all other layers, in the order of their places, on both.

Neither is a backend's: an emitter's particles are instances of one quad, which `artfEmitterStep` moves and fades
(through eight materials between the first color and the last, and opacity), and terrain is a mesh. Each backend
turns the particles to the camera as it draws (`artfSceneFaceParticles`), when the camera or they moved. Particles are
layers: drawn after what is solid, the farthest first. On the GPU those that follow each other are one draw, in a
pipeline that tests and writes depth as each goes (a quad has one surface, so this is what the two passes of a layer
do); on the CPU a layer clears and reads only the rows of the tile it reaches. They are marked, and left out of the
trees tracing and probes use, and of what moves shadow maps, unless the whole frame is traced.

## Decals

`artfSceneAddDecal` lays a picture over what is drawn, through a box. Each frame the decals in view go to both
backends as five rows each (from the world into the box, color and opacity, texture, roughness and the mipmap to read,
chosen once for the box's distance); as each pixel is lit, those whose box holds it put their color over the
surface's, by the picture's alpha, fading toward the box's ends and on what does not face up it. What blends takes
none.

## Light from all around

`environment.c` makes a picture of all around (equirectangular, as `.hdr` files are) into maps of an
octahedron around +y, folded below. A direction falls on one by additions and a division: no angle to work out,
so the CPU and the GPU find the same texels, and the shaders need no `atan` or `acos`. Each map has a border of
one texel copied from across the fold, so bilinear sampling needs no care at the edges.

There are six, in one array of floats that the GPU reads as a storage buffer (binding 49):

| | Side | |
| --- | ---: | --- |
| 0 | 512 | The picture itself: mirrors and the background. |
| 1 to 4 | 64, 32, 16, 16 | What surfaces of roughness .25, .5, .75 and 1 reflect: each texel the sum of what GGX's lobe around it gathers from a smaller map, the eye taken to look along the normal. |
| 5 | 16 | What a surface facing each way takes from all around, over pi. |

A surface reads the last for its diffuse light, and the two maps around its roughness in the mirror direction,
weighed as the sky and ground were. Traced rays read the first where they leave the scene. Making the maps reads
every texel of a smaller map for each texel of a larger one, on all cores, with no sine or power from the C
library: about 40 ms for a picture of 512 x 256, and the same bytes on every processor.

## The look

Last, before the frame is bytes (`finalRow`, `artfShaderPostFinal`): white balance and saturation in light, a vignette,
then the tone curve, then contrast in what a screen shows. Each is skipped at 0, so a frame that asks for none is the
same bytes as before. Chromatic aberration reads red and blue a little apart, toward the corners.

Auto exposure meters the frame on a grid of 32 x 18 points: the mean of their light in stops, the middle weighed twice
the corners and each by what covers it. log2 and 2^x are worked out from a float's bits and a short polynomial
(`artfLog2Fast`, `artfExp2Fast`), so that the CPU and the GPU agree and no C library is asked. The exposure goes a share
of the way to the key each frame, and the state lives past bloom's levels in its buffer on the GPU, where one
invocation meters before bloom reads it.

With a histogram (`exposureLow`, `exposureHigh`) the grid is 64 x 36, and each point's weight, and weight times stops,
go into one of 64 bins as whole numbers (of 1 / 65536 and 1 / 4096), so that a sum does not hang on the order it is
made in: on the GPU, the 64 invocations of one group each take a column and add to bins in shared memory at once; on
the CPU, one thread in turn; the same sums. Then one walks the bins, and the mean of what is left between the shares
left out is what is aimed at, as Unreal's is. The exposure goes down at a speed of its own.

After the tone curve and contrast, in what a screen shows: lift, gamma (2^x and log2 as above) and gain; three color
wheels, each as much as a pixel's luminance l makes it shadow, middle or highlight ((1 - l)^2, 2 l (1 - l), l^2); a
3D look-up table, between its eight nearest points; then grain, a hash of the pixel and the frame's number. On the
GPU the table lives past auto exposure's state in bloom's buffer, copied there from a buffer the CPU writes when it
is another, so no binding is spent on it. The lens flare reads bloom's last level again at three places mirrored
across the middle, each its own color, gone at the frame's edges.

## What moves

Each renderer keeps, by instance handle, where each instance was at its last frame (`motion.c`): from that, for each,
the matrix that takes a point from where it is to where it was. The CPU's tiles leave which instance shows at each
pixel beside its depth; the GPU's scene pass writes it beside the surface word, as a second word, which the target
has only while temporal antialiasing or motion blur is on (its passes and pipelines are made again when that changes).
Temporal antialiasing then takes a pixel back through its instance's matrix and the frame before's view, and motion
blur spreads it along that way. When nothing moved, neither reads it.

What a pose bent goes by its vertices. On the CPU, the motion keeps, of each instance that bends, where its mesh's
vertices were at the last step, and the tile works out, at each of its pixels, where the point it shows was: its
triangle's corners where they were, through the instance as it was and the view before. On the GPU what the vertex
buffer held is copied before the bones and targets move it, and the vertex shader passes the same down to the pixels,
which interpolate it. Either way the second word is then that place, from where the pixel is, in quarter pixels
(`ARTF_MOTION_VECTOR`), and the instance's top bit says so.

## A clear coat and sheen

A coat is a second highlight, as addLight's but of no color, F0 .04 and its own roughness and normal, on top of
everything: what its Fresnel reflects toward the eye is taken from what is under it, sheen included. Sheen is Charlie's
(Estevez and Kulla): a distribution of sin(h)^(1 / alpha), with Neubelt's visibility, pi put back as Lambert's light has
it taken out; the power is 2^(log2 x / alpha), by `artfExp2Fast` and `artfLog2Fast`, the same on the CPU and the GPU.
From all around it takes a share of what lights the surface, by a fit to its lobe. Their textures are read where the
material's are.

They are kept apart from `ArtfMaterialData` (Metal's shaders read that one as it was), in `ArtfMaterialMore`, eight rows
after all of them in the material buffer, with the rest of what glTF's extensions say; the GPU builds the shaders that
read them only for a scene that has some (`ARTF_SHADER_COAT`, and a tracing shader of their own), and the CPU has a copy
of its lighting with them and one without, so that what has none pays nothing, registers included.

## More of glTF's materials

**A specular color** tints what a dielectric reflects head on. **Highlights drawn out** (anisotropy) are Trowbridge and
Reitz's distribution with two alphas, along and across a direction turned from where u grows: glTF's alpha mixed
toward 1 along it. Where u grows is worked out as for normal maps: from the triangle on the CPU and in tracing, from
how the coordinates change between pixels on the GPU. **A thin film** (iridescence) takes the place of what the
surface reflects head on: two beams, off the film's top and its bottom, add or cancel at red, green and blue's
wavelengths by how far apart they go, seen along the normal; Schlick's Fresnel goes on from there. Its cosine is
`artfCosFast`'s polynomial on both sides. **What glass takes**, traced: a path that comes out of a thing keeps
2^-(absorb x the distance it went inside) of each color, absorb being -log2 of the color it keeps over the distance
that keeps it.

## Light under the surface

Skin, wax and marble let light in, and give it back elsewhere. Drawn, that is two cheap things for each light. It
wraps: (n.l + .5) / 1.5 in place of n.l, so light goes half a radian past where the surface turns from it, the more
the closer to that edge, in the color under the surface. And it comes through from behind: where the light is behind
the surface, as much as is left of it after the way it went inside, 2^-(thickness / radius), thickness being how far
this place is past what the light's shadow map says it met first. That is the sun's maps and the sixteen lights';
both backends read the map's texel the same way (`artfShadowThroughSun`, `depthThroughOf`). Traced, a ray from just
under the surface toward the light finds how thick the thing is, and a shadow ray from where it comes out what stands
past it.

## Lights with a shape, and profiles

A tube is a line with a ball around it; a rectangle has a length, a width, and a side it lights. Their diffuse light
comes from their point nearest the place, as far as the light reaches from it, and a rectangle's less to the side as
it faces away (its cosine there); their highlight from their point nearest the mirror ray: on a tube, the point of
the line nearest it, then the ball's; on a rectangle, where the ray meets its plane, kept on it. That highlight is made
longer, and wider, by as much as the shape looks long and wide, and dimmer by as much (Karis, 2013), so that its light
in all is a point's. It is `artfShapeLight`, one function in C that the CPU calls lane by lane, and the shaders as the
same arithmetic. Traced, each path aims at a point of the tube or the rectangle, so shadows go soft. Shadow maps are
drawn from the middle, and soft shadows are as wide as the shape is long.

A profile, from an IES file (`artfIesDecode`), is a table of how much light goes each way: by the angle from the light's
direction (or down), and round it. It is kept as 96 x 16, read bilinearly; the angles come from polynomials for acos
and atan2 (`artfAcosFast`, `artfAtan2Fast`), so the CPU and the GPU agree. The GPU's tables go in the lights' buffer, past
those past the sixteen; the frame says, for each of its lights, its shape and where its profile starts. Lights with
neither pay nothing: the CPU's lights with a size, a shape or a profile go through the copy of its lighting that has
them, and the GPU's through shaders built for a scene that has some (`ARTF_SHADER_SHAPES`).

## A sky

`sky.c` follows each direction from a point a little above the ground, in 24 steps through air (Rayleigh's, a scale
height of 8 km), haze (Mie's, 1.2 km, Cornette and Shanks's phase) and ozone (a tent at 25 km), and at each, 8 more
steps toward the sun for what reaches it. That is light scattered once, in Artful's units: the sun's color times pi
times what the air sends toward the eye. Below the horizon the ground, lit through the air, is seen through it. The
picture becomes the scene's environment, so nothing of it is a backend's.

Asked for more, it goes as Hillaire's (2020) does, with two tables made first, on all cores. What the air leaves of
the sun, by height and angle, in 256 x 64 (Bruneton's mapping: across, how far the top of the air is, so that the
horizon keeps its texels), each texel a march of 40 steps: read in place of the 8 steps toward the sun at each step,
and nearer the truth than they were. And light scattered more than once, in 32 x 32, by height and the sun's angle:
from each texel 64 ways, light scattered once with a phase the same all ways, and how much of what comes goes on
scattering, f; every further order is taken to scatter as the second, so all of them are the second over 1 - f. Each
step toward the eye then adds what the air there scatters of that. In twilight, where most of the light has scattered
more than once, the sky's blue is 1.7 times what it was, and the earth's shadow shows. The sun drawn is ten times its
light as it reaches the eye, darker toward its edge (Neckel and Labs's, by channel): bright enough to bloom, not so
bright that a traced path meeting it is a firefly; its light on the scene is still the sun's.

Clouds are a layer between two spheres, followed from where a direction enters it, in 64 steps over at most 30 km,
each texel's first step a little apart by a hash so that steps do not band. What is there comes of two tiled noises,
made each time from a hash, value noise in octaves, the finer ones billowed (1 - |2n - 1|): where (a map 60 km
across, which coverage moves up and down) and what shape (a volume of 64^3 over 12 km, rounded below and thinner at
the top, worn at its edges by itself again at a fifth the size). Each step looks six times toward the sun, in steps
that grow, for what the cloud above it takes; the sun's light then comes through by Beer's law, in three octaves for
what scatters many times (Wrenninge's: each half as taken and half as forward), with powder at the edges that face
away from the eye, and the sky's light, more toward the top. What they leave of the sky behind is seen through the
air up to where they mostly are, and past 90 km they fade into it. All of it is the picture's, so tracing meets them
as it meets the sky.

## Lights with a size, and soft shadows

A light with a size is a ball. Its diffuse light comes from its middle; its highlight from the point of the ball nearest
the mirror ray, made wider by as much as the ball looks wide, and dimmer by as much, so that the light in all is a
point's (Karis, 2013). Traced, each path aims at a point of the ball.

Soft shadows look in the map around a pixel for what is in the way (25 texels over 12), take the mean of their depths,
and from how far that is from the pixel, against how far from the light, the penumbra's width: for the sun, its angle
times the distance; for a lamp, its size times how much nearer the blocker is (Fernando's PCSS). 25 texels over that
width are each in or out.

With temporal antialiasing that is 13 reads: 2 x 2 to look and 3 x 3 to filter, each in the middle of its square of the
area and moved within it to one of 16 places, by a 4 x 4 pattern over the pixels that turns each frame; the frames
gather them. Reads on rows of the map cost more than reads: a disc turned at random reads a row each, so these keep
to five.

## The ray tracing hardware

With `ArtfTraceDesc.hardware` on a device that has ray queries, the tracing shader asks the hardware where a ray meets
the scene, in place of walking the trees, and all that follows is as it was. Each mesh gets a bottom level from the
triangles the trees were made from, in the same order; the top level holds the traced instances in their order, so an
instance's id is its place among them, and the table of instances the trees use gives its mesh's first triangle (in
the word that says whether it has two sides, past bit 0: so no limit of a custom index's 24 bits). Fronts are
counter-clockwise, as the trees have them, which is the hardware's own way with no flag; eye rays cull backs but for
what has two sides; everything is opaque to the hardware, as what shows through is the path's business.

What bones or targets move each frame has its bottom level built again, in place, alone: the rest stay, and the top
level is built again over them. The hardware's refit (an update of a level built to allow it) was tried, and on a
Radeon RX 9060 XT it left shadows of what was not there, where building again draws as a level built from nothing does
(`testTracedMoved`).

## Rays while drawing

Unreal's ray traced shadows and reflections: the rasterizer draws, and asks rays only what maps and the frame cannot
know. Both read tracing's trees (`artfSceneTraceSync`, without particles), kept while drawing as instances move, and on
the GPU through the hardware where there is some; on a GPU without, the pixel shader and the pass walk the trees
themselves, with the tracer's code. The CPU's `artfTracedShadow` and `artfTracedReflection` (`trace.c`) and the GPU's
`tracedShadowSpv` and `artfShaderPostReflectionTraced` do the same arithmetic, so the backends draw the same picture,
and the hardware the same as the trees.

**Shadows** (`ArtfShadowDesc.traced`): for the sun and each of the sixteen lights that casts, where it can reach the
surface at all, a ray from a point lifted off the surface (by a ten-thousandth of how far it is from the origin, and a
thousandth of how far from the eye, as what is drawn far away may be a lighter level than the trees hold) toward it, through
`lightThrough`: what is in the way stops as much of the light as it is there, so glass and fences let light through as
they do when traced. A light with a size takes four rays, at points of a ball of its size (for the sun, of its size as
an angle): sixteen points, four by four, and each pixel of a 2 x 2 square takes its four, so that the square takes all
sixteen; with temporal antialiasing the four turn each frame, and the history mixes them. The pixel shader that does
this is made only for a scene that asks (`ARTF_SHADER_TRACED`, with `ARTF_SHADER_RAY_QUERY` for the hardware); the
sixteen take no maps, and the sun keeps its maps only for fog that light goes through.

**Reflections** (`ArtfPostDesc.tracedReflections`): in place of the march, at the same half resolution, a ray from the
surface the frame shows, along its mirror, strayed by a point of the same ball times the square of its roughness, finds
what is there, off the frame or behind something too. What is mostly not there (alpha under one half) is gone through.
What it finds is lit simply, as Unreal's reflections are: its color, and what it reflects head on, under light from all
around, the sun and the sixteen lights, with no shadows; and its own light. That takes the place of what the surface was
lit with from all around in that direction, as what the march found did, but sure everywhere: there are no frame's edges
to fade toward. Where it finds nothing, that light stays.

## Views, and what passes take

A view (`artfRendererSetView`) is worked out where light is: at the end of `lightLanesAs` on the CPU (`viewLanes`) and
of the scene's pixel shader on the GPU (`viewLightSpv`), in the same order, from what lighting already has there. What
it shows comes out as light, the square of the color wanted, so that the frame, in bytes or in floats, shows that
color; `artfFrameForView` takes from the frame all that would change it after (exposure, fog, the passes after the
frame) and makes what is behind black, once for both backends. A number becomes a color by a hash, the same in C and
in SPIR-V.

What is not in lighting is found beside it. On the CPU each triangle's setup says what it is colored by (its level,
meshlet or instance), and the batch carries it for each lane; on the GPU the vertex shader hands on the whole word it
reads of the mesh drawn, and the meshlet is found from `PrimitiveId` in a table of each triangle's, made when the view
is first asked. Overdraw is not lighting at all: on the CPU each tile counts, for each sample, the triangles binned to
it that cover it, with the rasterizer's edges; on the GPU a pipeline with no depth test adds a step for each.

Off, a view costs nothing: the CPU's plain copy of the lighting is the one it always was, and the views live in the copy
that is called (`lightsApart`, with lights that have a size); the GPU makes its pixel shaders with views only while
one is asked (`ARTF_SHADER_VIEW`).

What passes take: the CPU marks the clock as each phase ends (`artfPassMark`); the GPU writes a timestamp at the bottom
of the pipe after each pass, 33 a frame in flight, and reads them when the frame's fence says it is done.

## An orthographic camera

`ArtfCamera.orthoHeight` sends every ray the same way, each from its own place on the view's plane. Its matrix has w
1, so depth cannot be `near / t`: it is `(far - t) / (far - near)`, 1 at near and 0 at far, and even across a
triangle, as all that is interpolated is then. Far is just past the scene's bounds, so all of it has a depth above 0.
A perspective's depth is as it was, to the bit, and so is all that reads it: the frame says which camera it is
(`ArtfFrameData.ortho`), and where the two part, the code asks.

What changes is in a few places, the same on both backends. Depth becomes distance by `artfDepthAlong`; the eye a
point is lit and fogged from is the place on the view's plane behind it (`artfEyeOf`, the shaders' `eyeOf`), so the
view and the distance along it come out right with no other change; and what goes by size on screen takes every
distance as 1 (`artfSeenDistance`), as focalY is then 2 / height. The CPU's rasterizer keeps the clip's z as a
pixel's depth in place of 1 / w, and cuts at z = 1 in place of w = near. A meshlet's cone is tested against the one
way the view looks; the sun's cascades are boxes along the view, and the squares of the frame that list lights are
boxes too. After the frame, `worldAt` and `postWorld` start each pixel from its place, and reflections walk depth
itself along their line, as it goes evenly there. A traced ray starts on the plane (`ArtfTraceFrame.eye[3]`). Where
the GPU must choose, it multiplies by a 0 or a 1 of the view rather than choosing between results: what a perspective
computes stays as it was, and so does what the GPU's compiler fuses of it.

## Picking

`artfScenePick` and `artfSceneRaycast` go down the trees tracing makes (`trace.c`), on the calling thread, with
nothing of a renderer. What they meet is looked at as a traced ray looks at it: a surface with less than half of it
there, a hole for instance, is gone through, from just past it; particles are always gone through. A ray costs a walk
of two trees, and the trees are made again only for what changed, which the first pick of a frame finds out from
versions. Tracing's filter wants to see what moved since the frame before, which the next frame's own look at the trees
would think still: so a pick that made them again leaves a mark, and that look keeps what moved once more.

## The GPU: the CPU stays out of the way

All meshes share one vertex buffer and one index buffer, every texture is in one descriptor array,
and the instances are one buffer. So a frame is the same few commands however large the scene is:

1. A copy resets one draw command per mesh.
2. A compute shader tests each instance's sphere against the frustum and picks its level; those in view
   take a slot in that mesh's stretch of a list, by an atomic add on the command's instance count.
3. One `vkCmdDrawIndexedIndirect` draws everything (one per mesh, on a device without multi-draw).
   The vertex shader finds its instance through the
   list; the fragment shader indexes the texture array by material, with no branch: every material
   has a texture, white when none was given.
4. Layers come after, if there are any: see [Layers](#layers).
5. One triangle blends the overlay on.

The CPU's share is writing what changed, and recording those commands: 0.05 ms, with 10 objects or
90,000.

Other choices, each small:

- **Reversed depth with no far plane.** Depth is `near / distance` in a 32-bit float, which spreads
  precision evenly along the view: no far plane to tune, and far less z-fighting in the distance. (An orthographic
  camera has one: see above.)
- **Memory written by the CPU is the GPU's own** where it can be (always on an integrated GPU): no
  staging copy for what changes each frame.
- **Depth is never stored.** The render pass drops it at the end, which on a tiling GPU means it
  never leaves the chip.
- **Two frames in flight**, each with its own copy of what the CPU writes, so that neither waits.
- **Vulkan 1.2 with a classic render pass**, no newer: it reaches much older drivers and devices. The
  driver this was written on has neither dynamic rendering nor mesh shaders.

### Shaders with no compiler

Vulkan wants SPIR-V, which normally comes from a shader compiler at build time: that would be a
dependency. Artful's shaders are few and small, so `src/vulkan/shaders.c` writes them directly as
SSA when a renderer starts, on a builder of about 300 lines (`spirv.c`). No file, no tool, and the same
words on every build.

There is no validator in the loop, so what checks the shaders is the CPU backend: lighting is the same
arithmetic on both sides, and a test compares their pictures: they differ by more than 24 of 255 in
0.05% of the pixels of its scene. (The driver accepting a module proves less than that picture does.)

## The CPU: a tile-based GPU in software

`src/software/kernels.inc`. An immediate-mode rasterizer spends its time in memory: every triangle
reads and writes a depth buffer of 8 MB, and shades pixels that the next triangle covers. So this one
works as phones' GPUs do, tile-based and deferred:

**Cull.** Instances by sphere, in parallel; then meshlets, by sphere and cone: a meshlet that faces away
from the eye goes before a single vertex is read.

**Geometry.** A meshlet's vertices are transformed eight at a time into a scratch in L1. Each triangle
is rejected by outcodes, by area (back faces), and by holding no pixel center, which is what ends most
small triangles. Survivors go into the bins of the tiles they touch.

**Tile.** One thread takes a 64 x 64 tile, with its depth and ids in the cache:

1. *Rasterize.* All its triangles, for depth alone: exact edge functions on integers, vertices in 1/16
   pixel, the top-left rule, eight pixels at a time. Each pixel ends with the id of the triangle that
   shows there.
2. *Find what shows.* Most triangles of a dense scene turn out to show nowhere, and cost nothing more.
3. *Shade.* Only those are set up, and each pixel is shaded exactly once. There is no overdraw.
4. *Layers*, if any reach this tile, each over what is there: see [Layers](#layers).
5. *Overlay and out.* The overlay is blended on, and the tile goes to the frame: the only time its
   pixels touch main memory.

The depth buffer of the whole frame does not exist.

Details that matter:

- **Barycentrics from clip space** (Olano and Greer, 1997). They are worked out for the original
  triangle from its homogeneous vertices, which needs no vertex in front of the eye: clipping changes
  what a triangle covers and nothing else, and attributes are never clipped. The rare clipped triangle
  is a few lines.
- **Integers that are always big enough.** An edge's value at a tile's corner is taken in 64 bits. Across
  one tile it changes by less than 2^31: so an edge that crosses the tile fits 32 bits, and an edge
  that does not either rejects the triangle here, or need not be tested again.
- **Light in full vectors.** A dense mesh's triangles cover two or three pixels, and shading those
  alone would leave most of eight lanes empty. So a triangle only interpolates its pixels, and light
  is worked out eight pixels at a time across triangles of the same material. With a lighter triangle
  setup, that took the `heavy` scene from 44 to 34 ms. (Measured first: of its 36 ms in tiles,
  rasterizing was 8; setup and shading were the rest.)
- **The overlay saves work.** A tile under an opaque part of the overlay is not rasterized or shaded at
  all: a side panel of an interface makes the scene cheaper. Tile classes are worked out again only
  where the overlay's damage says so.
- **Pixels do not depend on threads.** Geometry is always cut into the same 64 pieces, whatever the
  number of threads, and tiles read them in order; so triangles reach a tile in a fixed order and even
  a tie in depth goes the same way.

### One source, two instruction sets

`kernels.inc` is written on eight-lane operations (`simd.h`) and built once for each instruction set:
AVX2, one instruction each; SSE4.1 and NEON, two registers of four lanes; and portably, as loops the
compiler makes what it can of. The choice is made at run time. All give the same bits: no fused
multiply-adds, no approximate reciprocals, and `min` and `max` as x86 defines them, which NEON has to be
told. A test holds them, at several thread counts, to one hash, and a picture drawn on a Snapdragon
matches one drawn on a Ryzen byte for byte. That costs little, and makes the fast paths testable against
the simple one.

On 64-bit Windows, GCC does not align the stack to 32 bytes (bug 54412), so the AVX2 file is assembled
with `-muse-unaligned-vector-move`, which costs nothing on processors that have AVX2.

### Colors

Lighting is linear; the frame holds the square root of light, and textures are squared on the way in.
That is gamma 2 rather than exact sRGB: one instruction each way on the CPU, the same arithmetic on
the GPU, and a BGRA frame like a window's, which cascade's page can be blended over in the space it
was painted in. A vertex's color is a byte of the same kind, squared as a texel is, and goes between
vertices as light.

## Metal

**Never run.** `metal.c` was written without a Mac. Everything below is what it is meant to do.

It is plain C. Both Metal and the Objective-C runtime are opened at run time, and a message is `objc_msgSend` cast to
what the method takes, so there is nothing to link and no Objective-C to compile. Its shaders are Vulkan's: SPIRV-Cross,
which comes with the Vulkan SDK, translates them, and `tools/mkmetal.py` writes the result into `shaders.inc`, so the
library takes only text. That script names each resource after its Vulkan binding first, and so gets a table from places
in an argument buffer to bindings, which is what `arguments` fills in.

That is also its first gap: shaders are made ahead of time, so a program's graphs, which Vulkan compiles when they
appear, cannot reach it. It would need its own way from a graph to Metal's language. The others are smaller. Occlusion
is left out, which changes no picture. A black hole would be a matter of porting. Textures go up decoded.

Being shared, memory needs no staging, and a frame waits for the one before it, so there is one copy of everything. Metal
has no single call for many indirect draws, so each mesh is a call: the CPU's part grows with meshes, though not with
instances.

## A black hole

`artfSceneSetBlackHole` puts one behind the scene. All of it is light: nothing is modelled, and the
frame's background is what comes back along each pixel's ray.

### A path is a few numbers

The usual way is to step a ray through four dimensions, with the metric's derivatives at each step.
Most of that can be done away with. Along a ray in Kerr's metric three things do not change: its energy,
its momentum along the axis (`xi`, per energy) and Carter's constant (`eta`). They separate the motion,
and with Mino's parameter `s` in place of time, `u = 1 / r`, and the ray's place on the unit sphere as
a vector `n` and not as two angles, every equation comes out as a polynomial:

```
u'' = (a^2 - K) u + 3 (eta + (xi - a)^2) u^2 - 2 a^2 eta u^3        K = eta + xi^2
n'' = -(K + 2 a^2 m^2) n, plus a^2 m on the axis                    m: n's height over the disc
psi' = -a u (2 - a xi u) / (1 - 2 u + a^2 u^2)
```

`psi` is how far space has dragged the ray around. Being a vector, `n` has no trouble at the poles, where
the azimuth's own equation blows up: a view straight down the axis works as any other. And with no spin
all this is the plane orbit `u'' = 3 u^2 - u`, which was the first version of this code.

So a ray is ten numbers or so, and a step a few dozen multiplications. Nystrom's method takes second
derivatives as they are: fourth order for three evaluations. The step is a 48th of an orbit at the
frequency `sqrt(K)`, and most rays leave in 24 to 50 of them. That is why this runs in real time with
no tables: nothing is precomputed for a camera, so the camera can go anywhere.

Stepping this way makes the rest simple as well. A ray crosses the disc wherever `m` changes sign,
however many times it goes around. It escapes where `u` reaches 0, and falls in past the horizon's.

The direction of a pixel becomes the constants through the frame of one at rest in the space that turns
there, which is a few lines, once a ray. Tracing goes back along the light, and Kerr's space does not
look the same with time reversed: the constants are the photon's, going the other way from the look,
and `s` runs backwards.

Tests hold this to what the theory says with no help from it. With no spin, the edge of the shadow, at
three distances, against `sqrt(27)` masses as seen from there. With spin, at .3, .6 and .95: the shadow's
two sides, off center and of different sizes, against the two circular orbits of light. Each comes
within a pixel.

### A moving eye

`velocity` is the eye's speed, relative to one at rest there, as a fraction of light's. Each pixel's
direction goes through the aberration formula first: what is seen crowds toward where the eye is going.
`artfBlackHoleOrbitSpeed` gives the speed of a circular orbit. With `doppler` on, the same factor goes
into the shift.

### The gas

The disc is a fluid (`fluid.c`): compressible, isothermal hydrodynamics in its plane, on 256 rings of 1024
cells, even in `1 / r`. Density, and the velocity that departs from a circular orbit:

```
dvr/dt = 2 Omega w + w^2 / r - c^2 d(ln S)/dr
dw/dt  = -(kappa^2 / 2 Omega) vr - vr w / r - (c^2 / r) d(ln S)/dphi
dS/dt  = -S div(v)
```

`Omega` and `kappa` are Kerr's, so `kappa` falls to 0 at the last stable orbit, where gas is no longer held.
That is all general relativity does here: the rest is Newton's fluid.

What stirs a real disc is magnetic, the magnetorotational instability, which needs a magnetic field and
three dimensions. Here eddies and clumps are stirred in at random, as two-dimensional studies of
such turbulence do, and the flow does what it will with them. Too much of that and the gas does not
settle: it gathers into a few hard rings within two thousand steps. So the stirring, the sound speed and
the drag are set for a state that lasts, which a long test holds.

The shear also wipes out all that varies around a ring, so that what is left of any gas, in time, is rings:
thick bands, nothing like a disc. Two things hold that off. Each ring's mean is drawn back to 1, which
leaves its threads alone, and small clumps go in all the time, for the shear to draw into new threads.
It only holds it off: after a few thousand steps the threads are coarser than they began.

The rings slide past each other far faster than anything else happens, and moved by plain interpolation
all detail would be smeared away in a few orbits. So, as in the FARGO scheme, a step moves what departs
from an orbit first, and then shifts each ring along itself by its own orbit, by a cubic that does not
overshoot. Between steps, the renderer turns each ring on by itself, so that motion is smooth whatever
the frame rate.

Clumps are drawn out by the shear into the long threads the eye knows, but that takes the outer disc
hundreds of masses of time. It starts already sheared: what the rings' turning does to a clump is known in
advance, so the first state is noise wound by each ring's orbit, and the fluid goes on from there.

Where the density falls below what the disc's thinning asks for, the disc tears. What shows of all this
is a tenth: the gas that shows is painted, at a finer grain than the fluid holds (see below), and the
fluid moves it.

The fluid runs on the CPU, for both backends, on as many threads as there are. They get the same field,
so they draw the same gas, and the GPU's copy is one `memcpy` a step into a buffer each frame in flight
has. It is chaotic, and uses only `+ - * /` in an order that does not depend on threads: a difference of a bit
would grow until it was another picture, and after 1,800 steps a Ryzen and a Snapdragon still agree to the
byte.

### The disc in the light

The disc has depth. At a crossing, the ray's speed in height says how far along it the disc goes either
way; three samples are taken along that stretch, front to back, each at its own radius, azimuth and
height. Seen at a slant the stretch is long and the ray goes through much gas: it is more opaque and
brighter, and that is what makes the thin bright line across the shadow, with a soft edge.

The disc is the film's, as [James et al., 2015](https://arxiv.org/abs/1502.03808) tell of it: not one
an astrophysicist would compute, but one an artist built, in Houdini, with procedural detail for the
close shots (their appendix A.6). So it is painted here too, into a map of 2048 by 1024, rows even in
1 / r:

- **strands** like hair along each orbit: ridged noise, stretched far along the orbit, wandering in and
  out as it goes around;
- **clouds**, whole near the hole and torn further out, so that the outer edge frays strand by strand;
- **thinner further out**, as (inner / r)^2, so that the image of the disc under the hole shows through
  the near side of it, as in their figure 15a; what each ring gives makes up for most of that, in a
  table by row, so that the disc is brightest near the hole and falls off slowly;
- **dust**: lanes, in a map of their own, from a little way out, that give 3% of the light and stop 26
  times as much: dark against the gas behind them.

The fluid moves it, at a tenth of its weight. What the fluid holds is coarser than what shows: at its
full weight, it drew rows of beads.

The temperature is one, 4500 K, as the film's, with its color from a table of Planck's law on film
balanced for 6500 K, and toward salmon for film's dyes: set against the colors of figure 15a, where dim
gas is orange and bright gas salmon going to pink. The default view is theirs too: spin 0.6, the disc
from the last stable orbit, the eye at 74 masses and 3.44 degrees.

What the film showed is figure 16: the same disc through the veil of an IMAX lens, which they made by
convolving with the point spread function of the real lenses. Here the bloom is that veil, and on by
default (4.5). It was set against figure 16 by measure, region by region, from the sky in the corners to
the inside of the shadow:

- only light past a threshold (2) feeds it: a lens's veil comes of what is far too bright, so it is
  strong around the hole and fades well before the corners;
- each level of its pyramid is worth 0.85 of the one below: wide, a third of the frame;
- 90% of its color goes to grey, so that it is nearly white, as theirs is.

`doppler` is cheap because what it needs of the ray is conserved: at each crossing the shift is the gas's
orbit against `xi`, in a closed form, and light goes as its fourth power. It is off by default, as in
the film.

### Stars

No catalogue. Space is cut into cubes, a few of which hold a star at a place their hash gives. A
direction looks up the cube it falls in: three multiplications and a hash. How near the sphere of
directions passes to a star gives it its brightness, so nothing has to be chosen for that. Behind them
is a faint map of dust, heaped along a band, as a galaxy is from inside.

At 26 masses the Einstein ring is about 22 degrees wide, so all of the frame is strongly lensed: stars
there really are drawn out along circles. The streaks are not motion blur.

### Light

The tracing writes floats, not bytes: the disc's light goes far past what a screen can show. With
several rays a pixel, each is at its own moment of `shutter`, which is motion blur for no more than
the rays cost.

A large frame with many rays is seconds of work for the GPU: 3840 x 2160 at 64 rays a pixel is 530
million rays, six seconds on a Vega 8. Windows takes a device that keeps the GPU two seconds as lost, and
a driver need not give the GPU back between the dispatches of one submission. So past 64 million rays
the rays go out in bands of rows, about 8 million rays each, each a submission of its own that is
waited for (`vkCmdDispatchBase`); below that a frame is one dispatch, as fast as it can go. What the GPU
spends on those bands is not in `gpuMs`.

```
rays  ──►  light  ──►  pyramid down  ──►  pyramid up  ──►  tone map, under the scene
```

Bloom is a pyramid of up to 8 levels, each adding what the one above holds, weighed down by 0.7 a level:
a blur some hundreds of pixels wide, for a few passes over small images. Very bright pixels are capped
going in, or the photons' orbit would paint squares. Levels past the first read sixteen
texels through four bilinear taps: means of four leave steps, too faint to see in the light, and
which a tone curve then brings out in the dark.

The curve is Narkowicz's fit to ACES, which rolls what is very bright off to white and leaves a color's
hue to the channels that have not yet got there: orange gas goes white as it heats, and not pink. The
last thing is a little noise, one step of a byte: glows fade over more pixels than a byte has steps.

On the CPU, rays go eight at a time, in lockstep, each lane leaving when it is done; a tile's pixels
share much the same path. On the GPU, a compute shader, in blocks of 8 x 8, for the same reason. A ray
is nearly all arithmetic and hardly touches memory, which suits both.

Both give the same picture: a test holds under 1% of the pixels to differing by more than 24 of 255,
and it finds fewer than 0.01%. Among the CPU's kernels and threads, and between a Ryzen and a Snapdragon, the bytes are equal. That
needed one thing: the maps are made with square roots and multiplications, which are exact, not `powf`
and `exp`, which differ from one C library to the next.

### What it is not

The gas is two-dimensional and has no magnetic field, and how much it is stirred is chosen, not derived:
it is a fluid, not a simulation of accretion, which takes a supercomputer. The gas that shows is painted on a
map of 2048 by 1024, which shows its texels when the eye comes close. The disc is lit only by itself: light that comes back to it around the hole is not counted.

What the scene holds is drawn straight over all this: a ship in front of the hole shows, but it is not
bent, and its light does not go through the bloom.

## Where it could go next

In the order that would pay the most, for the scenes that hurt today:

See [what stands between this and a stable version](stability.md).
