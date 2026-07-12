// dg_render.c — geometry/texture/view bridge for the GPU renderer (plan.md).
//
// Extracts Doom level geometry (walls from segs; floors/ceilings by fan-
// triangulating each convex subsector), textures (RGBA via PLAYPAL), and the
// per-frame view params into plain data the C++ DoomRenderPass consumes. All
// Doom-header access is confined here (its boolean/fixed_t clash with C++).

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>

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
#include "m_swap.h"       // SHORT (patch dimensions)
#include "tables.h"       // ANG45, angle_t

#include "dg_bridge.h"

void P_MobjThinker();     // to match thinker.function.acp1 for mobj filtering

// File-local-to-r_data.c globals we need (not header-exposed but non-static).
extern int   numtextures;
extern int*  texturewidthmask;
extern int   firstflat;
// textureheight, texturetranslation, flattranslation, firstflat: r_state.h.

extern int extralight;

// Muzzle-flash / light-amp brightness as a 0..1 level, fed to the fragment
// shaders as a localized point light at the eye (falls off with distance) rather
// than a flat global boost — firing lights up nearby walls, not the whole level.
float DG_FlashLevel(void) {
    if (extralight <= 0) return 0.0f;
    float f = extralight * 0.35f;
    return f > 1.0f ? 1.0f : f;
}

// Whether the console player owns the weapon in number-key slot 1..7. Slots map to
// weapontype wp_fist..wp_bfg (0..6), the same as Doom's '1'..'7' keys. Used by the
// touch weapon-cycle button to skip weapons you don't have.
int DG_WeaponOwned(int slot) {
    if (slot < 1 || slot > 7) return 0;
    return players[consoleplayer].weaponowned[slot - 1] ? 1 : 0;
}

// Whether the console player is dead (respawn on BT_USE). Lets the touch FIRE button
// also respawn — P_DeathThink only reads BT_USE, so plain fire wouldn't otherwise.
int DG_PlayerDead(void) {
    return players[consoleplayer].playerstate == PST_DEAD;
}

// ── HUD message + font (drawn by the GPU overlay, on top of the 3D) ──────────
// The software HUD message lives in screens[0]'s top region, which the GPU 3D
// draws over, so we mirror it out and re-draw it with Doom's own font.
extern char dg_hud_message[];        // hu_stuff.c
extern patch_t* hu_font[];           // hu_stuff.c: glyphs '!'..'_' at [c-'!']

const char* DG_HudMessage(void) { return dg_hud_message; }

// Font glyph for a character. Returns 1 and fills *lump/*w/*h for a drawable
// glyph (fetch RGBA via DG_SpriteTextureRGBA(lump)); 0 for space/non-printable
// (caller advances by DG_FONT_SPACE). Mirrors HUlib_drawTextLine.
int DG_FontGlyph(int ch, int* lump, int* w, int* h) {
    int c = toupper(ch);
    if (c < '!' || c > '_') return 0;
    patch_t* p = hu_font[c - '!'];
    if (!p) return 0;
    char name[16];
    sprintf(name, "STCFN%.3d", c);
    *lump = W_GetNumForName(name);
    *w = SHORT(p->width);
    *h = SHORT(p->height);
    return 1;
}

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

// A fullscreen software UI is active (menu, pause, automap, or a non-level
// gamestate like the intermission/finale/title). The host uses this to show the
// software render (which draws these) instead of the GPU 3D, and to free the mouse.
int DG_UIActive(void) {
    extern boolean menuactive, paused, automapactive;
    return menuactive || paused || automapactive || (gamestate != GS_LEVEL);
}

// GPU-sole-renderer mode (Fase 7). dg_gpu_active is read by R_RenderPlayerView
// (r_main.c) to skip the software 3D and by I_FinishUpdate (i_lumi.c) to key the
// transparent sentinel; the host toggles it here.
int dg_gpu_active = 0;
void DG_SetSoleRenderer(int on) { dg_gpu_active = on ? 1 : 0; }
int  DG_SoleRenderer(void)      { return dg_gpu_active; }

// A 3D level view should be GPU-drawn: in a level, not in the automap. Stays true
// while an in-game menu/pause is up so the menu composites over the GPU 3D.
int DG_Show3D(void) {
    extern boolean automapactive;
    return (gamestate == GS_LEVEL) && !automapactive && DG_WorldReady();
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

// ── Dynamic colored point lights ─────────────────────────────────────────────
// Gathered per frame from light-emitting mobjs: projectiles (colored by type)
// and fullbright decorations (torches/lamps). The nearest DG_MAX_LIGHTS to the
// camera are fed to the world/sprite shaders which add a colored, distance-
// attenuated contribution.
typedef struct { float x, y, z, r, g, b, rad, dist2; } dg_light_t;
static dg_light_t g_lights[DG_MAX_LIGHTS];
static int        g_lightCount = 0;

extern char* sprnames[];   // info.c: 4-char sprite names indexed by spritenum

// Classify a light-emitting mobj. Projectiles are coloured by type; static
// decorations only if they're an actual lamp/torch sprite (NOT every fullbright
// thing — items/ammo have fullbright frames too and would flood the light set,
// causing nearby lamps to pop on/off as the nearest-N set churns while moving).
// Returns 1 and fills colour/radius, or 0 if not a light.
static int light_color(mobj_t* mo, float* r, float* g, float* b, float* rad) {
    if (mo->flags & MF_MISSILE) {
        switch (mo->type) {
            case MT_TROOPSHOT:   *r=1.0f; *g=0.45f;*b=0.10f; *rad=170; return 1; // imp fireball
            case MT_FATSHOT:     *r=1.0f; *g=0.50f;*b=0.15f; *rad=170; return 1;
            case MT_HEADSHOT:    *r=0.55f;*g=0.35f;*b=1.00f; *rad=170; return 1; // caco
            case MT_BRUISERSHOT: *r=0.30f;*g=1.00f;*b=0.25f; *rad=180; return 1; // baron/knight
            case MT_ROCKET:      *r=0.80f;*g=0.55f;*b=0.25f; *rad=130; return 1;
            case MT_PLASMA:      *r=0.35f;*g=0.55f;*b=1.00f; *rad=160; return 1;
            case MT_ARACHPLAZ:   *r=0.40f;*g=0.60f;*b=1.00f; *rad=160; return 1;
            case MT_BFG:         *r=0.35f;*g=1.00f;*b=0.40f; *rad=320; return 1; // big green
            default:             *r=0.95f;*g=0.70f;*b=0.40f; *rad=150; return 1;
        }
    }
    // Static light decorations, keyed by 4-char sprite name (per-type colour).
    if (mo->sprite < 0 || mo->sprite >= numsprites) return 0;
    const char* s = sprnames[mo->sprite];
    if (!s) return 0;
    #define SPR4(a,b_,c,d) (s[0]==(a)&&s[1]==(b_)&&s[2]==(c)&&s[3]==(d))
    if (SPR4('T','L','M','P') || SPR4('T','L','P','2') || SPR4('C','O','L','U'))
        { *r=1.00f; *g=0.95f; *b=0.80f; *rad=210; return 1; }   // tech lamps
    if (SPR4('C','B','R','A'))
        { *r=1.00f; *g=0.78f; *b=0.45f; *rad=180; return 1; }   // candelabra
    if (SPR4('C','A','N','D'))
        { *r=1.00f; *g=0.82f; *b=0.50f; *rad=110; return 1; }   // candle
    if (SPR4('T','R','E','D') || SPR4('S','M','R','T'))
        { *r=1.00f; *g=0.42f; *b=0.20f; *rad=190; return 1; }   // red torch
    if (SPR4('T','B','L','U') || SPR4('S','M','B','T'))
        { *r=0.40f; *g=0.55f; *b=1.00f; *rad=190; return 1; }   // blue torch
    if (SPR4('T','G','R','N') || SPR4('S','M','G','T'))
        { *r=0.40f; *g=1.00f; *b=0.55f; *rad=190; return 1; }   // green torch
    if (SPR4('F','C','A','N'))
        { *r=1.00f; *g=0.50f; *b=0.15f; *rad=210; return 1; }   // flaming barrel
    #undef SPR4
    return 0;
}

static void gather_lights(void) {
    g_lightCount = 0;
    if (!thinkercap.next) return;
    float cx = FX(viewx), cz = FX(viewy);

    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t* mo = (mobj_t*)th;
        if (mo->player) continue;

        dg_light_t L;
        if (!light_color(mo, &L.r, &L.g, &L.b, &L.rad)) continue;
        L.x = FX(mo->x); L.z = FX(mo->y);
        L.y = FX(mo->z) + FX(mo->height) * 0.5f;
        float dx = L.x - cx, dz = L.z - cz;
        L.dist2 = dx*dx + dz*dz;
        // Cull on CAMERA distance, but generously: the lamp lights its surrounding
        // walls by wall→lamp distance (always, consistently), so it must stay in the
        // set well beyond its own radius — otherwise its lit walls blink off as the
        // player walks away. A large margin keeps the light on until it's genuinely
        // off-screen; distant walls get ~0 anyway via the wall→lamp falloff.
        float cull = L.rad + 900.0f;
        if (L.dist2 > cull * cull) continue;

        if (g_lightCount < DG_MAX_LIGHTS) {
            g_lights[g_lightCount++] = L;
        } else {
            // Replace the farthest kept light if this one is nearer.
            int far = 0;
            for (int k = 1; k < DG_MAX_LIGHTS; k++)
                if (g_lights[k].dist2 > g_lights[far].dist2) far = k;
            if (L.dist2 < g_lights[far].dist2) g_lights[far] = L;
        }
    }

    // Optional verification: a bright cyan light at the player's feet-ish.
    if (getenv("DOOM_LIGHTTEST") && g_lightCount < DG_MAX_LIGHTS) {
        mobj_t* pmo = players[consoleplayer].mo;
        if (pmo) {
            dg_light_t L = { FX(pmo->x), FX(pmo->z)+30.0f, FX(pmo->y),
                             0.2f, 1.0f, 1.0f, 400.0f, 0.0f };
            g_lights[g_lightCount++] = L;
        }
    }
}

int DG_LightCount(void) { gather_lights(); return g_lightCount; }

void DG_Light(int i, float* out7) {
    if (i < 0 || i >= g_lightCount) { for (int k=0;k<7;k++) out7[k]=0; return; }
    dg_light_t* L = &g_lights[i];
    out7[0]=L->x; out7[1]=L->y; out7[2]=L->z;
    out7[3]=L->r; out7[4]=L->g; out7[5]=L->b; out7[6]=L->rad;
}

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
