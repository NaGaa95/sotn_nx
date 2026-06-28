/* gl_compat.c -- GLES1 extension compatibility for libsotn.so. MIT; see LICENSE. */

#include <string.h>
#include "gl_compat.h"

// ---------------------------------------------------------------------------
// per-texture tracking: dimensions (for normalising crop rects) and the
// GL_TEXTURE_CROP_RECT_OES used by glDrawTexfOES. Small open-addressed table.
// ---------------------------------------------------------------------------

#define TEX_SLOTS 2048

typedef struct {
  GLuint id;       // 0 == empty slot
  GLsizei w, h;
  GLint crop[4];   // u, v, w, h (signed; negative h flips vertically)
} TexInfo;

static TexInfo g_tex[TEX_SLOTS];

#define MAX_TEX_UNITS 8
static GLuint g_bound_tex[MAX_TEX_UNITS]; // bound GL_TEXTURE_2D per unit
static int g_active_unit = 0;

static TexInfo *tex_find(GLuint id) {
  if (id == 0) return NULL;
  unsigned h = (id * 2654435761u) % TEX_SLOTS;
  for (unsigned i = 0; i < TEX_SLOTS; i++) {
    unsigned k = (h + i) % TEX_SLOTS;
    if (g_tex[k].id == id) return &g_tex[k];
    if (g_tex[k].id == 0) return NULL;
  }
  return NULL;
}

static TexInfo *tex_intern(GLuint id) {
  if (id == 0) return NULL;
  unsigned h = (id * 2654435761u) % TEX_SLOTS;
  for (unsigned i = 0; i < TEX_SLOTS; i++) {
    unsigned k = (h + i) % TEX_SLOTS;
    if (g_tex[k].id == id) return &g_tex[k];
    if (g_tex[k].id == 0) {
      memset(&g_tex[k], 0, sizeof(g_tex[k]));
      g_tex[k].id = id;
      return &g_tex[k];
    }
  }
  return NULL;
}

static GLuint cur_tex(void) {
  return g_bound_tex[g_active_unit < MAX_TEX_UNITS ? g_active_unit : 0];
}

void glActiveTexture_compat(GLenum texture) {
  g_active_unit = (int)(texture - GL_TEXTURE0);
  if (g_active_unit < 0 || g_active_unit >= MAX_TEX_UNITS) g_active_unit = 0;
  glActiveTexture(texture);
}

void glBindTexture_compat(GLenum target, GLuint texture) {
  if (target == GL_TEXTURE_2D && g_active_unit < MAX_TEX_UNITS)
    g_bound_tex[g_active_unit] = texture;
  glBindTexture(target, texture);
}

void glTexImage2D_compat(GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *pixels) {
  if (target == GL_TEXTURE_2D && level == 0) {
    TexInfo *t = tex_intern(cur_tex());
    if (t) { t->w = width; t->h = height; }
  }
  glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void glTexParameteriv_compat(GLenum target, GLenum pname, const GLint *params) {
  if (pname == GL_TEXTURE_CROP_RECT_OES) {
    // store the crop rect for glDrawTexfOES; do NOT forward (mesa may reject it)
    TexInfo *t = tex_intern(cur_tex());
    if (t && params) memcpy(t->crop, params, sizeof(t->crop));
    return;
  }
  glTexParameteriv(target, pname, params);
}

void glDeleteTextures_compat(GLsizei n, const GLuint *textures) {
  for (GLsizei i = 0; i < n; i++) {
    TexInfo *t = tex_find(textures[i]);
    if (t) t->id = 0; // free slot (note: leaves a tombstone, fine for our use)
    for (int u = 0; u < MAX_TEX_UNITS; u++)
      if (g_bound_tex[u] == textures[i]) g_bound_tex[u] = 0;
  }
  glDeleteTextures(n, textures);
}

// ---------------------------------------------------------------------------
// glDrawTexfOES: draw a screen-aligned textured quad of the bound texture's
// crop rectangle at window position (x,y) size (w,h). Implemented over the
// fixed-function pipeline; surrounding GL state is saved and restored.
// ---------------------------------------------------------------------------

void glDrawTexfOES_compat(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h) {
  TexInfo *t = tex_find(cur_tex());
  GLfloat tw = (t && t->w) ? (GLfloat)t->w : 1.0f;
  GLfloat th = (t && t->h) ? (GLfloat)t->h : 1.0f;
  GLfloat cx = t ? (GLfloat)t->crop[0] : 0.0f;
  GLfloat cy = t ? (GLfloat)t->crop[1] : 0.0f;
  GLfloat cw = t ? (GLfloat)t->crop[2] : tw;
  GLfloat ch = t ? (GLfloat)t->crop[3] : th;

  const GLfloat u0 = cx / tw;
  const GLfloat v0 = cy / th;
  const GLfloat u1 = (cx + cw) / tw;
  const GLfloat v1 = (cy + ch) / th;

  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);

  // map z (window depth) like the OES spec: clamp to [0,1]
  GLfloat zc = z;
  if (zc < 0.0f) zc = 0.0f;
  if (zc > 1.0f) zc = 1.0f;
  const GLfloat zndc = zc * 2.0f - 1.0f;

  const GLfloat verts[12] = {
    x,     y,     zndc,
    x + w, y,     zndc,
    x + w, y + h, zndc,
    x,     y + h, zndc,
  };
  const GLfloat texc[8] = { u0, v0,  u1, v0,  u1, v1,  u0, v1 };

  // --- save state we touch -------------------------------------------------
  const GLboolean had_vtx = glIsEnabled(GL_VERTEX_ARRAY);
  const GLboolean had_tex = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  const GLboolean had_col = glIsEnabled(GL_COLOR_ARRAY);
  const GLboolean had_nrm = glIsEnabled(GL_NORMAL_ARRAY);

  // client-side arrays are interpreted as VBO offsets if a buffer is bound;
  // unbind so our stack pointers are read directly, then restore.
  GLint prev_vbo = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
  if (prev_vbo) glBindBuffer(GL_ARRAY_BUFFER, 0);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof((GLfloat)vp[0], (GLfloat)(vp[0] + vp[2]),
           (GLfloat)vp[1], (GLfloat)(vp[1] + vp[3]), -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  if (had_col) glDisableClientState(GL_COLOR_ARRAY);
  if (had_nrm) glDisableClientState(GL_NORMAL_ARRAY);
  if (!had_vtx) glEnableClientState(GL_VERTEX_ARRAY);
  if (!had_tex) glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  glVertexPointer(3, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, texc);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

  // --- restore -------------------------------------------------------------
  if (!had_tex) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  if (!had_vtx) glDisableClientState(GL_VERTEX_ARRAY);
  if (had_col) glEnableClientState(GL_COLOR_ARRAY);
  if (had_nrm) glEnableClientState(GL_NORMAL_ARRAY);
  if (prev_vbo) glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
}
