/* gl_compat.h -- GLES1 extension compatibility for libsotn.so. We include only
 * <GLES2/gl2.h> (avoids the GLES/gl.h vs gl2.h header clash) and hand-declare
 * the GLES1-only entry points/enums. MIT license; see LICENSE. */

#ifndef __GL_COMPAT_H__
#define __GL_COMPAT_H__

#include <GLES2/gl2.h>

// GLES1-only enums (not in gl2.h)
#ifndef GL_MODELVIEW
#define GL_MODELVIEW            0x1700
#define GL_PROJECTION           0x1701
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY         0x8074
#define GL_NORMAL_ARRAY         0x8075
#define GL_COLOR_ARRAY          0x8076
#define GL_TEXTURE_COORD_ARRAY  0x8078
#endif
#ifndef GL_TEXTURE_CROP_RECT_OES
#define GL_TEXTURE_CROP_RECT_OES 0x8B9D
#endif

// GLES1 fixed-function entry points (resolve to mesa libGLESv1_CM at link time)
extern void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
extern void glMatrixMode(GLenum mode);
extern void glLoadIdentity(void);
extern void glPushMatrix(void);
extern void glPopMatrix(void);
extern void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
extern void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z);
extern void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
extern void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
extern void glEnableClientState(GLenum cap);
extern void glDisableClientState(GLenum cap);
extern void glTexEnvf(GLenum target, GLenum pname, GLfloat param);

// interception points (registered in the import table)
void glBindTexture_compat(GLenum target, GLuint texture);
void glActiveTexture_compat(GLenum texture);
void glTexImage2D_compat(GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *pixels);
void glTexParameteriv_compat(GLenum target, GLenum pname, const GLint *params);
void glDeleteTextures_compat(GLsizei n, const GLuint *textures);

// GL_OES_draw_texture
void glDrawTexfOES_compat(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h);

// (the GL_OES_framebuffer_object *OES entry points are ABI-identical to the
// core GLES2 FBO functions and are mapped straight to them in imports.c)

#endif
