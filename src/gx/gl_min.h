#ifndef NFLS2_GX_GL_MIN_H
#define NFLS2_GX_GL_MIN_H

/* The small part of OpenGL 3.3 (core profile) the GPU renderer uses, loaded
   at run time through the window system's GetProcAddress, so no OpenGL
   headers or import libraries are needed to build. */
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#elif !defined(APIENTRY)
#define APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef uint64_t GLuint64;
typedef struct __GLsync* GLsync;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_CLAMP 0x864F
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_DITHER 0x0BD0
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#define GL_TRIANGLE_STRIP 0x0005

/* name, return type, arguments */
#define GLR_FUNCS(X) \
    X(glGetError, GLenum, (void)) \
    X(glGetString, const GLubyte*, (GLenum)) \
    X(glGetIntegerv, void, (GLenum, GLint*)) \
    X(glEnable, void, (GLenum)) \
    X(glDisable, void, (GLenum)) \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glScissor, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glDepthFunc, void, (GLenum)) \
    X(glDepthMask, void, (GLboolean)) \
    X(glColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum)) \
    X(glBlendEquationSeparate, void, (GLenum, GLenum)) \
    X(glCullFace, void, (GLenum)) \
    X(glFrontFace, void, (GLenum)) \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glClearDepth, void, (GLdouble)) \
    X(glClear, void, (GLbitfield)) \
    X(glFinish, void, (void)) \
    X(glFlush, void, (void)) \
    X(glPixelStorei, void, (GLenum, GLint)) \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(glGenTextures, void, (GLsizei, GLuint*)) \
    X(glDeleteTextures, void, (GLsizei, const GLuint*)) \
    X(glBindTexture, void, (GLenum, GLuint)) \
    X(glActiveTexture, void, (GLenum)) \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(glTexParameteri, void, (GLenum, GLenum, GLint)) \
    X(glGenSamplers, void, (GLsizei, GLuint*)) \
    X(glBindSampler, void, (GLuint, GLuint)) \
    X(glSamplerParameteri, void, (GLuint, GLenum, GLint)) \
    X(glSamplerParameterf, void, (GLuint, GLenum, GLfloat)) \
    X(glGenFramebuffers, void, (GLsizei, GLuint*)) \
    X(glDeleteFramebuffers, void, (GLsizei, const GLuint*)) \
    X(glBindFramebuffer, void, (GLenum, GLuint)) \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(glCheckFramebufferStatus, GLenum, (GLenum)) \
    X(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(glGenBuffers, void, (GLsizei, GLuint*)) \
    X(glBindBuffer, void, (GLenum, GLuint)) \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(glBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void*)) \
    X(glBindBufferRange, void, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
    X(glGenVertexArrays, void, (GLsizei, GLuint*)) \
    X(glBindVertexArray, void, (GLuint)) \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(glEnableVertexAttribArray, void, (GLuint)) \
    X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void*)) \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei)) \
    X(glCreateShader, GLuint, (GLenum)) \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(glCompileShader, void, (GLuint)) \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint*)) \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(glDeleteShader, void, (GLuint)) \
    X(glCreateProgram, GLuint, (void)) \
    X(glAttachShader, void, (GLuint, GLuint)) \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar*)) \
    X(glBindFragDataLocation, void, (GLuint, GLuint, const GLchar*)) \
    X(glLinkProgram, void, (GLuint)) \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint*)) \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(glUseProgram, void, (GLuint)) \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar*)) \
    X(glUniform1i, void, (GLint, GLint)) \
    X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glGetUniformBlockIndex, GLuint, (GLuint, const GLchar*)) \
    X(glUniformBlockBinding, void, (GLuint, GLuint, GLuint)) \
    X(glUniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(glFenceSync, GLsync, (GLenum, GLbitfield)) \
    X(glWaitSync, void, (GLsync, GLbitfield, GLuint64)) \
    X(glDeleteSync, void, (GLsync))

#define GLR_DECLARE(name, ret, args) typedef ret(APIENTRY* PFN_##name) args; extern PFN_##name glr_##name;
GLR_FUNCS(GLR_DECLARE)
#undef GLR_DECLARE

/* Calls go through the loaded pointers (glr_glClear etc.). */
#ifndef GLR_NO_MACROS
#define GLR_CALL(name) glr_##name
#endif

#endif
