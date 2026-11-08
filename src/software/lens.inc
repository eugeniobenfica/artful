/* artful — light around a black hole, eight pixels at a time. Part of kernels.inc.
 *
 * Around a spinning hole a ray has three things that do not change: its energy, its momentum along the
 * axis, and Carter's constant. Those separate the motion, and with a parameter chosen for that (Mino's
 * s) and the right variables the equations of Kerr's metric come out as plain polynomials:
 *
 *     u'' = (a^2 - K) u + 3 (eta + (xi - a)^2) u^2 - 2 a^2 eta u^3         u = 1 / r
 *     n'' = -(K + 2 a^2 m^2) n, with a^2 m added to the axis' part         n: where it is on the unit sphere
 *     psi' = -a u (2 - a xi u) / (1 - 2 u + a^2 u^2)                       m: n's height over the disc
 *
 * with K = eta + xi^2. n is a unit vector, not two angles, so nothing goes wrong at the poles; psi is the
 * angle by which space itself has dragged the ray along. Nystrom's steps take second derivatives as
 * they are: fourth order for three evaluations. With no spin this is the plane orbit of u'' = 3 u^2 - u.
 *
 * Tracing goes backwards along the light, and Kerr's space does not look the same with time reversed:
 * hence the signs. Where a ray crosses the disc's plane is where m changes sign, however often it goes
 * around; where it escapes is where u reaches 0.
 *
 * vulkan/shaders.c does the same arithmetic. */

#define LENS_STEP .13089969f     /* pi / 24, of an orbit */
#define LENS_STEPS 192           /* eight half turns: what goes on beyond that is on the photons' orbit */
#define LENS_SLICES 3            /* samples through the disc's depth */

typedef struct LensVec { V8F x, y, z; } LensVec;

V8_INLINE V8F lensDot(LensVec a, LensVec b) { return v8fAdd(v8fAdd(v8fMul(a.x, b.x), v8fMul(a.y, b.y)), v8fMul(a.z, b.z)); }

/* The angle of (x, y), in turns; a polynomial, so that it is the same on every processor. */
V8_INLINE V8F lensTurns(V8F y, V8F x) {
    const V8F ax = v8fAbs(x), ay = v8fAbs(y);
    const V8F most = v8fMax(v8fMax(ax, ay), v8fSet(1e-20f)), least = v8fMin(ax, ay);
    const V8F a = v8fDiv(least, most), s = v8fMul(a, a);
    V8F r = v8fAdd(v8fMul(v8fSet(-.0464964749f), s), v8fSet(.15931422f));
    r = v8fSub(v8fMul(r, s), v8fSet(.327622764f));
    r = v8fAdd(v8fMul(v8fMul(r, s), a), a);
    r = v8fSelect(v8fGt(ay, ax), r, v8fSub(v8fSet(1.57079633f), r));
    r = v8fSelect(v8fLt(x, v8fSet(0.f)), r, v8fSub(v8fSet(3.14159265f), r));
    r = v8fSelect(v8fLt(y, v8fSet(0.f)), r, v8fSub(v8fSet(0.f), r));
    return v8fMul(r, v8fSet(.15915494f));
}

/* The sine of an angle given in turns. */
V8_INLINE V8F lensSine(V8F turns) {
    V8F t = v8fSub(turns, v8fFloor(v8fAdd(turns, v8fSet(.5f)))); /* -.5 .. .5 */
    /* folded to -.25 .. .25, where the polynomial is good */
    t = v8fSelect(v8fGt(t, v8fSet(.25f)), t, v8fSub(v8fSet(.5f), t));
    t = v8fSelect(v8fLt(t, v8fSet(-.25f)), t, v8fSub(v8fSet(-.5f), t));
    const V8F x = v8fMul(t, v8fSet(6.2831853f)), x2 = v8fMul(x, x);
    V8F p = v8fAdd(v8fMul(x2, v8fSet(2.7557319e-6f)), v8fSet(-1.9841270e-4f));
    p = v8fAdd(v8fMul(p, x2), v8fSet(8.3333333e-3f));
    p = v8fAdd(v8fMul(p, x2), v8fSet(-.16666667f));
    return v8fAdd(v8fMul(v8fMul(p, x2), x), x);
}

V8_INLINE V8I lensHash(V8I x, V8I y, V8I z) {
    V8I h = v8iXor(v8iXor(v8iMul(x, v8iSet(73856093)), v8iMul(y, v8iSet(19349663))), v8iMul(z, v8iSet(83492791)));
    h = v8iMul(v8iXor(h, v8iShr(h, 15)), v8iSet(0x2C1B3C6D));
    h = v8iMul(v8iXor(h, v8iShr(h, 12)), v8iSet(0x297A2D39));
    return v8iXor(h, v8iShr(h, 15));
}

/* A byte of a hash, as 0..1. */
#define LENS_BYTE(h, shift) v8fMul(v8iToF(v8iAnd(v8iShr(h, shift), v8iSet(255))), v8fSet(1.f / 255.f))

/* One layer of stars. Space is cut in cubes, a few of which hold a star somewhere in them; a direction
 * sees the star of the cube it falls in, as a point of light with a soft edge. How close the sphere
 * of directions comes to the star gives each its brightness: nothing has to be chosen for that. */
V8_INLINE void lensStars(LensVec dir, float cells, int density, float size, float gain, V8F* r, V8F* g, V8F* b) {
    const V8F n = v8fSet(cells);
    const V8F px = v8fMul(dir.x, n), py = v8fMul(dir.y, n), pz = v8fMul(dir.z, n);
    const V8F cx = v8fFloor(px), cy = v8fFloor(py), cz = v8fFloor(pz);
    const V8I h = lensHash(v8fToI(cx), v8fToI(cy), v8fToI(cz));
    const V8I here = v8iGt(v8iSet(density), v8iAnd(h, v8iSet(255)));
    if (!v8iSigns(here)) return;
    const V8F dx = v8fSub(v8fSub(px, cx), LENS_BYTE(h, 8)), dy = v8fSub(v8fSub(py, cy), LENS_BYTE(h, 16)), dz = v8fSub(v8fSub(pz, cz), LENS_BYTE(h, 24));
    const V8F d2 = v8fAdd(v8fAdd(v8fMul(dx, dx), v8fMul(dy, dy)), v8fMul(dz, dz));
    V8F light = v8fMax(v8fSub(v8fSet(1.f), v8fMul(d2, v8fSet(1.f / (size * size)))), v8fSet(0.f));
    light = v8fMul(v8fMul(light, light), v8fSet(gain));
    light = v8fSelect(here, v8fSet(0.f), light);
    /* from orange to blue-white, as stars go */
    const V8F warm = LENS_BYTE(h, 3);
    *r = v8fAdd(*r, v8fMul(light, v8fAdd(v8fSet(.75f), v8fMul(warm, v8fSet(.25f)))));
    *g = v8fAdd(*g, v8fMul(light, v8fSet(.82f)));
    *b = v8fAdd(*b, v8fMul(light, v8fSub(v8fSet(1.f), v8fMul(warm, v8fSet(.45f)))));
}

/* What a row of a map holds at a turn, between two columns. width: a power of two. */
V8_INLINE V8F lensRow(const float* map, int width, V8I row, V8F turn) {
    const V8F at = v8fMul(v8fSub(turn, v8fFloor(turn)), v8fSet((float)width));
    const V8F column = v8fFloor(at), part = v8fSub(at, column);
    const V8I base = v8iMul(row, v8iSet(width));
    const V8I c0 = v8iAnd(v8fToI(column), v8iSet(width - 1)), c1 = v8iAnd(v8iAdd(c0, v8iSet(1)), v8iSet(width - 1));
    const V8F a = v8iAsF(v8iGather((const uint32_t*)map, v8iAdd(base, c0))), b = v8iAsF(v8iGather((const uint32_t*)map, v8iAdd(base, c1)));
    return v8fAdd(a, v8fMul(v8fSub(b, a), part));
}

/* What eight rays bring back. */
static void lensTrace(const ArtfHoleFrame* h, const float* map, const float* fluid, V8F nx, V8F ny, float time, V8F* outR, V8F* outG, V8F* outB) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    LensVec d = {v8fAdd(v8fAdd(v8fSet(h->forward[0]), v8fMul(nx, v8fSet(h->right[0]))), v8fMul(ny, v8fSet(h->up[0]))),
                 v8fAdd(v8fAdd(v8fSet(h->forward[1]), v8fMul(nx, v8fSet(h->right[1]))), v8fMul(ny, v8fSet(h->up[1]))),
                 v8fAdd(v8fAdd(v8fSet(h->forward[2]), v8fMul(nx, v8fSet(h->right[2]))), v8fMul(ny, v8fSet(h->up[2])))};
    const V8F invLength = v8fDiv(one, v8fSqrt(lensDot(d, d)));
    d.x = v8fMul(d.x, invLength);
    d.y = v8fMul(d.y, invLength);
    d.z = v8fMul(d.z, invLength);

    /* a moving eye: what it sees toward d comes, for one at rest there, from elsewhere, and bluer */
    V8F moved = one;
    const float gamma = h->boost[3];
    if (gamma > 1.f) {
        const float bx = h->boost[0], by = h->boost[1], bz = h->boost[2], share = gamma / (gamma + 1.f);
        /* light goes the opposite way to the look */
        const V8F c = v8fSub(zero, v8fAdd(v8fAdd(v8fMul(d.x, v8fSet(bx)), v8fMul(d.y, v8fSet(by))), v8fMul(d.z, v8fSet(bz))));
        const V8F push = v8fMul(v8fSet(gamma), v8fAdd(one, v8fMul(v8fSet(share), c)));
        const V8F over = v8fDiv(one, v8fMul(v8fSet(gamma), v8fAdd(one, c)));
        d.x = v8fMul(v8fSub(d.x, v8fMul(v8fSet(bx), push)), over);
        d.y = v8fMul(v8fSub(d.y, v8fMul(v8fSet(by), push)), over);
        d.z = v8fMul(v8fSub(d.z, v8fMul(v8fSet(bz), push)), over);
        moved = over;
    }

    /* what does not change along the ray; d is r, theta, phi there, for one at rest in the turning space */
    const float a = h->spin[0], a2 = a * a, rho = h->place[0], rootDelta = h->place[1], u0 = h->place[2], mu0 = h->place[3];
    const V8F energy = v8fSub(v8fSet(h->metric[0]), v8fMul(v8fSet(h->metric[1]), d.z));
    const V8F invEnergy = v8fDiv(one, energy);
    const V8F xi = v8fSub(zero, v8fMul(v8fMul(v8fSet(h->metric[2]), d.z), invEnergy));
    const V8F xiAxis = v8fMul(v8fMul(v8fSet(h->metric[3]), d.z), invEnergy); /* xi / sin(theta), but for its sign */
    const V8F across = v8fMul(v8fMul(v8fSet(rho), d.y), invEnergy);
    const V8F eta = v8fAdd(v8fMul(across, across), v8fMul(v8fSet(mu0 * mu0), v8fSub(v8fMul(xiAxis, xiAxis), v8fSet(a2))));
    const V8F big = v8fAdd(eta, v8fMul(xi, xi));
    const V8F step = v8fDiv(v8fSet(LENS_STEP), v8fSqrt(v8fMax(big, v8fSet(4.f))));
    const V8F step2 = v8fMul(step, step);

    /* the equations' coefficients */
    const V8F uA = v8fSub(v8fSet(a2), big);
    const V8F xiOff = v8fSub(xi, v8fSet(a));
    const V8F uB = v8fMul(v8fSet(3.f), v8fAdd(eta, v8fMul(xiOff, xiOff)));
    const V8F uC = v8fMul(v8fSet(-2.f * a2), eta);
    const V8F xiA = v8fMul(xi, v8fSet(a));
    const V8F twoA2 = v8fSet(2.f * a2), a2v = v8fSet(a2);
#define BEND(x) v8fMul(x, v8fAdd(uA, v8fMul(x, v8fAdd(uB, v8fMul(x, uC)))))
#define DRAG(x) v8fDiv(v8fMul(v8fMul(v8fSet(-a), x), v8fSub(v8fSet(2.f), v8fMul(xiA, x))), v8fMax(v8fAdd(v8fSub(one, v8fAdd(x, x)), v8fMul(v8fMul(a2v, x), x)), v8fSet(1e-3f)))

    V8F u = v8fSet(u0), w = v8fMul(v8fMul(v8fSet(-u0 * u0 * rootDelta * rho), d.x), invEnergy);
    LensVec n = {v8fSet(h->er[0]), v8fSet(h->er[1]), v8fSet(h->er[2])};
    const V8F alongTheta = across;
    LensVec v = {v8fAdd(v8fMul(alongTheta, v8fSet(h->etheta[0])), v8fMul(xiAxis, v8fSet(h->ephi[0]))),
                 v8fAdd(v8fMul(alongTheta, v8fSet(h->etheta[1])), v8fMul(xiAxis, v8fSet(h->ephi[1]))),
                 v8fAdd(v8fMul(alongTheta, v8fSet(h->etheta[2])), v8fMul(xiAxis, v8fSet(h->ephi[2])))};
    V8F psi = zero, drag = a != 0.f ? DRAG(u) : zero;

    const float inner = h->disc[0], thickness = h->disc[3], gain = h->look[0], doppler = h->look[1], discHeight = h->spin[2];
    const float rhoScale = 1.f / (1.f - inner / h->disc[1]);
    const float* rows = map + ARTF_HOLE_ROWS_AT;
    const float* colors = map + ARTF_HOLE_COLORS_AT;

    V8F sumR = zero, sumG = zero, sumB = zero, through = one;
    V8I active = v8iSet(-1), sky = v8iSet(0);
    LensVec away = {zero, zero, zero};
    V8F awayPsi = zero;
    const V8F uHorizon = v8fSet(h->spin[1]);

    for (int i = 0; i < LENS_STEPS; ++i) {
        /* Nystrom: three looks at each second derivative */
        const V8F half = v8fMul(step, v8fSet(.5f)), eighth = v8fMul(step2, v8fSet(.125f)), halfSquare = v8fMul(step2, v8fSet(.5f)), sixthSquare = v8fMul(step2, v8fSet(1.f / 6.f)), sixth = v8fMul(step, v8fSet(1.f / 6.f));
        const V8F k1 = BEND(u);
        const V8F c1 = v8fAdd(big, v8fMul(twoA2, v8fMul(n.z, n.z)));
        const LensVec a1 = {v8fSub(zero, v8fMul(c1, n.x)), v8fSub(zero, v8fMul(c1, n.y)), v8fSub(v8fMul(a2v, n.z), v8fMul(c1, n.z))};
        const V8F midU = v8fAdd(v8fAdd(u, v8fMul(half, w)), v8fMul(eighth, k1));
        const LensVec midN = {v8fAdd(v8fAdd(n.x, v8fMul(half, v.x)), v8fMul(eighth, a1.x)), v8fAdd(v8fAdd(n.y, v8fMul(half, v.y)), v8fMul(eighth, a1.y)), v8fAdd(v8fAdd(n.z, v8fMul(half, v.z)), v8fMul(eighth, a1.z))};
        const V8F k2 = BEND(midU);
        const V8F c2 = v8fAdd(big, v8fMul(twoA2, v8fMul(midN.z, midN.z)));
        const LensVec a2s = {v8fSub(zero, v8fMul(c2, midN.x)), v8fSub(zero, v8fMul(c2, midN.y)), v8fSub(v8fMul(a2v, midN.z), v8fMul(c2, midN.z))};
        const V8F endU = v8fAdd(v8fAdd(u, v8fMul(step, w)), v8fMul(halfSquare, k2));
        const LensVec endN = {v8fAdd(v8fAdd(n.x, v8fMul(step, v.x)), v8fMul(halfSquare, a2s.x)), v8fAdd(v8fAdd(n.y, v8fMul(step, v.y)), v8fMul(halfSquare, a2s.y)), v8fAdd(v8fAdd(n.z, v8fMul(step, v.z)), v8fMul(halfSquare, a2s.z))};
        const V8F k3 = BEND(endU);
        const V8F c3 = v8fAdd(big, v8fMul(twoA2, v8fMul(endN.z, endN.z)));
        const LensVec a3 = {v8fSub(zero, v8fMul(c3, endN.x)), v8fSub(zero, v8fMul(c3, endN.y)), v8fSub(v8fMul(a2v, endN.z), v8fMul(c3, endN.z))};
#define NEXT_P(p, q, a_, b_) v8fAdd(v8fAdd(p, v8fMul(step, q)), v8fMul(sixthSquare, v8fAdd(a_, v8fAdd(b_, b_))))
#define NEXT_V(q, a_, b_, c_) v8fAdd(q, v8fMul(sixth, v8fAdd(v8fAdd(a_, c_), v8fMul(v8fSet(4.f), b_))))
        const V8F nextU = NEXT_P(u, w, k1, k2), nextW = NEXT_V(w, k1, k2, k3);
        const LensVec nextN = {NEXT_P(n.x, v.x, a1.x, a2s.x), NEXT_P(n.y, v.y, a1.y, a2s.y), NEXT_P(n.z, v.z, a1.z, a2s.z)};
        const LensVec nextV = {NEXT_V(v.x, a1.x, a2s.x, a3.x), NEXT_V(v.y, a1.y, a2s.y, a3.y), NEXT_V(v.z, a1.z, a2s.z, a3.z)};
#undef NEXT_P
#undef NEXT_V
        V8F nextDrag = zero, nextPsi = zero;
        if (a != 0.f) {
            nextDrag = DRAG(nextU);
            nextPsi = v8fAdd(psi, v8fMul(sixth, v8fAdd(v8fAdd(drag, nextDrag), v8fMul(v8fSet(4.f), DRAG(midU)))));
        }

        /* through the disc's plane? */
        V8I crossed = v8iAnd(active, v8fLt(v8fMul(n.z, nextN.z), zero));
        if (v8iSigns(crossed)) {
            const V8F part = v8fDiv(n.z, v8fSub(n.z, nextN.z));
            const V8F hu = v8fMul(part, step), hh = v8fMul(v8fMul(hu, hu), v8fSet(.5f));
            const V8F uc = v8fAdd(v8fAdd(u, v8fMul(hu, w)), v8fMul(hh, k1));
            const V8F rhoCenter = v8fMul(v8fSub(one, v8fMul(v8fSet(inner), uc)), v8fSet(rhoScale));
            /* (a chord of the disc can reach in or out of its range from a crossing just outside) */
            crossed = v8iAnd(crossed, v8iAnd(v8fGt(rhoCenter, v8fSet(-.03f)), v8fLt(rhoCenter, v8fSet(1.02f))));
            if (v8iSigns(crossed)) {
                const V8F wc = v8fAdd(w, v8fMul(hu, k1));
                const V8F cx = v8fAdd(v8fAdd(n.x, v8fMul(hu, v.x)), v8fMul(hh, a1.x)), cy = v8fAdd(v8fAdd(n.y, v8fMul(hu, v.y)), v8fMul(hh, a1.y));
                const V8F vx = v8fAdd(v.x, v8fMul(hu, a1.x)), vy = v8fAdd(v.y, v8fMul(hu, a1.y)), vz = v8fAdd(v.z, v8fMul(hu, a1.z));
                const V8F psiC = v8fAdd(psi, v8fMul(part, v8fSub(nextPsi, psi)));
                const V8F dragC = v8fAdd(drag, v8fMul(part, v8fSub(nextDrag, drag)));
                const V8F turn = v8fSelect(crossed, zero, v8fAdd(lensTurns(cy, cx), v8fMul(psiC, v8fSet(.15915494f))));
                /* how fast the azimuth goes, in turns */
                const V8F rate = v8fMul(v8fAdd(v8fDiv(v8fSub(v8fMul(cx, vy), v8fMul(cy, vx)), v8fMax(v8fAdd(v8fMul(cx, cx), v8fMul(cy, cy)), v8fSet(1e-6f))), dragC), v8fSet(.15915494f));

                /* gas seen at a slant is deeper: more of it stops light, and more of it shines */
                const V8F slope = v8fDiv(wc, uc);
                const V8F speed2 = v8fAdd(v8fAdd(v8fMul(vx, vx), v8fMul(vy, vy)), v8fAdd(v8fMul(vz, vz), v8fMul(slope, slope)));
                const V8F mu = v8fMax(v8fDiv(v8fAbs(vz), v8fSqrt(speed2)), v8fSet(.02f));
                const V8F slant = v8fDiv(v8fSet(thickness / (float)LENS_SLICES), mu);
                /* how far along the ray the disc's depth goes either way, within reason */
                const V8F reach = v8fMin(v8fDiv(v8fSet(discHeight), v8fMax(v8fAbs(vz), v8fSet(1e-5f))), v8fMul(step, v8fSet(1.5f)));

                V8F glow = zero, clear = through;
                for (int k = 0; k < LENS_SLICES; ++k) {
                    const float t = ((float)k + .5f) * (2.f / (float)LENS_SLICES) - 1.f;
                    const V8F ds = v8fMul(reach, v8fSet(t));
                    const V8F uk = v8fAdd(uc, v8fMul(wc, ds));
                    const V8F rhoK = v8fMul(v8fSub(one, v8fMul(v8fSet(inner), uk)), v8fSet(rhoScale));
                    const V8I inside = v8iAnd(crossed, v8iAnd(v8fGt(rhoK, zero), v8fLt(rhoK, one)));
                    const V8F rowAt = v8fMul(v8fSelect(inside, zero, rhoK), v8fSet((float)(ARTF_HOLE_MAP_HEIGHT - 1)));
                    const V8F rowFloor = v8fMin(v8fFloor(rowAt), v8fSet((float)(ARTF_HOLE_MAP_HEIGHT - 2)));
                    const V8F rowPart = v8fSub(rowAt, rowFloor);
                    const V8I row0 = v8fToI(rowFloor), row1 = v8iAdd(row0, v8iSet(1));
                    const V8F speed0 = v8iAsF(v8iGather((const uint32_t*)rows, v8iAdd(row0, row0))), speed1 = v8iAsF(v8iGather((const uint32_t*)rows, v8iAdd(row1, row1)));
                    const V8F turnK = v8fAdd(turn, v8fSelect(inside, zero, v8fMul(rate, ds)));
                    const V8F d0 = lensRow(map, ARTF_HOLE_MAP_WIDTH, row0, v8fSub(turnK, v8fMul(speed0, v8fSet(time))));
                    const V8F d1 = lensRow(map, ARTF_HOLE_MAP_WIDTH, row1, v8fSub(turnK, v8fMul(speed1, v8fSet(time))));
                    V8F density = v8fAdd(d0, v8fMul(v8fSub(d1, d0), rowPart));
                    /* the fluid, as it was when last stepped, with each ring turned on since */
                    const V8F fluidAt = v8fMul(v8fSelect(inside, zero, rhoK), v8fSet((float)(ARTF_FLUID_HEIGHT - 1)));
                    const V8F fluidFloor = v8fMin(v8fFloor(fluidAt), v8fSet((float)(ARTF_FLUID_HEIGHT - 2)));
                    const V8I fluidRow = v8fToI(fluidFloor);
                    const V8F fluidTurn = v8fSub(turnK, v8fMul(speed0, v8fSet(time - h->flow[0])));
                    const V8F g0 = lensRow(fluid, ARTF_FLUID_WIDTH, fluidRow, fluidTurn), g1 = lensRow(fluid, ARTF_FLUID_WIDTH, v8iAdd(fluidRow, v8iSet(1)), fluidTurn);
                    /* (it moves the painted gas, a little: what it holds is coarser than what shows) */
                    const V8F gas = v8fAdd(g0, v8fMul(v8fSub(g1, g0), v8fSub(fluidAt, fluidFloor)));
                    density = v8fMul(density, v8fAdd(v8fSet(1.f - ARTF_HOLE_FLUID), v8fMul(v8fSet(ARTF_HOLE_FLUID), gas)));
                    /* what of it is dust */
                    const V8F dustAt = v8fMul(v8fSelect(inside, zero, rhoK), v8fSet((float)(ARTF_HOLE_DUST_HEIGHT - 1)));
                    const V8I dustRow = v8fToI(v8fMin(v8fFloor(dustAt), v8fSet((float)(ARTF_HOLE_DUST_HEIGHT - 1))));
                    const V8F dusty = lensRow(map + ARTF_HOLE_DUST_AT, ARTF_HOLE_DUST_WIDTH, dustRow, v8fSub(turnK, v8fMul(speed0, v8fSet(time))));
                    const V8F lit = v8fSub(one, v8fMul(v8fSet(ARTF_HOLE_DUST_DIM), dusty));
                    /* and how far from the middle of the disc it is */
                    const V8F height = v8fMul(v8fSet(t), v8fDiv(reach, v8fSet(discHeight)));
                    const V8F level = v8fMax(v8fSub(one, v8fMul(v8fMul(vz, vz), v8fMul(height, height))), zero);
                    density = v8fSelect(inside, zero, v8fMul(density, level));
                    const V8F depth = v8fMul(v8fMul(slant, density), v8fAdd(one, v8fMul(v8fSet(ARTF_HOLE_DUST_DENSE), dusty)));
                    const V8F alpha = v8fDiv(depth, v8fAdd(one, depth));
                    glow = v8fAdd(glow, v8fMul(v8fMul(clear, alpha), v8fMul(density, lit)));
                    clear = v8fMul(clear, v8fSub(one, alpha));
                }

                /* its temperature, and what the gas's orbit and the hole's pull do to it */
                const V8F rhoSafe = v8fMin(v8fMax(rhoCenter, zero), one);
                const V8F rowAt = v8fMul(v8fSelect(crossed, zero, rhoSafe), v8fSet((float)(ARTF_HOLE_MAP_HEIGHT - 1)));
                const V8I rowIndex = v8fToI(rowAt);
                V8F heat = v8iAsF(v8iGather((const uint32_t*)rows + 1, v8iAdd(rowIndex, rowIndex)));
                const V8F gives = v8iAsF(v8iGather((const uint32_t*)(map + ARTF_HOLE_GLOW_AT), rowIndex));
                V8F light = v8fMul(v8fMul(v8fSet(gain), glow), gives);
                if (doppler != 0.f) {
                    const V8F safeU = v8fMax(uc, v8fSet(1e-4f));
                    const V8F root = v8fMul(safeU, v8fSqrt(safeU)); /* u^3/2 */
                    const V8F lift = v8fAdd(one, v8fMul(v8fSet(a), root));
                    const V8F spin = v8fDiv(root, lift);
                    const V8F beneath = v8fMax(v8fAdd(v8fSub(one, v8fMul(v8fSet(3.f), safeU)), v8fMul(v8fSet(2.f * a), root)), v8fSet(1e-4f));
                    const V8F shift = v8fDiv(v8fMul(v8fSqrt(beneath), v8fMul(invEnergy, moved)), v8fMul(lift, v8fSub(one, v8fMul(spin, xi))));
                    const V8F g = v8fAdd(one, v8fMul(v8fSet(doppler), v8fSub(shift, one)));
                    const V8F g2 = v8fMul(g, g);
                    light = v8fMul(light, v8fMul(g2, g2));
                    heat = v8fMul(heat, g);
                }
                /* a black body's color at that temperature */
                const V8F colorAt = v8fMin(v8fMul(heat, v8fSet((float)(ARTF_HOLE_COLORS / 2))), v8fSet((float)(ARTF_HOLE_COLORS - 1)));
                const V8I color = v8fToI(v8fSelect(crossed, zero, colorAt));
                const V8I at = v8iAdd(v8iAdd(color, color), color);
                const uint32_t* table = (const uint32_t*)colors;
                const V8F red = v8iAsF(v8iGather(table, at)), green = v8iAsF(v8iGather(table + 1, at)), blue = v8iAsF(v8iGather(table + 2, at));
                sumR = v8fSelect(crossed, sumR, v8fAdd(sumR, v8fMul(light, red)));
                sumG = v8fSelect(crossed, sumG, v8fAdd(sumG, v8fMul(light, green)));
                sumB = v8fSelect(crossed, sumB, v8fAdd(sumB, v8fMul(light, blue)));
                through = v8fSelect(crossed, through, clear);
            }
        }

        /* out, or in */
        const V8I escaped = v8iAnd(active, v8fGt(zero, nextU));
        if (v8iSigns(escaped)) {
            const V8F part = v8fDiv(u, v8fSub(u, nextU));
            away.x = v8fSelect(escaped, away.x, v8fAdd(n.x, v8fMul(part, v8fSub(nextN.x, n.x))));
            away.y = v8fSelect(escaped, away.y, v8fAdd(n.y, v8fMul(part, v8fSub(nextN.y, n.y))));
            away.z = v8fSelect(escaped, away.z, v8fAdd(n.z, v8fMul(part, v8fSub(nextN.z, n.z))));
            awayPsi = v8fSelect(escaped, awayPsi, v8fAdd(psi, v8fMul(part, v8fSub(nextPsi, psi))));
            sky = v8iOr(sky, escaped);
        }
        active = v8iAndNot(v8iOr(escaped, v8fGt(nextU, uHorizon)), active);
        active = v8iAnd(active, v8fGt(through, v8fSet(.004f)));
        if (!v8iSigns(active)) break;
        u = nextU;
        w = nextW;
        n = nextN;
        v = nextV;
        psi = nextPsi;
        drag = nextDrag;
    }
#undef BEND
#undef DRAG

    if (h->look[2] > 0.f && v8iSigns(sky)) {
        const V8F invAway = v8fDiv(one, v8fSqrt(v8fMax(lensDot(away, away), v8fSet(1e-12f))));
        const V8F psiTurns = v8fMul(awayPsi, v8fSet(.15915494f));
        const V8F sine = lensSine(psiTurns), cosine = lensSine(v8fAdd(psiTurns, v8fSet(.25f)));
        const V8F x = v8fMul(away.x, invAway), y = v8fMul(away.y, invAway), m = v8fMul(away.z, invAway);
        const LensVec dir = {v8fSub(v8fMul(x, cosine), v8fMul(y, sine)), v8fAdd(v8fMul(x, sine), v8fMul(y, cosine)), m};
        V8F r = zero, g = zero, b = zero;
        lensStars(dir, 90.f, 26, .1f, 1.6f, &r, &g, &b);
        lensStars(dir, 240.f, 70, .17f, .22f, &r, &g, &b);

        /* the dust: one who moves, or sits deep in the well, sees all of it brighter */
        const V8F skyTurn = v8fSelect(sky, zero, lensTurns(dir.y, dir.x));
        const V8F heightAt = v8fMul(v8fAdd(v8fMul(v8fSelect(sky, zero, m), v8fSet(.5f)), v8fSet(.5f)), v8fSet((float)(ARTF_HOLE_SKY_HEIGHT - 1)));
        const V8F skyFloor = v8fMin(v8fFloor(heightAt), v8fSet((float)(ARTF_HOLE_SKY_HEIGHT - 2)));
        const V8I skyRow = v8fToI(skyFloor);
        const V8F s0 = lensRow(map + ARTF_HOLE_SKY_AT, ARTF_HOLE_SKY_WIDTH, skyRow, skyTurn), s1 = lensRow(map + ARTF_HOLE_SKY_AT, ARTF_HOLE_SKY_WIDTH, v8iAdd(skyRow, v8iSet(1)), skyTurn);
        const V8F dust = v8fAdd(s0, v8fMul(v8fSub(s1, s0), v8fSub(heightAt, skyFloor)));
        r = v8fAdd(r, v8fMul(dust, v8fSet(.92f)));
        g = v8fAdd(g, v8fMul(dust, v8fSet(.86f)));
        b = v8fAdd(b, dust);

        V8F shown = v8fMul(through, v8fSet(h->look[2]));
        if (doppler != 0.f) {
            const V8F g1 = v8fAdd(one, v8fMul(v8fSet(doppler), v8fSub(v8fMul(invEnergy, moved), one)));
            const V8F g2 = v8fMul(g1, g1);
            shown = v8fMul(shown, v8fMul(g2, g2));
        }
        shown = v8fSelect(sky, zero, shown);
        sumR = v8fAdd(sumR, v8fMul(r, shown));
        sumG = v8fAdd(sumG, v8fMul(g, shown));
        sumB = v8fAdd(sumB, v8fMul(b, shown));
    }
    *outR = sumR;
    *outG = sumG;
    *outB = sumB;
}

/* Every ray of a tile, into the frame's high dynamic range planes. With several rays a pixel, each is
 * at its own moment of the shutter's opening: what moves comes out blurred, as on film. */
static void lensTile(ArtfSoftFrame* f, uint32_t index) {
    const ArtfHoleFrame* h = &f->holeData;
    const int ox = (int)(index % (uint32_t)f->tilesX) * ARTF_TILE, oy = (int)(index / (uint32_t)f->tilesX) * ARTF_TILE;
    const int tw = artfMini(ARTF_TILE, f->width - ox), th = artfMini(ARTF_TILE, f->height - oy);
    const int side = (int)h->size[2];
    const V8F lanes = v8iToF(v8iIota()), scaleX = v8fSet(h->image[1]), one = v8fSet(1.f), share = v8fSet(h->image[3]);
    for (int y = 0; y < th; ++y) {
        const size_t row = (size_t)(oy + y) * f->hdrStride + ox;
        for (int x = 0; x < tw; x += 8) {
            V8F r = v8fSet(0.f), g = r, b = r;
            for (int sy = 0; sy < side; ++sy)
                for (int sx = 0; sx < side; ++sx) {
                    const float dx = ((float)sx + .5f) / (float)side, dy = ((float)sy + .5f) / (float)side;
                    const V8F nx = v8fSub(v8fMul(v8fAdd(lanes, v8fSet((float)(ox + x) + dx)), scaleX), one);
                    const V8F ny = v8fSet(1.f - ((float)(oy + y) + dy) * h->image[2]);
                    /* (the moments go by a stride, or neighbours along a row would all share one) */
                    const float moment = ((float)((sy * side + sx) * 5 % (side * side)) + .5f) * h->image[3] - .5f;
                    V8F tr, tg, tb;
                    lensTrace(h, f->holeMap, f->holeFluid, nx, ny, h->disc[2] + h->spin[3] * moment, &tr, &tg, &tb);
                    r = v8fAdd(r, tr);
                    g = v8fAdd(g, tg);
                    b = v8fAdd(b, tb);
                }
            v8fStore(f->hdr[0] + row + x, v8fMul(r, share));
            v8fStore(f->hdr[1] + row + x, v8fMul(g, share));
            v8fStore(f->hdr[2] + row + x, v8fMul(b, share));
        }
    }
}

/* Light to what a screen can show, as the curve film stock has: a toe, and a shoulder that bends all
 * that is very bright to white, without clipping. (Narkowicz's fit to ACES.) */
V8_INLINE V8F lensToneMap(V8F x) {
    const V8F top = v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.51f)), v8fSet(.03f)));
    const V8F bottom = v8fAdd(v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.43f)), v8fSet(.59f))), v8fSet(.14f));
    return v8fSqrt(v8fMin(v8fDiv(top, bottom), v8fSet(1.f)));
}

/* A tile's background: the light with its bloom, tone mapped. Glows fade over more pixels than a byte
 * has steps, so a little noise goes in to keep them from showing bands. */
static void lensFill(ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    const ArtfHoleFrame* h = &f->holeData;
    const float exposure = h->image[0];
    const uint32_t* level = h->bloom[0];
    const int bw = (int)level[1], bh = (int)level[2];
    const V8F lanes = v8iToF(v8iIota()), zero = v8fSet(0.f);
    const V8F spread = v8fSet(h->look[3] * (1.f - ARTF_BLOOM_FADE));
    const V8F lastX = v8fSet((float)(bw - 1));
    const V8F bytes = v8fSet(255.f), round = v8fSet(.5f);
    for (int y = 0; y < th; ++y) {
        const float sy = ((float)(oy + y) + .5f) * .5f - .5f, baseY = floorf(sy), partY = sy - baseY;
        const int y0 = artfClampi((int)baseY, 0, bh - 1), y1 = artfClampi((int)baseY + 1, 0, bh - 1);
        const V8F rowPart = v8fSet(partY);
        const size_t row = (size_t)(oy + y) * f->hdrStride + ox;
        for (int x = 0; x < tw; x += 8) {
            const V8F pixel = v8fAdd(lanes, v8fSet((float)(ox + x)));
            const V8F sx = v8fSub(v8fMul(v8fAdd(pixel, round), round), round);
            const V8F baseX = v8fFloor(sx), partX = v8fSub(sx, baseX);
            const V8I x0 = v8fToI(v8fMin(v8fMax(baseX, zero), lastX)), x1 = v8fToI(v8fMin(v8fMax(v8fAdd(baseX, v8fSet(1.f)), zero), lastX));
            const V8I top = v8iSet(y0 * bw), bottom = v8iSet(y1 * bw);
            V8F glows[3];
            for (int c = 0; c < 3; ++c) {
                const uint32_t* plane = (const uint32_t*)(f->bloom[c] + level[0]);
                const V8F a = v8iAsF(v8iGather(plane, v8iAdd(top, x0))), b = v8iAsF(v8iGather(plane, v8iAdd(top, x1)));
                const V8F d = v8iAsF(v8iGather(plane, v8iAdd(bottom, x0))), e = v8iAsF(v8iGather(plane, v8iAdd(bottom, x1)));
                const V8F upper = v8fAdd(a, v8fMul(v8fSub(b, a), partX)), lower = v8fAdd(d, v8fMul(v8fSub(e, d), partX));
                glows[c] = v8fAdd(upper, v8fMul(v8fSub(lower, upper), rowPart));
            }
            /* a lens's glow is nearly white: the colors of what makes it run together in it */
            const V8F grey = v8fAdd(v8fAdd(v8fMul(glows[0], v8fSet(.3f)), v8fMul(glows[1], v8fSet(.59f))), v8fMul(glows[2], v8fSet(.11f)));
            V8F out[3];
            for (int c = 0; c < 3; ++c) {
                const V8F glow = v8fAdd(glows[c], v8fMul(v8fSub(grey, glows[c]), v8fSet(ARTF_BLOOM_WHITE)));
                const V8F light = v8fAdd(v8fLoad(f->hdr[c] + row + x), v8fMul(glow, spread));
                out[c] = lensToneMap(v8fMul(light, v8fSet(exposure)));
            }
            const V8I id = v8iAdd(v8iAdd(v8iSet(ox + x), v8iIota()), v8iSet((oy + y) * 8192));
            const V8I noiseBits = lensHash(id, v8iSet(7), v8iSet(13));
            const V8F dither = v8fMul(v8iToF(v8iAnd(noiseBits, v8iSet(255))), v8fSet(1.f / 255.f));
#define BYTE(v) v8fToI(v8fMin(v8fAdd(v8fMul(v, bytes), dither), bytes))
            const V8I pixelOut = v8iOr(v8iOr(v8iShl(BYTE(out[0]), 16), v8iShl(BYTE(out[1]), 8)), v8iOr(BYTE(out[2]), v8iSet((int32_t)0xFF000000u)));
#undef BYTE
            v8iStore(w->color + y * ARTF_TILE + x, pixelOut);
        }
    }
}
