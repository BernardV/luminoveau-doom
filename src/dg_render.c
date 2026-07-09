// dg_render.c — geometry/view bridge for the GPU renderer (plan.md, Fase 1).
//
// Extracts Doom's level geometry (walls, from segs) and the per-frame view
// params into plain float arrays the C++ DoomRenderPass consumes. Doom is C and
// its headers define `boolean`/`fixed_t`/etc. that clash with C++, so all Doom
// access is confined here and exposed through the plain-C dg_bridge.h.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomdef.h"
#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "m_fixed.h"

#include "dg_bridge.h"

#define FX(a) ((float)(a) / (float)FRACUNIT)   // 16.16 fixed -> float world units

// One vertex: engine-space position (Doom XY -> engine XZ, Doom height -> Y-up)
// plus a grayscale shade from the sector light level.
typedef struct { float x, y, z, shade; } dg_vert_t;

static dg_vert_t* g_verts    = NULL;
static int        g_vertCount = 0;   // number of vertices (multiple of 3)
static int        g_vertCap   = 0;
static unsigned   g_version   = 0;   // bumped each rebuild
static seg_t*     g_builtSegs = NULL; // detects level change (segs realloc'd per level)
static int        g_builtNumSegs = -1;

static void push_vert(float x, float y, float z, float shade)
{
    if (g_vertCount >= g_vertCap) {
        g_vertCap = g_vertCap ? g_vertCap * 2 : 4096;
        g_verts = (dg_vert_t*)realloc(g_verts, g_vertCap * sizeof(dg_vert_t));
    }
    dg_vert_t* v = &g_verts[g_vertCount++];
    v->x = x; v->y = y; v->z = z; v->shade = shade;
}

// Emit a wall quad between world-space endpoints (x1,z1)-(x2,z2), spanning
// heights [ybot, ytop]. Two triangles; winding is CCW when viewed from the
// front side (culling is off in Fase 1 anyway).
static void push_quad(float x1, float z1, float x2, float z2,
                      float ybot, float ytop, float shade)
{
    if (ytop <= ybot) return;
    // corners: A=(x1,ybot,z1) B=(x2,ybot,z2) C=(x2,ytop,z2) D=(x1,ytop,z1)
    push_vert(x1, ybot, z1, shade); push_vert(x2, ybot, z2, shade); push_vert(x2, ytop, z2, shade);
    push_vert(x1, ybot, z1, shade); push_vert(x2, ytop, z2, shade); push_vert(x1, ytop, z1, shade);
}

static void build_walls(void)
{
    int i;
    g_vertCount = 0;

    for (i = 0; i < numsegs; i++) {
        seg_t*    seg = &segs[i];
        sector_t* fs  = seg->frontsector;
        if (!fs) continue;

        float x1 = FX(seg->v1->x), z1 = FX(seg->v1->y);
        float x2 = FX(seg->v2->x), z2 = FX(seg->v2->y);
        float shade = fs->lightlevel / 255.0f;
        if (shade < 0.0f) shade = 0.0f; if (shade > 1.0f) shade = 1.0f;

        float ffloor = FX(fs->floorheight), fceil = FX(fs->ceilingheight);

        if (!seg->backsector) {
            // One-sided: solid wall floor..ceiling.
            push_quad(x1, z1, x2, z2, ffloor, fceil, shade);
        } else {
            sector_t* bs = seg->backsector;
            float bfloor = FX(bs->floorheight), bceil = FX(bs->ceilingheight);
            // Lower wall (step up into the back sector).
            if (bfloor > ffloor) push_quad(x1, z1, x2, z2, ffloor, bfloor, shade);
            // Upper wall (back ceiling lower than front ceiling).
            if (bceil < fceil)   push_quad(x1, z1, x2, z2, bceil, fceil, shade);
        }
    }

    g_builtSegs    = segs;
    g_builtNumSegs = numsegs;
    g_version++;
}

int DG_WorldReady(void)
{
    return (gamestate == GS_LEVEL) && (numsegs > 0) && (segs != NULL);
}

const float* DG_WorldVertices(int* outFloatCount, unsigned* outVersion)
{
    if (!DG_WorldReady()) { *outFloatCount = 0; if (outVersion) *outVersion = g_version; return NULL; }

    // Rebuild when the level changed (segs pointer / count differs).
    if (segs != g_builtSegs || numsegs != g_builtNumSegs)
        build_walls();

    *outFloatCount = g_vertCount * 4;         // 4 floats per vertex
    if (outVersion) *outVersion = g_version;
    return (const float*)g_verts;
}

void DG_GetView(float* pos3, float* yawRad, float* pitchRad)
{
    // Doom world: X,Y horizontal, Z up. Engine: X, Y-up, Z. Map (dx,dy,dz)->(x,z-up... )
    // We use engine pos = (doomX, doomZ_height, doomY).
    pos3[0] = FX(viewx);
    pos3[1] = FX(viewz);
    pos3[2] = FX(viewy);

    // viewangle is BAM (full circle = 2^32). Doom angle 0 = +X (east), CCW.
    *yawRad   = (float)((double)viewangle / 4294967296.0 * (2.0 * M_PI));
    *pitchRad = 0.0f;   // no vertical look yet (Fase 5 mouselook)
}
