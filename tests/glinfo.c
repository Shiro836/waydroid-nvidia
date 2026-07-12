/* glinfo.c — minimal EGL pbuffer context + glGetString dump, to check what
 * the system GLES (ANGLE -> Venus) reports.  Repro probe for glmark2's
 * strlen(NULL) crash in print_info().
 *
 * build: x86_64-linux-android34-clang -O1 -o glinfo-android glinfo.c -lEGL -lGLESv2
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>

#define S(x) ((x) ? (const char *)(x) : "(NULL)")

int main(void)
{
   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY) { printf("no display\n"); return 1; }
   EGLint maj, min;
   if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed\n"); return 1; }
   printf("EGL %d.%d\n", maj, min);
   printf("EGL_VENDOR:  %s\n", S(eglQueryString(dpy, EGL_VENDOR)));
   printf("EGL_VERSION: %s\n", S(eglQueryString(dpy, EGL_VERSION)));

   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
      EGL_DEPTH_SIZE, 16,
      EGL_NONE
   };
   EGLConfig cfg;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || !n) {
      printf("no config (n=%d)\n", n); return 1;
   }
   static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (ctx == EGL_NO_CONTEXT) { printf("no context, egl error 0x%x\n", eglGetError()); return 1; }
   static const EGLint pb_attrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
   if (surf == EGL_NO_SURFACE) { printf("no surface, egl error 0x%x\n", eglGetError()); return 1; }
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      printf("makeCurrent failed, egl error 0x%x\n", eglGetError()); return 1;
   }

   printf("GL_VENDOR:   %s\n", S(glGetString(GL_VENDOR)));
   printf("GL_RENDERER: %s\n", S(glGetString(GL_RENDERER)));
   printf("GL_VERSION:  %s\n", S(glGetString(GL_VERSION)));
   printf("GL_SL:       %s\n", S(glGetString(GL_SHADING_LANGUAGE_VERSION)));
   const char *ext = (const char *)glGetString(GL_EXTENSIONS);
   printf("GL_EXTENSIONS: %s\n", ext ? "present" : "(NULL)");
   return 0;
}
