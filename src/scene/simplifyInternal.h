/* artful — lighter versions of a mesh (simplify.c). */
#ifndef ARTFUL_SIMPLIFY_INTERNAL_H
#define ARTFUL_SIMPLIFY_INTERNAL_H

#include "artful/scene.h"

typedef struct ArtfSimplified {
    uint32_t* indices;   /* into the vertices given; the caller's (artfFree) */
    uint32_t indexCount;
    float error;         /* about how far the surface moved, in the mesh's units */
} ArtfSimplified;

/* Reduces a mesh to each number of triangles in targets, from the largest down. Returns how many
 * levels it made: fewer when the mesh cannot go that far. */
uint32_t artfSimplify(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const uint32_t* targets, uint32_t targetCount, ArtfSimplified* out);

#endif
