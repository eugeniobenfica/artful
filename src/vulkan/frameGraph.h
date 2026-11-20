/* artful — the GPU's frame as a graph (the plan's phase 4.2, its first step): each pass vulkan.c records, what it reads and
 * writes of the targets a frame shares, and what that says of memory. Described, not yet driving anything: what each
 * pass's shaders read and write comes from their IR (irUses), so the description cannot drift from them; what a render
 * pass draws into is said here. */
#ifndef ARTFUL_FRAME_GRAPH_H
#define ARTFUL_FRAME_GRAPH_H

#include "artful/artful.h"

#include <stdio.h>

/* The targets a frame's passes share, as big as the frame (the small per-frame buffers are left out). */
typedef enum ArtfFgTarget {
    FG_FLOAT_COLOR, FG_FLOAT_SURFACE, FG_DEPTH_IMAGE, FG_HIZ, FG_FOG, FG_LIGHT, FG_SECOND, FG_HISTORY, FG_DEPTH, FG_SURFACE,
    FG_OCCLUSION, FG_BLOOM, FG_BLUR, FG_FRAME, FG_TARGETS
} ArtfFgTarget;

/* Of a Vulkan renderer: each target's bytes, as allocated now (vulkan.c). */
void artfVulkanTargetBytes(const ArtfRenderer* r, uint64_t* bytes);

/* The graph of the last frame r drew (its passes, as artfRendererStats lists them): as a table and what aliasing would
 * save to report, and as Graphviz to dot (either NULL: not written). Returns the bytes aliasing would save, or 0. */
uint64_t artfFrameGraphReport(ArtfRenderer* r, FILE* report, FILE* dot);
/* The passes the last report met that the description has not, or not in its order (for tests: 0). */
extern int artfFrameGraphUndescribed;

#endif
