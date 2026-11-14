/* artful — what a frame shows when it is not the picture (ArtfView), the part both backends share: the frame they
 * draw it with. What each view is, is worked out where light is, in kernels.inc and in the scene's pixel shader. */
#include "scene/sceneInternal.h"

void artfFrameForView(ArtfFrameData* f, int view) {
    f->view = (float)view;
    if (!view) return;
    /* the color a view gives is what shows: no exposure, no fog, nothing after the frame */
    f->look[0] = 0;
    f->look[1] = 0;
    memset(f->post, 0, sizeof f->post);
    memset(f->grade, 0, sizeof f->grade);
    f->motionBlur = 0;
    f->fogFalloff = 0;
    /* and black behind it, with no picture of all around and no light from a black hole's backdrop */
    const float black[4] = {0, 0, 0, 1};
    memcpy(f->background, black, sizeof black);
    f->ground[3] = 0;
}
