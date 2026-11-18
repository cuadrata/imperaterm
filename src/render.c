/* See LICENSE for licence details. */
/*
 * Drawing. A terminal is a texture with two texels a cell: the colours,
 * where the glyph is and what lines go through it. One triangle covers
 * the window and the fragment shader looks each pixel up: in the cell it
 * is in, then in the atlas the glyphs are kept in. Nothing is drawn twice,
 * nothing is blended, no integer is divided, and a frame costs the same
 * whatever is on the screen.
 *
 * GL is called through pointers GDK finds, so that the same program runs
 * on OpenGL 3.1 and on OpenGL ES 3.0 with neither library linked.
 */
#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <gtkgl/gdkgl.h>

#include "render.h"

#define GL_FUNCS(X) \
    X(void, Viewport, (int, int, int, int)) \
    X(unsigned, CreateShader, (unsigned)) \
    X(void, ShaderSource, (unsigned, int, const char *const *, const int *)) \
    X(void, CompileShader, (unsigned)) \
    X(void, GetShaderiv, (unsigned, unsigned, int *)) \
    X(void, GetShaderInfoLog, (unsigned, int, int *, char *)) \
    X(unsigned, CreateProgram, (void)) \
    X(void, AttachShader, (unsigned, unsigned)) \
    X(void, LinkProgram, (unsigned)) \
    X(void, GetProgramiv, (unsigned, unsigned, int *)) \
    X(void, UseProgram, (unsigned)) \
    X(int, GetUniformLocation, (unsigned, const char *)) \
    X(void, Uniform1i, (int, int)) \
    X(void, Uniform4i, (int, int, int, int, int)) \
    X(void, Uniform4f, (int, float, float, float, float)) \
    X(void, Uniform3f, (int, float, float, float)) \
    X(void, GenTextures, (int, unsigned *)) \
    X(void, DeleteTextures, (int, const unsigned *)) \
    X(void, BindTexture, (unsigned, unsigned)) \
    X(void, TexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *)) \
    X(void, TexSubImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *)) \
    X(void, TexParameteri, (unsigned, unsigned, int)) \
    X(void, ActiveTexture, (unsigned)) \
    X(void, PixelStorei, (unsigned, int)) \
    X(void, GenVertexArrays, (int, unsigned *)) \
    X(void, DeleteVertexArrays, (int, const unsigned *)) \
    X(void, BindVertexArray, (unsigned)) \
    X(void, DrawArrays, (unsigned, int, int)) \
    X(const unsigned char *, GetString, (unsigned)) \
    X(void, GetIntegerv, (unsigned, int *))

#define X(r, n, a) static r (*gl##n) a;
GL_FUNCS(X)
#undef X

enum {
    GL_TRIANGLES = 4, GL_MAX_TEXTURE_SIZE = 0x0d33, GL_UNPACK_ALIGNMENT = 0x0cf5,
    GL_TEXTURE_2D = 0x0de1, GL_UNSIGNED_BYTE = 0x1401, GL_RED = 0x1903, GL_RGBA = 0x1908,
    GL_VERSION = 0x1f02, GL_NEAREST = 0x2600, GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801, GL_RGBA8 = 0x8058, GL_R8 = 0x8229, GL_TEXTURE0 = 0x84c0,
    GL_FRAGMENT_SHADER = 0x8b30, GL_VERTEX_SHADER = 0x8b31, GL_COMPILE_STATUS = 0x8b81,
    GL_LINK_STATUS = 0x8b82
};

static const char vertex[] =
    "void main() {\n"
    "    gl_Position = vec4(float((gl_VertexID << 1 & 2) * 2 - 1), float((gl_VertexID & 2) * 2 - 1), 0.0, 1.0);\n"
    "}\n";

/*
 * The cursor is drawn in its own colour, and the letter under a block in
 * the colour of the cell's ground. Where those two are all but the same,
 * as on reversed text in a set whose cursor is the colour of its text,
 * the cursor takes the colour of the cell's text: a block there is the
 * cell reversed, and a line is seen on it.
 */
static const char fragment[] =
    "uniform highp sampler2D cells, atlas;\n"
    "uniform ivec4 geo;\n"          /* a cell's width and height, the padding, the window's height */
    "uniform vec4 grid;\n"          /* columns, rows, and one over a cell's width and height */
    "uniform ivec4 lines;\n"        /* underline: where and how thick; and strike through */
    "uniform ivec4 cur;\n"          /* the cursor's column, row, shape and thickness */
    "uniform vec3 back, curcol;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    vec2 q = vec2(gl_FragCoord.x, float(geo.w) - gl_FragCoord.y) - float(geo.z);\n"
    "    vec2 cf = floor(q * grid.zw);\n"
    "    if (q.x < 0.0 || q.y < 0.0 || cf.x >= grid.x || cf.y >= grid.y) {\n"
    "        o = vec4(back, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    ivec2 c = ivec2(cf);\n"
    "    ivec2 s = ivec2(q) - c * geo.xy;\n"
    "    vec4 a = texelFetch(cells, ivec2(c.x * 2, c.y), 0);\n"
    "    vec4 b = texelFetch(cells, ivec2(c.x * 2 + 1, c.y), 0);\n"
    "    int f = int(b.a * 255.0 + 0.5);\n"
    "    float v = texelFetch(atlas, ivec2(int(a.a * 255.0 + 0.5), f & 63) * geo.xy + s, 0).r;\n"
    "    if (((f & 64) != 0 && s.y >= lines.x && s.y < lines.x + lines.y) ||\n"
    "        ((f & 128) != 0 && s.y >= lines.z && s.y < lines.z + lines.w))\n"
    "        v = 1.0;\n"
    "    vec3 fg = a.rgb, bg = b.rgb;\n"
    "    if (c == cur.xy) {\n"
    "        vec3 d = curcol - bg;\n"
    "        vec3 cc = dot(d, d) < 0.09 ? fg : curcol;\n"
    "        if (cur.z == 0) {\n"
    "            fg = bg;\n"
    "            bg = cc;\n"
    "        } else if (cur.z == 1 ? s.y >= geo.y - cur.w : cur.z == 2 ? s.x < cur.w :\n"
    "                   s.x < cur.w || s.y < cur.w || s.x >= geo.x - cur.w || s.y >= geo.y - cur.w) {\n"
    "            v = 1.0;\n"
    "            fg = cc;\n"
    "        }\n"
    "    }\n"
    "    o = vec4(mix(bg, fg, v), 1.0);\n"
    "}\n";

/* A glyph's place in the atlas is a column and a row, a byte and six bits. */
enum { NHASH = 1 << 15, NCOL = 256, NROW = 64, NFALLBACK = 16 };

int cellw, cellh;

static struct {
    FT_Library ft;
    FT_Face face[4], fb[NFALLBACK];  /* regular, bold, italic, both; and what covers the rest */
    int synth[4], nfb, px, asc;
    char *family, *version;
    uint8_t *bitmap;  /* a glyph being drawn, two cells wide */
    unsigned prog, atlas;
    int ready, aw, ah, per, nslot, next, gen;
    int ugeo, ugrid, ulines, ucur, uback, ucurcol;
    uint16_t ascii[4][128], hv[NHASH];
    uint32_t hk[NHASH];
} R;

/* Fonts. */

static FT_Face
openface(FcPattern *want, int *synth)
{
    FcResult res;
    FcPattern *m;
    FcChar8 *file;
    FcMatrix *mx;
    FcBool b;
    FT_Face f = NULL;
    int idx = 0, i, best = 0;

    FcConfigSubstitute(NULL, want, FcMatchPattern);
    FcDefaultSubstitute(want);
    m = FcFontMatch(NULL, want, &res);
    FcPatternDestroy(want);
    if (!m)
        return NULL;
    FcPatternGetInteger(m, FC_INDEX, 0, &idx);
    if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && !FT_New_Face(R.ft, (char *)file, idx, &f)) {
        if (FT_IS_SCALABLE(f))
            FT_Set_Pixel_Sizes(f, 0, R.px);
        else if (f->num_fixed_sizes) {  /* a bitmap face: the strike nearest */
            for (i = 1; i < f->num_fixed_sizes; i++)
                if (abs(f->available_sizes[i].height - R.px) < abs(f->available_sizes[best].height - R.px))
                    best = i;
            FT_Select_Size(f, best);
        }
        if (synth) {
            *synth = FcPatternGetBool(m, FC_EMBOLDEN, 0, &b) == FcResultMatch && b;
            if (FcPatternGetMatrix(m, FC_MATRIX, 0, &mx) == FcResultMatch && mx->xy != 0)
                *synth |= 2;
        }
    }
    FcPatternDestroy(m);
    return f;
}

static FT_Face
face(int style)
{
    if (!R.face[style])
        R.face[style] = openface(FcPatternBuild(NULL, FC_FAMILY, FcTypeString, R.family,
            FC_PIXEL_SIZE, FcTypeDouble, (double)R.px,
            FC_WEIGHT, FcTypeInteger, style & 1 ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR,
            FC_SLANT, FcTypeInteger, style & 2 ? FC_SLANT_ITALIC : FC_SLANT_ROMAN, NULL), &R.synth[style]);
    return R.face[style] ? R.face[style] : R.face[0];
}

/* A face that has the character the user's font lacks. */
static FT_Face
fallback(uint32_t u)
{
    FcCharSet *cs;
    FT_Face f;
    int i;

    for (i = 0; i < R.nfb; i++)
        if (FT_Get_Char_Index(R.fb[i], u))
            return R.fb[i];
    if (R.nfb == NFALLBACK)
        return NULL;
    cs = FcCharSetCreate();
    FcCharSetAddChar(cs, u);
    f = openface(FcPatternBuild(NULL, FC_FAMILY, FcTypeString, R.family, FC_PIXEL_SIZE, FcTypeDouble,
        (double)R.px, FC_CHARSET, FcTypeCharSet, cs, FC_COLOR, FcTypeBool, FcFalse, NULL), NULL);
    FcCharSetDestroy(cs);
    if (f && FT_Get_Char_Index(f, u))
        return R.fb[R.nfb++] = f;
    if (f)
        FT_Done_Face(f);
    return NULL;
}

/* The atlas starts over: every terminal has its cells to make again. */
static void
atlasreset(void)
{
    R.per = MIN(R.aw / cellw, NCOL);
    R.nslot = R.per * MIN(R.ah / cellh, NROW);
    R.next = 1;
    R.gen++;
    memset(R.ascii, 0, sizeof R.ascii);
    memset(R.hk, 0, sizeof R.hk);
    memset(R.bitmap, 0, cellw * cellh);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, R.atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, R.aw, R.ah, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cellw, cellh, GL_RED, GL_UNSIGNED_BYTE, R.bitmap);
}

/* The font to draw with from here on; with a context current once there is one. */
int
r_font(const char *family, int px)
{
    FT_Face f;
    int i, desc, h;

    if (!R.ft && (FT_Init_FreeType(&R.ft) || !FcInit()))
        return 0;
    if (R.face[0] && R.px == px && !strcmp(R.family, family))
        return 1;  /* which is what there is */
    for (i = 0; i < 4; i++)
        if (R.face[i])
            FT_Done_Face(R.face[i]), R.face[i] = NULL;
    while (R.nfb)
        FT_Done_Face(R.fb[--R.nfb]);
    g_free(R.family);
    R.family = g_strdup(family);
    R.px = px;
    if (!(f = face(0)))
        return 0;
    cellw = FT_Load_Char(f, 'M', FT_LOAD_TARGET_LIGHT) ? 0 : (f->glyph->advance.x + 32) >> 6;
    if (cellw < 1)
        cellw = MAX((f->size->metrics.max_advance + 32) >> 6, 1);
    R.asc = (f->size->metrics.ascender + 63) >> 6;
    desc = (-f->size->metrics.descender + 63) >> 6;
    h = (f->size->metrics.height + 32) >> 6;
    cellh = MAX(R.asc + desc, 1);
    if (h > cellh)
        R.asc += (h - cellh) / 2, cellh = h;
    R.bitmap = g_realloc(R.bitmap, 2 * cellw * cellh);
    if (R.ready)
        atlasreset();
    return 1;
}

static gint
namecmp(gconstpointer a, gconstpointer b)
{
    return g_strcasecmp(a, b);
}

/* The fixed width families there are, sorted. */
GList *
r_families(void)
{
    FcPattern *p = FcPatternBuild(NULL, FC_SPACING, FcTypeInteger, FC_MONO, NULL);
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, NULL);
    FcFontSet *fs = FcFontList(NULL, p, os);
    FcChar8 *name;
    GList *l = NULL;
    int i;

    for (i = 0; fs && i < fs->nfont; i++)
        if (FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &name) == FcResultMatch &&
            !g_list_find_custom(l, name, namecmp))
            l = g_list_insert_sorted(l, g_strdup((char *)name), namecmp);
    if (fs)
        FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(p);
    return l;
}

/* Glyphs. */

static void
rect(int w, int x0, int y0, int x1, int y1, int v)
{
    int y;

    x0 = CLAMP(x0, 0, w), x1 = CLAMP(x1, 0, w), y0 = CLAMP(y0, 0, cellh), y1 = CLAMP(y1, 0, cellh);
    for (y = y0; y < y1 && x1 > x0; y++)
        memset(R.bitmap + y * w + x0, v, x1 - x0);
}

/*
 * Lines and blocks are drawn here, not taken from the font, so that they
 * meet their neighbours whatever the font's own are like. Each line
 * drawing character is four arms, two bits each: left, right, up, down;
 * 1 is light and 2 heavy. 0 is one left to the font.
 */
static const uint8_t arms[128] = {
    0x05, 0x0a, 0x50, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x48, 0x84, 0x88,
    0x41, 0x42, 0x81, 0x82, 0x14, 0x18, 0x24, 0x28, 0x11, 0x12, 0x21, 0x22, 0x54, 0x58, 0x64, 0x94,
    0xa4, 0x68, 0x98, 0xa8, 0x51, 0x52, 0x61, 0x91, 0xa1, 0x62, 0x92, 0xa2, 0x45, 0x46, 0x49, 0x4a,
    0x85, 0x86, 0x89, 0x8a, 0x15, 0x16, 0x19, 0x1a, 0x25, 0x26, 0x29, 0x2a, 0x55, 0x56, 0x59, 0x5a,
    0x65, 0x95, 0xa5, 0x66, 0x69, 0x96, 0x99, 0x6a, 0x9a, 0xa6, 0xa9, 0xaa, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80, 0x09, 0x90, 0x06, 0x60,
};

static int
boxdraw(uint32_t u)
{
    static const uint8_t quad[] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
    int w = cellw, h = cellh, lt = MAX(1, (w + 4) / 8), t[3] = { 0, lt, lt * 2 }, a, l, r, up, dn, mh, mv, q;

    if (u >= 0x2500 && u < 0x2580) {
        if (!(a = arms[u - 0x2500]))
            return 0;
        l = t[a & 3], r = t[a >> 2 & 3], up = t[a >> 4 & 3], dn = t[a >> 6];
        mh = MAX(l, r), mv = MAX(up, dn);
        rect(w, 0, (h - l) / 2, l ? (w - mv) / 2 + mv : 0, (h - l) / 2 + l, 255);
        rect(w, (w - mv) / 2, (h - r) / 2, r ? w : 0, (h - r) / 2 + r, 255);
        rect(w, (w - up) / 2, 0, (w - up) / 2 + up, up ? (h - mh) / 2 + mh : 0, 255);
        rect(w, (w - dn) / 2, (h - mh) / 2, (w - dn) / 2 + dn, dn ? h : 0, 255);
    } else if (u == 0x2580) {
        rect(w, 0, 0, w, h / 2, 255);
    } else if (u <= 0x2588) {  /* eighths, from the bottom */
        rect(w, 0, h - (h * (u - 0x2580) + 4) / 8, w, h, 255);
    } else if (u <= 0x258f) {  /* and from the left */
        rect(w, 0, 0, (w * (0x2590 - u) + 4) / 8, h, 255);
    } else if (u == 0x2590) {
        rect(w, w / 2, 0, w, h, 255);
    } else if (u <= 0x2593) {  /* shades */
        rect(w, 0, 0, w, h, 64 * (u - 0x2590));
    } else if (u == 0x2594) {
        rect(w, 0, 0, w, MAX(h / 8, 1), 255);
    } else if (u == 0x2595) {
        rect(w, w - MAX(w / 8, 1), 0, w, h, 255);
    } else if (u <= 0x259f) {  /* quadrants */
        q = quad[u - 0x2596];
        for (a = 0; a < 4; a++)
            if (q >> a & 1)
                rect(w, a & 1 ? w / 2 : 0, a & 2 ? h / 2 : 0, a & 1 ? w : w / 2, a & 2 ? h : h / 2, 255);
    } else
        return 0;
    return 1;
}

/* A character drawn into the atlas; where it is there, column and row. */
static int
raster(uint32_t u, int style)
{
    int n = u >= 0x300 && wcwidth(u) == 2 ? 2 : 1, w = cellw * n, slot, x, y, x0, y0, synth;
    FT_Face f = face(style), fb;
    FT_Bitmap *bm;
    FT_UInt idx;
    uint8_t v;

    if (n == 2 && R.next % R.per == R.per - 1)  /* the halves sit side by side */
        R.next++;
    if (R.next + n > R.nslot)
        atlasreset();
    slot = R.next, R.next += n;
    memset(R.bitmap, 0, w * cellh);
    if (!(u >= 0x2500 && u < 0x25a0 && boxdraw(u))) {
        synth = R.face[style] ? R.synth[style] : style;
        if (!(idx = FT_Get_Char_Index(f, u)) && (fb = fallback(u)))
            f = fb, idx = FT_Get_Char_Index(f, u), synth = style;
        if (FT_Load_Glyph(f, idx, FT_LOAD_TARGET_LIGHT))
            return 0;
        if (synth & 1)
            FT_GlyphSlot_Embolden(f->glyph);
        if (synth & 2)
            FT_GlyphSlot_Oblique(f->glyph);
        if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LIGHT))
            return 0;
        bm = &f->glyph->bitmap;
        x0 = f->glyph->bitmap_left, y0 = R.asc - f->glyph->bitmap_top;
        for (y = MAX(-y0, 0); y < (int)bm->rows && y0 + y < cellh; y++)
            for (x = MAX(-x0, 0); x < (int)bm->width && x0 + x < w; x++) {
                if (bm->pixel_mode == FT_PIXEL_MODE_GRAY)
                    v = bm->buffer[y * bm->pitch + x];
                else if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
                    v = bm->buffer[y * bm->pitch + (x >> 3)] & (0x80 >> (x & 7)) ? 255 : 0;
                else
                    continue;
                R.bitmap[(y0 + y) * w + x0 + x] = v;
            }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, slot % R.per * cellw, slot / R.per * cellh, w, cellh,
                    GL_RED, GL_UNSIGNED_BYTE, R.bitmap);
    return slot % R.per | slot / R.per << 8;
}

static int
glyph(uint32_t u, int style)
{
    uint32_t k = u | style << 21, i;

    if (u < 128)
        return R.ascii[style][u] ? R.ascii[style][u] : (R.ascii[style][u] = raster(u, style));
    for (i = k * 2654435761u >> 17; R.hk[i] && R.hk[i] != k; i = (i + 1) & (NHASH - 1));
    if (R.hk[i])
        return R.hv[i];
    u = raster(u, style);  /* which may have emptied the table */
    for (i = k * 2654435761u >> 17; R.hk[i]; i = (i + 1) & (NHASH - 1));
    R.hk[i] = k;
    return R.hv[i] = u;
}

/* GL. */

static unsigned
shader(unsigned type, const char *head, const char *body)
{
    const char *src[2] = { head, body };
    unsigned s = glCreateShader(type);
    char log[512];
    int ok;

    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        g_warning("shader: %s", log);
    }
    return s;
}

int
r_init(void)
{
    const char *ver, *head;
    int ok, max = 0;

    if (R.ready)
        return 1;
#define X(r, n, a) if (!(gl##n = (r (*) a)gdk_gl_get_proc_address("gl" #n))) return 0;
    GL_FUNCS(X)
#undef X
    ver = (const char *)glGetString(GL_VERSION);
    R.version = g_strdup(ver ? ver : "?");
    head = ver && !strncmp(ver, "OpenGL ES", 9) ?
        "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 140\n";
    R.prog = glCreateProgram();
    glAttachShader(R.prog, shader(GL_VERTEX_SHADER, head, vertex));
    glAttachShader(R.prog, shader(GL_FRAGMENT_SHADER, head, fragment));
    glLinkProgram(R.prog);
    glGetProgramiv(R.prog, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    glUseProgram(R.prog);
    glUniform1i(glGetUniformLocation(R.prog, "cells"), 0);
    glUniform1i(glGetUniformLocation(R.prog, "atlas"), 1);
    R.ugeo = glGetUniformLocation(R.prog, "geo");
    R.ugrid = glGetUniformLocation(R.prog, "grid");
    R.ulines = glGetUniformLocation(R.prog, "lines");
    R.ucur = glGetUniformLocation(R.prog, "cur");
    R.uback = glGetUniformLocation(R.prog, "back");
    R.ucurcol = glGetUniformLocation(R.prog, "curcol");

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
    R.aw = R.ah = CLAMP(max, 256, 2048);
    glGenTextures(1, &R.atlas);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, R.atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    atlasreset();
    return R.ready = 1;
}

const char *
r_version(void)
{
    return R.version ? R.version : "";
}

/* A row of the terminal's, as the shader reads it. */
static void
buildrow(Grid *g, Term *t, int y)
{
    const Cell *l = vt_row(t, y);
    uint8_t *o = g->cells + (size_t)y * t->cols * 8;
    uint32_t dfg = t->pal[t->mode & M_REVERSE ? C_BG : C_FG], dbg = t->pal[t->mode & M_REVERSE ? C_FG : C_BG];
    uint32_t u, fg, bg, tmp;
    int x, x1 = 0, x2 = -1, at, prev = 0;

    vt_selrange(t, y, &x1, &x2);
    for (x = 0; x < t->cols; x++, o += 8) {
        u = l[x].u;
        fg = !l[x].fg ? dfg : l[x].fg & TRUECOL ? l[x].fg : t->pal[l[x].fg - 1];
        bg = !l[x].bg ? dbg : l[x].bg & TRUECOL ? l[x].bg : t->pal[l[x].bg - 1];
        if (u & A_FAINT)
            fg = (fg >> 1 & 0x7f7f7f) + (bg >> 1 & 0x7f7f7f);
        if (!(u & A_REV) != !(x >= x1 && x <= x2))
            tmp = fg, fg = bg, bg = tmp;
        if (u & A_INVIS)
            fg = bg;
        if (u & A_WDUMMY)  /* the half to the right of the one before */
            at = prev ? prev + 1 : 0;
        else
            at = RUNE(u) <= ' ' ? 0 : glyph(RUNE(u), (u >> 21 & 1) | (u >> 22 & 2));
        prev = u & A_WIDE ? at : 0;
        o[0] = fg >> 16, o[1] = fg >> 8, o[2] = fg, o[3] = at;
        o[4] = bg >> 16, o[5] = bg >> 8, o[6] = bg;
        o[7] = at >> 8 | (u & A_UNDER ? 64 : 0) | (u & A_STRUCK ? 128 : 0);
    }
}

void
r_draw(Grid *g, Term *t, int w, int h, int pad, int shape)
{
    int y, y0 = t->rows, y1 = -1, full = 0, th = MAX(1, (R.px + 7) / 14), cy = t->c.y + (t->mode & M_ALT ? 0 : t->scr);
    uint32_t c;

    if (!g->vao) {  /* these two are a context's own */
        glGenVertexArrays(1, &g->vao);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    glActiveTexture(GL_TEXTURE0 + 1);  /* where glyphs go as they are met */
    glBindTexture(GL_TEXTURE_2D, R.atlas);
    if (g->cols != t->cols || g->rows != t->rows) {
        g->cols = t->cols, g->rows = t->rows;
        g->cells = g_realloc(g->cells, (size_t)g->cols * g->rows * 8);
        g->gen = -1;
    }
    do {
        full = full || g->gen != R.gen || t->alldirty;
        g->gen = R.gen;
        for (y = 0; y < t->rows; y++)
            if (full || t->dirty[y]) {
                buildrow(g, t, y);
                y0 = MIN(y0, y), y1 = MAX(y1, y);
            }
    } while (g->gen != R.gen);  /* the atlas started over meanwhile */
    t->alldirty = 0;
    memset(t->dirty, 0, t->rows);

    glActiveTexture(GL_TEXTURE0);
    if (!g->tex) {
        glGenTextures(1, &g->tex);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        full = 1;
    }
    glBindTexture(GL_TEXTURE_2D, g->tex);
    if (full && y0 == 0 && y1 == t->rows - 1)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->cols * 2, g->rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, g->cells);
    else if (y1 >= y0)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, g->cols * 2, y1 - y0 + 1, GL_RGBA, GL_UNSIGNED_BYTE,
                        g->cells + (size_t)y0 * g->cols * 8);

    if ((t->mode & M_HIDE) || cy >= t->rows)
        shape = CUR_NONE;
    glViewport(0, 0, w, h);
    glUseProgram(R.prog);
    glUniform4i(R.ugeo, cellw, cellh, pad, h);
    glUniform4f(R.ugrid, t->cols, t->rows, 1.0f / cellw, 1.0f / cellh);
    glUniform4i(R.ulines, MIN(R.asc + th, cellh - th), th, R.asc - R.asc * 3 / 10 - th / 2, th);
    glUniform4i(R.ucur, shape == CUR_NONE ? -1 : t->c.x, cy, shape, MAX(th, cellw / 8));
    c = t->pal[t->mode & M_REVERSE ? C_FG : C_BG];
    glUniform3f(R.uback, (c >> 16 & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c & 255) / 255.0f);
    c = t->pal[C_CURSOR];
    glUniform3f(R.ucurcol, (c >> 16 & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c & 255) / 255.0f);
    glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* With the terminal's context current. */
void
r_free(Grid *g)
{
    if (g->tex)
        glDeleteTextures(1, &g->tex);
    if (g->vao)
        glDeleteVertexArrays(1, &g->vao);
    g_free(g->cells);
    memset(g, 0, sizeof *g);
}
