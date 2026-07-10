// dg_render.c — geometry/texture/view bridge for the GPU renderer (plan.md).
//
// Extracts Doom level geometry (walls from segs; floors/ceilings by fan-
// triangulating each convex subsector), textures (RGBA via PLAYPAL), and the
// per-frame view params into plain data the C++ DoomRenderPass consumes. All
// Doom-header access is confined here (its boolean/fixed_t clash with C++).

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomdef.h"
#include "doomstat.h"     // gamestate, skyflatnum
#include "doomdata.h"     // ML_TWOSIDED / ML_DONTPEG*
#include "r_state.h"
#include "r_defs.h"
#include "r_data.h"       // R_GetColumn
#include "w_wad.h"
#include "z_zone.h"
#include "m_fixed.h"

#include "dg_bridge.h"

// File-local-to-r_data.c globals we need (not header-exposed but non-static).
extern int   numtextures;
extern int*  texturewidthmask;
extern int   firstflat;
// textureheight, texturetranslation, flattranslation, firstflat: r_state.h.

#define FX(a) ((float)(a) / (float)FRACUNIT)
#define TEX_W(t) (texturewidthmask[t] + 1)
#define TEX_H(t) (textureheight[t] >> FRACBITS)
#define TEX_VALID(t) ((t) > 0 && (t) < numtextures)

enum { KIND_WALL = 0, KIND_FLAT = 1 };
enum { FLOATS_PER_VERT = 6 };   // x,y,z, u,v, shade

// ── Triangle list (built per level, then grouped by (kind,texid)) ────────────
typedef struct { int kind, texid; float v[FLOATS_PER_VERT * 3]; } dg_tri_t;
static dg_tri_t* g_tris = NULL;
static int       g_triCount = 0, g_triCap = 0;

// ── Grouped output ───────────────────────────────────────────────────────────
static float*    g_verts = NULL;
static int       g_vertCount = 0;
typedef struct { int kind, texid, first, count; } dg_group_t;
static dg_group_t* g_groups = NULL;
static int         g_groupCount = 0;

static unsigned g_version = 0;
static seg_t*   g_builtSegs = NULL;
static int      g_builtNumSegs = -1;

// ── Texture caches (RGBA), separate namespaces for wall textures and flats ───
static unsigned char** g_texRGBA = NULL; static int* g_texW = NULL; static int* g_texH = NULL; static int g_texN = 0;
static unsigned char** g_flatRGBA = NULL; static int g_flatN = 0;

static void tri_reserve(void) {
    if (g_triCount >= g_triCap) {
        g_triCap = g_triCap ? g_triCap * 2 : 4096;
        g_tris = (dg_tri_t*)realloc(g_tris, g_triCap * sizeof(dg_tri_t));
    }
}
static void add_tri(int kind, int texid, const float* a, const float* b, const float* c) {
    tri_reserve();
    dg_tri_t* t = &g_tris[g_triCount++];
    t->kind = kind; t->texid = texid;
    memcpy(t->v + 0,  a, FLOATS_PER_VERT * sizeof(float));
    memcpy(t->v + 6,  b, FLOATS_PER_VERT * sizeof(float));
    memcpy(t->v + 12, c, FLOATS_PER_VERT * sizeof(float));
}

// Wall quad → 2 triangles. Corners A/B/C/D as described in add_quad.
static void add_quad(int texid, float x1, float z1, float x2, float z2,
                     float ybot, float ytop, float uoff, float roff,
                     float refTop, float shade) {
    if (!TEX_VALID(texid) || ytop <= ybot) return;
    float texW = (float)TEX_W(texid); if (texW <= 0.f) texW = 64.f;
    float texH = (float)TEX_H(texid); if (texH <= 0.f) texH = 64.f;
    float segLen = sqrtf((x2 - x1) * (x2 - x1) + (z2 - z1) * (z2 - z1));
    float u1 = uoff / texW, u2 = (uoff + segLen) / texW;
    float vTop = (refTop - ytop + roff) / texH, vBot = (refTop - ybot + roff) / texH;
    float A[6] = {x1,ybot,z1, u1,vBot, shade};
    float B[6] = {x2,ybot,z2, u2,vBot, shade};
    float C[6] = {x2,ytop,z2, u2,vTop, shade};
    float D[6] = {x1,ytop,z1, u1,vTop, shade};
    add_tri(KIND_WALL, texid, A, B, C);
    add_tri(KIND_WALL, texid, A, C, D);
}

static void build_walls(void) {
    for (int i = 0; i < numsegs; i++) {
        seg_t* seg = &segs[i]; side_t* sd = seg->sidedef;
        sector_t* fs = seg->frontsector; line_t* ld = seg->linedef;
        if (!fs || !sd || !ld) continue;
        float x1 = FX(seg->v1->x), z1 = FX(seg->v1->y);
        float x2 = FX(seg->v2->x), z2 = FX(seg->v2->y);
        float shade = fs->lightlevel / 255.0f; if (shade < 0.f) shade = 0.f; if (shade > 1.f) shade = 1.f;
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
            if (bfloor > ffloor) {
                int tex = texturetranslation[sd->bottomtexture];
                float refTop = (ld->flags & ML_DONTPEGBOTTOM) ? fceil : bfloor;
                add_quad(tex, x1, z1, x2, z2, ffloor, bfloor, uoff, roff, refTop, shade);
            }
            if (bceil < fceil) {
                int tex = texturetranslation[sd->toptexture];
                float texH = TEX_VALID(tex) ? (float)TEX_H(tex) : 64.f;
                float refTop = (ld->flags & ML_DONTPEGTOP) ? fceil : (bceil + texH);
                add_quad(tex, x1, z1, x2, z2, bceil, fceil, uoff, roff, refTop, shade);
            }
        }
    }
}

// Fan-triangulate one subsector's floor+ceiling. Subsectors are convex, so the
// ordered ring of seg endpoints (implicit BSP edges = straight segments between
// consecutive seg endpoints) fans correctly.
static void build_flats(void) {
    static float ringX[256], ringZ[256];
    for (int s = 0; s < numsubsectors; s++) {
        subsector_t* sub = &subsectors[s];
        sector_t* sec = sub->sector;
        if (!sec || sub->numlines < 3) continue;

        int n = 0;
        for (int k = 0; k < sub->numlines && n < 254; k++) {
            seg_t* seg = &segs[sub->firstline + k];
            float ax = FX(seg->v1->x), az = FX(seg->v1->y);
            float bx = FX(seg->v2->x), bz = FX(seg->v2->y);
            if (n == 0 || ringX[n-1] != ax || ringZ[n-1] != az) { ringX[n]=ax; ringZ[n]=az; n++; }
            if (n < 255) { ringX[n]=bx; ringZ[n]=bz; n++; }
        }
        if (n > 1 && ringX[n-1] == ringX[0] && ringZ[n-1] == ringZ[0]) n--;  // drop closing dup
        if (n < 3) continue;

        float shade = sec->lightlevel / 255.0f; if (shade < 0.f) shade = 0.f; if (shade > 1.f) shade = 1.f;
        float fh = FX(sec->floorheight), ch = FX(sec->ceilingheight);
        int floorPic = sec->floorpic, ceilPic = sec->ceilingpic;

        // Floor (skip sky). Fan from ring[0].
        if (floorPic != skyflatnum) {
            for (int k = 1; k + 1 < n; k++) {
                float p0[6] = {ringX[0], fh, ringZ[0], ringX[0]/64.f, ringZ[0]/64.f, shade};
                float p1[6] = {ringX[k], fh, ringZ[k], ringX[k]/64.f, ringZ[k]/64.f, shade};
                float p2[6] = {ringX[k+1], fh, ringZ[k+1], ringX[k+1]/64.f, ringZ[k+1]/64.f, shade};
                add_tri(KIND_FLAT, floorPic, p0, p1, p2);
            }
        }
        // Ceiling (skip sky).
        if (ceilPic != skyflatnum) {
            for (int k = 1; k + 1 < n; k++) {
                float p0[6] = {ringX[0], ch, ringZ[0], ringX[0]/64.f, ringZ[0]/64.f, shade};
                float p1[6] = {ringX[k], ch, ringZ[k], ringX[k]/64.f, ringZ[k]/64.f, shade};
                float p2[6] = {ringX[k+1], ch, ringZ[k+1], ringX[k+1]/64.f, ringZ[k+1]/64.f, shade};
                add_tri(KIND_FLAT, ceilPic, p0, p1, p2);
            }
        }
    }
}

static int cmp_tri(const void* a, const void* b) {
    const dg_tri_t* x = (const dg_tri_t*)a; const dg_tri_t* y = (const dg_tri_t*)b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return x->texid - y->texid;
}

static void build_world(void) {
    g_triCount = 0;
    build_walls();
    build_flats();

    qsort(g_tris, g_triCount, sizeof(dg_tri_t), cmp_tri);

    g_verts  = (float*)realloc(g_verts, (size_t)g_triCount * 3 * FLOATS_PER_VERT * sizeof(float));
    g_groups = (dg_group_t*)realloc(g_groups, (size_t)(g_triCount + 1) * sizeof(dg_group_t));
    g_vertCount = 0; g_groupCount = 0;
    int curKind = -1, curTex = -2;
    for (int i = 0; i < g_triCount; i++) {
        if (g_tris[i].kind != curKind || g_tris[i].texid != curTex) {
            g_groups[g_groupCount].kind  = curKind = g_tris[i].kind;
            g_groups[g_groupCount].texid = curTex  = g_tris[i].texid;
            g_groups[g_groupCount].first = g_vertCount;
            g_groups[g_groupCount].count = 0;
            g_groupCount++;
        }
        memcpy(&g_verts[g_vertCount * FLOATS_PER_VERT], g_tris[i].v, 3 * FLOATS_PER_VERT * sizeof(float));
        g_vertCount += 3;
        g_groups[g_groupCount - 1].count += 3;
    }

    g_builtSegs = segs; g_builtNumSegs = numsegs; g_version++;
}

int DG_WorldReady(void) {
    return (gamestate == GS_LEVEL) && (numsegs > 0) && (segs != NULL);
}

const float* DG_WorldVertices(int* outFloatCount, unsigned* outVersion) {
    if (!DG_WorldReady()) { *outFloatCount = 0; if (outVersion) *outVersion = g_version; return NULL; }
    if (segs != g_builtSegs || numsegs != g_builtNumSegs) build_world();
    *outFloatCount = g_vertCount * FLOATS_PER_VERT;
    if (outVersion) *outVersion = g_version;
    return g_verts;
}

int DG_DrawGroupCount(void) { return g_groupCount; }

void DG_DrawGroup(int i, int* kind, int* texid, int* firstVert, int* vertCount) {
    if (i < 0 || i >= g_groupCount) { *kind = 0; *texid = 0; *firstVert = 0; *vertCount = 0; return; }
    *kind = g_groups[i].kind; *texid = g_groups[i].texid;
    *firstVert = g_groups[i].first; *vertCount = g_groups[i].count;
}

const unsigned char* DG_WallTextureRGBA(int texid, int* w, int* h) {
    if (!TEX_VALID(texid)) { *w = *h = 0; return NULL; }
    if (g_texN < numtextures) {
        g_texRGBA = (unsigned char**)realloc(g_texRGBA, numtextures * sizeof(unsigned char*));
        g_texW = (int*)realloc(g_texW, numtextures * sizeof(int));
        g_texH = (int*)realloc(g_texH, numtextures * sizeof(int));
        for (int k = g_texN; k < numtextures; k++) { g_texRGBA[k] = NULL; g_texW[k] = g_texH[k] = 0; }
        g_texN = numtextures;
    }
    if (g_texRGBA[texid]) { *w = g_texW[texid]; *h = g_texH[texid]; return g_texRGBA[texid]; }
    int tw = TEX_W(texid), th = TEX_H(texid);
    unsigned char* out = (unsigned char*)malloc((size_t)tw * th * 4);
    const unsigned char* pal = (const unsigned char*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    for (int x = 0; x < tw; x++) {
        const unsigned char* col = (const unsigned char*)R_GetColumn(texid, x);
        for (int y = 0; y < th; y++) {
            int idx = col[y]; unsigned char* o = &out[((size_t)y * tw + x) * 4];
            o[0] = pal[idx*3]; o[1] = pal[idx*3+1]; o[2] = pal[idx*3+2]; o[3] = 255;
        }
    }
    g_texRGBA[texid] = out; g_texW[texid] = tw; g_texH[texid] = th;
    *w = tw; *h = th; return out;
}

// Flats are raw 64x64 palette-index lumps at firstflat + flattranslation[picnum].
const unsigned char* DG_FlatTextureRGBA(int picnum, int* w, int* h) {
    if (picnum < 0) { *w = *h = 0; return NULL; }
    if (g_flatN <= picnum) {
        int n = picnum + 1;
        g_flatRGBA = (unsigned char**)realloc(g_flatRGBA, n * sizeof(unsigned char*));
        for (int k = g_flatN; k < n; k++) g_flatRGBA[k] = NULL;
        g_flatN = n;
    }
    if (g_flatRGBA[picnum]) { *w = *h = 64; return g_flatRGBA[picnum]; }
    const unsigned char* src = (const unsigned char*)W_CacheLumpNum(firstflat + flattranslation[picnum], PU_CACHE);
    const unsigned char* pal = (const unsigned char*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    unsigned char* out = (unsigned char*)malloc(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
        int idx = src[i]; out[i*4]=pal[idx*3]; out[i*4+1]=pal[idx*3+1]; out[i*4+2]=pal[idx*3+2]; out[i*4+3]=255;
    }
    g_flatRGBA[picnum] = out; *w = *h = 64; return out;
}

void DG_GetView(float* pos3, float* yawRad, float* pitchRad) {
    pos3[0] = FX(viewx); pos3[1] = FX(viewz); pos3[2] = FX(viewy);
    *yawRad = (float)((double)viewangle / 4294967296.0 * (2.0 * M_PI));
    *pitchRad = 0.0f;
}
