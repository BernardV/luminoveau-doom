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
#include "doomstat.h"     // gamestate, skyflatnum, players, consoleplayer
#include "doomdata.h"     // ML_TWOSIDED / ML_DONTPEG*
#include "r_state.h"
#include "r_defs.h"
#include "r_data.h"       // R_GetColumn
#include "r_main.h"       // R_PointToAngle2
#include "p_mobj.h"       // mobj_t
#include "p_local.h"      // thinkercap
#include "p_pspr.h"       // FF_FRAMEMASK / FF_FULLBRIGHT
#include "w_wad.h"
#include "z_zone.h"
#include "m_fixed.h"
#include "tables.h"       // ANG45, angle_t

#include "dg_bridge.h"

void P_MobjThinker();     // to match thinker.function.acp1 for mobj filtering

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

// Doom subsectors are convex but their boundary is NOT fully described by segs
// (1-2 segs is common; the rest are implicit BSP partition lines). So we can't
// fan seg endpoints. Instead, reconstruct each subsector's exact convex polygon
// by descending the BSP tree from the root and clipping a large quad by every
// partition half-plane along the path — each leaf = intersection of half-planes.

#define MAXPOLY 64

// Sutherland-Hodgman clip of a convex poly (interleaved x,z) by the partition
// half-plane. Side fn f = ndy*(x-nx) - ndx*(z-ny); keepFront keeps f>=0 (child 0).
static int clip_half(const float* in, int n, float* out,
                     float nx, float ny, float ndx, float ndy, int keepFront) {
    int m = 0;
    for (int i = 0; i < n && m < MAXPOLY - 1; i++) {
        int j = (i + 1) % n;
        float ax = in[2*i], az = in[2*i+1], bx = in[2*j], bz = in[2*j+1];
        float fa = ndy * (ax - nx) - ndx * (az - ny);
        float fb = ndy * (bx - nx) - ndx * (bz - ny);
        if (!keepFront) { fa = -fa; fb = -fb; }
        int ina = fa >= 0.0f, inb = fb >= 0.0f;
        if (ina) { out[2*m] = ax; out[2*m+1] = az; m++; }
        if (ina != inb && m < MAXPOLY - 1) {
            float t = fa / (fa - fb);
            out[2*m] = ax + t * (bx - ax); out[2*m+1] = az + t * (bz - az); m++;
        }
    }
    return m;
}

static void emit_flat_poly(int subidx, const float* poly, int n) {
    if (n < 3) return;
    subsector_t* sub = &subsectors[subidx];
    sector_t* sec = sub->sector;
    if (!sec) return;
    float shade = sec->lightlevel / 255.0f; if (shade < 0.f) shade = 0.f; if (shade > 1.f) shade = 1.f;
    float fh = FX(sec->floorheight), ch = FX(sec->ceilingheight);
    int fp = sec->floorpic, cp = sec->ceilingpic;
    // Group flats by their TRANSLATED (animated) flat index so animation shows:
    // flattranslation[picnum] cycles per tic for animated flats (nukage, lava…).
    int fflat = (fp != skyflatnum) ? flattranslation[fp] : -1;
    int cflat = (cp != skyflatnum) ? flattranslation[cp] : -1;

    for (int k = 1; k + 1 < n; k++) {
        float x0 = poly[0], z0 = poly[1], x1 = poly[2*k], z1 = poly[2*k+1], x2 = poly[2*(k+1)], z2 = poly[2*(k+1)+1];
        if (fflat >= 0) {
            float p0[6]={x0,fh,z0, x0/64.f,z0/64.f, shade};
            float p1[6]={x1,fh,z1, x1/64.f,z1/64.f, shade};
            float p2[6]={x2,fh,z2, x2/64.f,z2/64.f, shade};
            add_tri(KIND_FLAT, fflat, p0, p1, p2);
        }
        if (cflat >= 0) {
            float p0[6]={x0,ch,z0, x0/64.f,z0/64.f, shade};
            float p1[6]={x1,ch,z1, x1/64.f,z1/64.f, shade};
            float p2[6]={x2,ch,z2, x2/64.f,z2/64.f, shade};
            add_tri(KIND_FLAT, cflat, p0, p1, p2);
        }
    }
}

static void descend(int nodenum, const float* poly, int n) {
    if (n < 3) return;
    if (nodenum & NF_SUBSECTOR) { emit_flat_poly(nodenum & ~NF_SUBSECTOR, poly, n); return; }
    node_t* nd = &nodes[nodenum];
    float nx = FX(nd->x), ny = FX(nd->y), ndx = FX(nd->dx), ndy = FX(nd->dy);
    float buf[2 * MAXPOLY];
    int m = clip_half(poly, n, buf, nx, ny, ndx, ndy, 1);   // front → child 0
    descend(nd->children[0], buf, m);
    m = clip_half(poly, n, buf, nx, ny, ndx, ndy, 0);       // back  → child 1
    descend(nd->children[1], buf, m);
}

static void build_flats(void) {
    if (numnodes <= 0) return;
    // Large seed quad covering the whole map (Doom coords fit in +-32768).
    const float B = 32768.0f;
    float quad[8] = { -B,-B,  B,-B,  B,B,  -B,B };
    descend(numnodes - 1, quad, 4);
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
    // Rebuild every frame: reads live sector floor/ceiling heights (doors, lifts,
    // crushers move) and current texturetranslation/flattranslation (animated
    // walls + flats). Geometry is small (~thousands of verts), so this is cheap.
    build_world();
    *outFloatCount = g_vertCount * FLOATS_PER_VERT;
    if (outVersion) *outVersion = g_version;   // build_world bumps g_version → host re-uploads
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

// Flats are raw 64x64 palette-index lumps at firstflat + flatidx. flatidx is the
// already-translated (animated) flat index used as the draw-group id, so each
// animation frame is a distinct cache entry.
const unsigned char* DG_FlatTextureRGBA(int flatidx, int* w, int* h) {
    if (flatidx < 0) { *w = *h = 0; return NULL; }
    if (g_flatN <= flatidx) {
        int n = flatidx + 1;
        g_flatRGBA = (unsigned char**)realloc(g_flatRGBA, n * sizeof(unsigned char*));
        for (int k = g_flatN; k < n; k++) g_flatRGBA[k] = NULL;
        g_flatN = n;
    }
    if (g_flatRGBA[flatidx]) { *w = *h = 64; return g_flatRGBA[flatidx]; }
    const unsigned char* src = (const unsigned char*)W_CacheLumpNum(firstflat + flatidx, PU_CACHE);
    const unsigned char* pal = (const unsigned char*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    unsigned char* out = (unsigned char*)malloc(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
        int idx = src[i]; out[i*4]=pal[idx*3]; out[i*4+1]=pal[idx*3+1]; out[i*4+2]=pal[idx*3+2]; out[i*4+3]=255;
    }
    g_flatRGBA[flatidx] = out; *w = *h = 64; return out;
}

// Current sky texture id (a composite texture id, usable with DG_WallTextureRGBA).
int DG_SkyTextureId(void) {
    extern int skytexture;
    return skytexture;
}

// ── Sprites (things: monsters, items, decorations) ───────────────────────────
// Enumerated per frame from the thinker list, like R_ProjectSprite: pick the
// sprite lump for the current frame + view rotation (8 dirs) and flip.

typedef struct {
    float x, y, z;     // engine-space: x, feet-height (y-up), z
    float halfW;       // half sprite width in world units
    float top;         // world Y of the sprite's top
    int   lump;        // absolute sprite patch lump (firstspritelump + rel)
    int   flip;        // 1 = mirror U
    float shade;       // sector light (or 1 for fullbright)
} dg_sprite_t;

static dg_sprite_t* g_sprites = NULL;
static int g_spriteCount = 0, g_spriteCap = 0;

static void build_sprites(void) {
    g_spriteCount = 0;
    if (!thinkercap.next) return;

    angle_t viewa = viewangle;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t* mo = (mobj_t*)th;
        if (mo->player) continue;   // don't draw the player's own body

        if (mo->sprite < 0 || mo->sprite >= numsprites) continue;
        spritedef_t* sprdef = &sprites[mo->sprite];
        int frame = mo->frame & FF_FRAMEMASK;
        if (frame >= sprdef->numframes) continue;
        spriteframe_t* sf = &sprdef->spriteframes[frame];

        int rot = 0;
        if (sf->rotate) {
            angle_t ang = R_PointToAngle2(viewx, viewy, mo->x, mo->y);
            rot = (ang - mo->angle + (unsigned)(ANG45 / 2) * 9) >> 29;
        }
        int rel = sf->lump[rot];
        int flip = sf->flip[rot];
        int lump = firstspritelump + rel;

        float sw = FX(spritewidth[rel]);
        float soff = FX(spriteoffset[rel]);
        float stop = FX(spritetopoffset[rel]);

        if (g_spriteCount >= g_spriteCap) {
            g_spriteCap = g_spriteCap ? g_spriteCap * 2 : 256;
            g_sprites = (dg_sprite_t*)realloc(g_sprites, g_spriteCap * sizeof(dg_sprite_t));
        }
        dg_sprite_t* s = &g_sprites[g_spriteCount++];
        s->x = FX(mo->x); s->z = FX(mo->y);        // Doom XY -> engine XZ
        s->y = FX(mo->z);                          // feet
        s->halfW = sw * 0.5f;
        // leftoffset shifts the billboard horizontally; approximate by centering
        // on the sprite's origin: origin is `soff` from the left edge.
        s->top = FX(mo->z) + stop;                 // top of sprite above feet
        (void)soff;
        s->lump = lump; s->flip = flip;
        float shade = mo->subsector->sector->lightlevel / 255.0f;
        if (mo->frame & FF_FULLBRIGHT) shade = 1.0f;
        if (shade < 0.f) shade = 0.f; if (shade > 1.f) shade = 1.f;
        s->shade = shade;
    }
}

int DG_SpriteCount(void) { build_sprites(); return g_spriteCount; }

void DG_Sprite(int i, float* out8) {
    // out: x,y,z, halfW, top, lump, flip, shade
    if (i < 0 || i >= g_spriteCount) { for (int k=0;k<8;k++) out8[k]=0; return; }
    dg_sprite_t* s = &g_sprites[i];
    out8[0]=s->x; out8[1]=s->y; out8[2]=s->z; out8[3]=s->halfW;
    out8[4]=s->top; out8[5]=(float)s->lump; out8[6]=(float)s->flip; out8[7]=s->shade;
}

// Player weapon sprite (psprite) placement in Doom's 320x200 view space.
// idx: 0 = weapon, 1 = muzzle flash. Returns 1 if active; fills out6 =
// {lump, xLeft, yTop, w, h} in 320x200 pixels + flip (out6[5]). fullbright→out6[6].
int DG_WeaponSprite(int idx, float* out7) {
    for (int k=0;k<7;k++) out7[k]=0;
    player_t* pl = &players[consoleplayer];
    if (idx < 0 || idx >= NUMPSPRITES) return 0;
    pspdef_t* psp = &pl->psprites[idx];
    if (!psp->state) return 0;
    int sprite = psp->state->sprite;
    if (sprite < 0 || sprite >= numsprites) return 0;
    spritedef_t* sd = &sprites[sprite];
    int frame = psp->state->frame & FF_FRAMEMASK;
    if (frame >= sd->numframes) return 0;
    spriteframe_t* sf = &sd->spriteframes[frame];
    int rel = sf->lump[0];
    int flip = sf->flip[0];
    int lump = firstspritelump + rel;
    const patch_t* p = (const patch_t*)W_CacheLumpNum(lump, PU_CACHE);
    // Doom: x_left = (sx>>16) - leftoffset; y_top = (sy>>16) - topoffset (200-space).
    out7[0] = (float)lump;
    out7[1] = (float)(psp->sx >> FRACBITS) - (float)(spriteoffset[rel] >> FRACBITS);
    out7[2] = (float)(psp->sy >> FRACBITS) - (float)(spritetopoffset[rel] >> FRACBITS);
    out7[3] = (float)p->width;
    out7[4] = (float)p->height;
    out7[5] = (float)flip;
    out7[6] = (psp->state->frame & FF_FULLBRIGHT) ? 1.0f : 0.0f;
    return 1;
}

// Decode a sprite patch (posts) to RGBA with alpha (0 in transparent gaps).
static unsigned char** g_sprRGBA = NULL; static int* g_sprW = NULL; static int* g_sprH = NULL; static int g_sprN = 0;

const unsigned char* DG_SpriteTextureRGBA(int lump, int* w, int* h) {
    if (lump < 0) { *w = *h = 0; return NULL; }
    if (g_sprN <= lump) {
        int n = lump + 1;
        g_sprRGBA = (unsigned char**)realloc(g_sprRGBA, n * sizeof(unsigned char*));
        g_sprW = (int*)realloc(g_sprW, n * sizeof(int));
        g_sprH = (int*)realloc(g_sprH, n * sizeof(int));
        for (int k = g_sprN; k < n; k++) { g_sprRGBA[k]=NULL; g_sprW[k]=g_sprH[k]=0; }
        g_sprN = n;
    }
    if (g_sprRGBA[lump]) { *w = g_sprW[lump]; *h = g_sprH[lump]; return g_sprRGBA[lump]; }

    const patch_t* p = (const patch_t*)W_CacheLumpNum(lump, PU_CACHE);
    const unsigned char* pal = (const unsigned char*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    int pw = p->width, ph = p->height;
    unsigned char* out = (unsigned char*)calloc((size_t)pw * ph, 4);   // alpha 0 = transparent
    const unsigned char* base = (const unsigned char*)p;
    for (int x = 0; x < pw; x++) {
        const post_t* post = (const post_t*)(base + p->columnofs[x]);
        while (post->topdelta != 0xff) {
            const unsigned char* data = (const unsigned char*)post + 3;  // skip topdelta,length,pad
            for (int y = 0; y < post->length; y++) {
                int py = post->topdelta + y;
                if (py < 0 || py >= ph) continue;
                int idx = data[y];
                unsigned char* o = &out[((size_t)py * pw + x) * 4];
                o[0]=pal[idx*3]; o[1]=pal[idx*3+1]; o[2]=pal[idx*3+2]; o[3]=255;
            }
            post = (const post_t*)((const unsigned char*)post + post->length + 4); // next post
        }
    }
    g_sprRGBA[lump] = out; g_sprW[lump] = pw; g_sprH[lump] = ph;
    *w = pw; *h = ph; return out;
}

// Renderer-only pitch (look up/down) — Doom has no pitch concept, so the host
// tracks it from mouse Y and pushes it here. Radians; + = look up.
static float g_pitch = 0.0f;
void DG_SetPitch(float pitchRad) {
    g_pitch = pitchRad;
    // Feed Doom's freelook aiming: slope = tan(pitch) in 16.16 fixed.
    extern fixed_t dg_lookslope; extern int dg_freelook;
    dg_lookslope = (fixed_t)(tanf(pitchRad) * (float)FRACUNIT);
    dg_freelook = 1;
}

void DG_GetView(float* pos3, float* yawRad, float* pitchRad) {
    pos3[0] = FX(viewx); pos3[1] = FX(viewz); pos3[2] = FX(viewy);
    *yawRad = (float)((double)viewangle / 4294967296.0 * (2.0 * M_PI));
    *pitchRad = g_pitch;
}
