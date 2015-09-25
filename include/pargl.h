#pragma once

#include "par.h"

#if defined(__APPLE_CC__)
  #if defined(GLFW_INCLUDE_GLCOREARB)
    #include <OpenGL/gl3.h>
  #elif !defined(GLFW_INCLUDE_NONE)
    #define GL_GLEXT_LEGACY
    #include <OpenGL/gl.h>
  #endif
#else
  #if defined(GLFW_INCLUDE_GLCOREARB)
    #include <GL/glcorearb.h>
  #elif defined(GLFW_INCLUDE_ES1)
    #include <GLES/gl.h>
  #elif defined(GLFW_INCLUDE_ES2)
    #include <GLES2/gl2.h>
  #elif defined(GLFW_INCLUDE_ES3)
    #include <GLES3/gl3.h>
  #elif !defined(GLFW_INCLUDE_NONE)
    #include <GL/gl.h>
  #endif
#endif

GLint pargl_create_buffer_u8(par_buffer_u8* );
GLint pargl_get_uniform(par_token);
void pargl_bind_shader(par_token);
