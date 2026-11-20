/* artful — a frame's passes as one graph for both backends: what each reads and writes, and the check that the order a
 * backend ran its last frame's passes in keeps to it (framePasses.c). */
#ifndef ARTFUL_FRAME_PASSES_H
#define ARTFUL_FRAME_PASSES_H

#include "artful/artful.h"

#include <stddef.h>
#include <stdint.h>

/* Of r's last frame (artfRendererStats' passes): how many of its passes are not in the graph, or read what a pass after
 * them writes; the first, said in why. 0: the frame keeps to the graph. */
int artfFramePassesCheck(const ArtfRenderer* r, char* why, size_t size);
/* The same of passes named in an order (for tests: an order that breaks the graph is found). */
int artfFramePassesOrder(const char* const* names, uint32_t count, ArtfBackend backend, char* why, size_t size);

#endif
