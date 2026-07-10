// dg_render.c — geometry/texture/view bridge for the GPU renderer (plan.md).
//
// Extracts Doom level geometry (walls from segs, with UVs + per-texture draw
// groups), wall textures (as RGBA via PLAYPAL), and the per-frame view params
// into plain data the C++ DoomRenderPass consumes. All Doom-header access is
// confined here (its `boolean`/`fixed_t` clash with C++).

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomdef.h"
#include "doomstat.h"
#include "doomdata.h"     // ML_TWOSIDED / ML_DONTPEG*
#include "r_state.h"
#include "r_defs.h"
#include "r_data.h"       // R_GetColumn
#include "w_wad.h"
#include "z_zone.h"
#include "m_fixed.h"

#include "dg_bridge.h"

// texture_t and these globals are file-local to r_data.c (not in any header),
// but the arrays are non-static, so declare what we need. Width comes from the
// power-of-two column mask (+1); height from textureheight (fixed, >>FRACBITS).
extern int   numtextures;
extern int*  texturewidthmask;
// textureheight (fixed_t*) and texturetranslation (int*) come from r_state.h.

#define FX(a) ((float)(a) / (float)FRACUNIT)
#define TEX_W(t) (texturewidthmask[t] + 1)
#define TEX_H(t) (textureheight[t] >> FRACBITS)
#define TEX_VALID(t) ((t) > 0 && (t) < numtextures)

// Vertex: engine-space pos (Doom XY -> engine XZ, height -> Y-up) + UV + shade.
enum { FLOATS_PER_VERT = 6 };

// ── Temp quad list (built per level, then grouped by texture) ────────────────
typedef struct { int texid; float v[FLOATS_PER_VERT * 6]; } dg_quad_t;
static dg_quad_t* g_quads = NULL;
static int        g_quadCount = 0, g_quadCap = 0;

// ── Grouped output consumed by the host ──────────────────────────────────────
static float*     g_verts = NULL;      // FLOATS_PER_VERT per vertex
static int        g_vertCount = 0;
typedef struct { int texid, first, count; } dg_group_t;   // first/count in vertices
static dg_group_t* g_groups = NULL;
static int         g_groupCount = 0;

static unsigned   g_version = 0;
static seg_t*     g_builtSegs = NULL;
static int        g_builtNumSegs = -1;

// ── Texture RGBA cache (indexed by texture id) ───────────────────────────────
static unsigned char** g_texRGBA = NULL;
static int*            g_texW = NULL;
static int*            g_texH = NULL;
static int             g_texCacheN = 0;

static void quad_reserve(void) {
    if (g_quadCount >= g_quadCap) {
        g_quadCap = g_quadCap ? g_quadCap * 2 : 2048;
        g_quads = (dg_quad_t*)realloc(g_quads, g_quadCap * sizeof(dg_quad_t));
    }
}

// Emit one textured quad. Endpoints (x1,z1)-(x2,z2) in engine XZ; heights
// [ybot,ytop]; texture-space offsets uoff/roff (world units) and pegging
// reference refTop (world height at texture-top).
static void add_quad(int texid, float x1, float z1, float x2, float z2,
                     float ybot, float ytop, float uoff, float roff,
                     float refTop, float shade) {
    if (!TEX_VALID(texid) || ytop <= ybot) return;
    float texW = (float)TEX_W(texid); if (texW <= 0.f) texW = 64.f;
    float texH = (float)TEX_H(texid); if (texH <= 0.f) texH = 64.f;

    float segLen = sqrtf((x2 - x1) * (x2 - x1) + (z2 - z1) * (z2 - z1));
    float u1 = uoff / texW;
    float u2 = (uoff + segLen) / texW;
    float vTop = (refTop - ytop + roff) / texH;
    float vBot = (refTop - ybot + roff) / texH;

    quad_reserve();
    float* d = g_quads[g_quadCount].v;
    g_quads[g_quadCount].texid = texid;
    // A=(x1,ybot) B=(x2,ybot) C=(x2,ytop) D=(x1,ytop); tris A,B,C / A,C,D
    #define V(px,py,pz,pu,pv) do{ *d++=px; *d++=py; *d++=pz; *d++=pu; *d++=pv; *d++=shade; }while(0)
    V(x1,ybot,z1, u1,vBot);  V(x2,ybot,z2, u2,vBot);  V(x2,ytop,z2, u2,vTop);
    V(x1,ybot,z1, u1,vBot);  V(x2,ytop,z2, u2,vTop);  V(x1,ytop,z1, u1,vTop);
    #undef V
    g_quadCount++;
}

static int cmp_quad(const void* a, const void* b) {
    return ((const dg_quad_t*)a)->texid - ((const dg_quad_t*)b)->texid;
}

static void build_walls(void) {
    int i;
    g_quadCount = 0;

    for (i = 0; i < numsegs; i++) {
        seg_t*    seg = &segs[i];
        side_t*   sd  = seg->sidedef;
        sector_t* fs  = seg->frontsector;
        line_t*   ld  = seg->linedef;
        if (!fs || !sd || !ld) continue;

        float x1 = FX(seg->v1->x), z1 = FX(seg->v1->y);
        float x2 = FX(seg->v2->x), z2 = FX(seg->v2->y);
        float shade = fs->lightlevel / 255.0f;
        if (shade < 0.f) shade = 0.f; if (shade > 1.f) shade = 1.f;

        float ffloor = FX(fs->floorheight), fceil = FX(fs->ceilingheight);
        float uoff = FX(seg->offset) + FX(sd->textureoffset);
        float roff = FX(sd->rowoffset);

        if (!seg->backsector) {
            int tex = texturetranslation[sd->midtexture];
            float texH = TEX_VALID(tex) ? (float)TEX_H(tex) : 64.f;
            float refTop = (ld->flags & ML_DONTPEGBOTTOM) ? (ffloor + texH) : fceil;
            add_quad(tex, x1, z1, x2, z2, ffloor, fceil, uoff, roff, refTop, shade);
        } else {
            sector_t* bs = seg->backsector;
            float bfloor = FX(bs->floorheight), bceil = FX(bs->ceilingheight);
            if (bfloor > ffloor) {   // lower wall
                int tex = texturetranslation[sd->bottomtexture];
                float refTop = (ld->flags & ML_DONTPEGBOTTOM) ? fceil : bfloor;
                add_quad(tex, x1, z1, x2, z2, ffloor, bfloor, uoff, roff, refTop, shade);
            }
            if (bceil < fceil) {     // upper wall
                int tex = texturetranslation[sd->toptexture];
                float texH = TEX_VALID(tex) ? (float)TEX_H(tex) : 64.f;
                float refTop = (ld->flags & ML_DONTPEGTOP) ? fceil : (bceil + texH);
                add_quad(tex, x1, z1, x2, z2, bceil, fceil, uoff, roff, refTop, shade);
            }
        }
    }

    // Group by texture: sort quads, then emit vertices + contiguous groups.
    qsort(g_quads, g_quadCount, sizeof(dg_quad_t), cmp_quad);

    g_verts = (float*)realloc(g_verts, (size_t)g_quadCount * 6 * FLOATS_PER_VERT * sizeof(float));
    g_groups = (dg_group_t*)realloc(g_groups, (size_t)(g_quadCount + 1) * sizeof(dg_group_t));
    g_vertCount = 0; g_groupCount = 0;
    int curTex = -1;
    for (i = 0; i < g_quadCount; i++) {
        int tex = g_quads[i].texid;
        if (tex != curTex) {
            g_groups[g_groupCount].texid = tex;
            g_groups[g_groupCount].first = g_vertCount;
            g_groups[g_groupCount].count = 0;
            g_groupCount++;
            curTex = tex;
        }
        memcpy(&g_verts[g_vertCount * FLOATS_PER_VERT], g_quads[i].v,
               6 * FLOATS_PER_VERT * sizeof(float));
        g_vertCount += 6;
        g_groups[g_groupCount - 1].count += 6;
    }

    g_builtSegs = segs; g_builtNumSegs = numsegs; g_version++;
}

int DG_WorldReady(void) {
    return (gamestate == GS_LEVEL) && (numsegs > 0) && (segs != NULL);
}

const float* DG_WorldVertices(int* outFloatCount, unsigned* outVersion) {
    if (!DG_WorldReady()) { *outFloatCount = 0; if (outVersion) *outVersion = g_version; return NULL; }
    if (segs != g_builtSegs || numsegs != g_builtNumSegs) build_walls();
    *outFloatCount = g_vertCount * FLOATS_PER_VERT;
    if (outVersion) *outVersion = g_version;
    return g_verts;
}

int DG_WallGroupCount(void) { return g_groupCount; }

void DG_WallGroup(int i, int* texid, int* firstVert, int* vertCount) {
    if (i < 0 || i >= g_groupCount) { *texid = 0; *firstVert = 0; *vertCount = 0; return; }
    *texid = g_groups[i].texid; *firstVert = g_groups[i].first; *vertCount = g_groups[i].count;
}

const unsigned char* DG_WallTextureRGBA(int texid, int* w, int* h) {
    if (!TEX_VALID(texid)) { *w = *h = 0; return NULL; }

    if (g_texCacheN < numtextures) {   // grow cache arrays
        g_texRGBA = (unsigned char**)realloc(g_texRGBA, numtextures * sizeof(unsigned char*));
        g_texW    = (int*)realloc(g_texW, numtextures * sizeof(int));
        g_texH    = (int*)realloc(g_texH, numtextures * sizeof(int));
        for (int k = g_texCacheN; k < numtextures; k++) { g_texRGBA[k] = NULL; g_texW[k] = g_texH[k] = 0; }
        g_texCacheN = numtextures;
    }
    if (g_texRGBA[texid]) { *w = g_texW[texid]; *h = g_texH[texid]; return g_texRGBA[texid]; }

    int tw = TEX_W(texid), th = TEX_H(texid);
    unsigned char* out = (unsigned char*)malloc((size_t)tw * th * 4);
    const unsigned char* pal = (const unsigned char*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    for (int x = 0; x < tw; x++) {
        const unsigned char* col = (const unsigned char*)R_GetColumn(texid, x);
        for (int y = 0; y < th; y++) {
            int idx = col[y];
            unsigned char* o = &out[((size_t)y * tw + x) * 4];
            o[0] = pal[idx * 3 + 0]; o[1] = pal[idx * 3 + 1]; o[2] = pal[idx * 3 + 2]; o[3] = 255;
        }
    }
    g_texRGBA[texid] = out; g_texW[texid] = tw; g_texH[texid] = th;
    *w = tw; *h = th;
    return out;
}

void DG_GetView(float* pos3, float* yawRad, float* pitchRad) {
    pos3[0] = FX(viewx);
    pos3[1] = FX(viewz);
    pos3[2] = FX(viewy);
    *yawRad   = (float)((double)viewangle / 4294967296.0 * (2.0 * M_PI));
    *pitchRad = 0.0f;
}
