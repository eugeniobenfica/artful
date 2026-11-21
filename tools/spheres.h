/* artful's tools: the spheres, the scene artful-render draws first and the web page draws (tools/web), and the
 * checkered texture both take. */
#ifndef ARTFUL_TOOLS_SPHERES_H
#define ARTFUL_TOOLS_SPHERES_H

#include "artful/artful.h"

static ArtfTexture checker(ArtfScene* s) {
    static uint32_t px[256 * 256];
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) px[y * 256 + x] = ((x / 32 + y / 32) & 1) ? 0xFFE8E4DCu : 0xFF3A4252u;
    return artfSceneAddTexture(s, px, 256, 256, 0);
}

/* A floor, a sphere, a torus and a cube under one light; and where the camera is. */
static void spheresScene(ArtfScene* s, ArtfCamera* camera) {
    ArtfMaterialDesc floorDesc = {artfVec3(.9f, .9f, .9f), checker(s), .1f, .33f, artfVec3(0, 0, 0)};
    ArtfMesh plane = artfSceneAddPlane(s, 40.f, 10.f);
    ArtfTransform t = artfTransformAt(0, -1.2f, 0, 1);
    artfSceneAddInstance(s, plane, artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMesh sphere = artfSceneAddSphere(s, 1.f, 48, 64), torus = artfSceneAddTorus(s, 1.f, .35f, 64, 32), cube = artfSceneAddCube(s, 1.6f);
    ArtfMaterialDesc red = {artfVec3(.8f, .12f, .1f), 0, .8f, .14f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc gold = {artfVec3(.85f, .6f, .15f), 0, .6f, .24f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc box = {artfVec3(1, 1, 1), checker(s), .3f, .24f, artfVec3(0, 0, 0)};
    t = artfTransformAt(-2.6f, 0, 0, 1);
    artfSceneAddInstance(s, sphere, artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(1, .2f, 0), 1.1f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, torus, artfSceneAddMaterial(s, &gold), &t);
    t = artfTransformTrs(artfVec3(2.6f, -.2f, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), .6f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, cube, artfSceneAddMaterial(s, &box), &t);
    artfSceneAddLight(s, artfVec3(-1.5f, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.2f), 7.f);
    const ArtfCamera c = {artfVec3(0, 2.4f, 6.5f), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.0f, .1f};
    *camera = c;
}

#endif
