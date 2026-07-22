/* gpuload.c — deterministic GPU load benchmark for the Waydroid Venus stack.
 * Renders offscreen (EGL pbuffer, GLES2) so it needs no Activity/APK and no
 * display; the load still flows through ANGLE -> Venus -> vtest -> host GPU,
 * which is what varies between GPU tiers.  Per-frame glFinish bounds each
 * frame honestly (no unbounded pipelining), fps scales with GPU power.
 *
 * Scenes:
 *   alu       - fragment ALU: iterated sin/dot loop, fullscreen quad
 *   texture   - bandwidth: 8 dependent fetches from a 1024^2 texture
 *   overdraw  - 24 blended fullscreen layers per frame
 *   geometry  - 512x512 vertex grid, per-vertex wave transform
 *
 * Output (parse-friendly):
 *   GPULOAD <scene> fps <float> frame_ms <float>
 *   GPULOAD score <geometric mean fps>
 *
 * build: x86_64-linux-android34-clang -O2 -o gpuload-android gpuload.c -lEGL -lGLESv2 -lm
 * usage: gpuload-android [width height seconds_per_scene]   (default 2560 1440 4)
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint compile(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader compile failed: %s\n", log);
      exit(1);
   }
   return s;
}

static GLuint program(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
   glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   GLint ok = 0;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { fprintf(stderr, "link failed\n"); exit(1); }
   return p;
}

static const char *quad_vs =
   "attribute vec2 pos;\n"
   "varying vec2 uv;\n"
   "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *alu_fs =
   "precision highp float;\n"
   "varying vec2 uv;\n"
   "uniform float t;\n"
   "void main() {\n"
   "  vec3 c = vec3(uv, t);\n"
   "  for (int i = 0; i < 96; i++) {\n"
   "    c = vec3(dot(c, vec3(0.299, 0.587, 0.114)),\n"
   "             sin(c.x * 12.9898 + t) * 0.5 + 0.5,\n"
   "             fract(c.y * 43758.5453));\n"
   "  }\n"
   "  gl_FragColor = vec4(c, 1.0);\n"
   "}\n";

static const char *tex_fs =
   "precision highp float;\n"
   "varying vec2 uv;\n"
   "uniform float t;\n"
   "uniform sampler2D tex;\n"
   "void main() {\n"
   "  vec4 c = texture2D(tex, uv);\n"
   "  for (int i = 0; i < 8; i++)\n"
   "    c = texture2D(tex, fract(c.xy + uv * 0.37 + t * 0.01));\n"
   "  gl_FragColor = c;\n"
   "}\n";

static const char *over_fs =
   "precision mediump float;\n"
   "varying vec2 uv;\n"
   "uniform float t;\n"
   "void main() {\n"
   "  gl_FragColor = vec4(uv.x, uv.y, fract(t), 0.25);\n"
   "}\n";

static const char *grid_vs =
   "attribute vec2 pos;\n"
   "varying vec2 uv;\n"
   "uniform float t;\n"
   "void main() {\n"
   "  uv = pos * 0.5 + 0.5;\n"
   "  float w = sin(pos.x * 37.0 + t * 3.0) * cos(pos.y * 29.0 + t * 2.0);\n"
   "  w += sin(dot(pos, pos) * 19.0 - t * 4.0);\n"
   "  gl_Position = vec4(pos * (0.95 + 0.05 * w), w * 0.1, 1.0);\n"
   "}\n";

static const char *grid_fs =
   "precision mediump float;\n"
   "varying vec2 uv;\n"
   "void main() { gl_FragColor = vec4(uv, 0.5, 1.0); }\n";

/* run one scene: warmup, then timed frames with per-frame glFinish */
static double run_scene(const char *name, GLuint prog, GLint t_loc,
                        double seconds, void (*draw)(void))
{
   glUseProgram(prog);
   for (int i = 0; i < 5; i++) {   /* warmup */
      if (t_loc >= 0) glUniform1f(t_loc, i * 0.05f);
      draw();
      glFinish();
   }
   int frames = 0;
   const double t0 = now_s();
   double t1 = t0;
   while (t1 - t0 < seconds) {
      if (t_loc >= 0) glUniform1f(t_loc, (float)(t1 - t0));
      draw();
      glFinish();
      frames++;
      t1 = now_s();
   }
   const double fps = frames / (t1 - t0);
   printf("GPULOAD %s fps %.1f frame_ms %.3f\n", name, fps, 1000.0 / fps);
   fflush(stdout);
   return fps;
}

static void draw_quad(void)
{
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_overdraw(void)
{
   glClear(GL_COLOR_BUFFER_BIT);
   for (int i = 0; i < 24; i++)
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static GLsizei grid_indices;
static void draw_grid(void)
{
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glDrawElements(GL_TRIANGLES, grid_indices, GL_UNSIGNED_SHORT, 0);
}

int main(int argc, char **argv)
{
   const int W = argc > 1 ? atoi(argv[1]) : 2560;
   const int H = argc > 2 ? atoi(argv[2]) : 1440;
   const double secs = argc > 3 ? atof(argv[3]) : 4.0;

   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
#ifndef __ANDROID__
      /* headless host: EGL device platform (NVIDIA/Mesa), pick arg-selected
       * or first device that initializes
       */
      typedef EGLBoolean (*QueryDevs)(EGLint, void **, EGLint *);
      typedef EGLDisplay (*GetPlatDpy)(EGLenum, void *, const EGLint *);
      QueryDevs query_devs =
         (QueryDevs)eglGetProcAddress("eglQueryDevicesEXT");
      GetPlatDpy get_plat =
         (GetPlatDpy)eglGetProcAddress("eglGetPlatformDisplayEXT");
      void *devs[16];
      EGLint ndev = 0;
      dpy = EGL_NO_DISPLAY;
      if (query_devs && get_plat && query_devs(16, devs, &ndev)) {
         for (EGLint i = 0; i < ndev; i++) {
            EGLDisplay d = get_plat(0x313F /* EGL_PLATFORM_DEVICE_EXT */,
                                    devs[i], NULL);
            if (d != EGL_NO_DISPLAY && eglInitialize(d, &maj, &min)) {
               dpy = d;
               break;
            }
         }
      }
      if (dpy == EGL_NO_DISPLAY)
#endif
      {
         fprintf(stderr, "EGL init failed\n");
         return 1;
      }
   }
   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 16,
      EGL_NONE
   };
   EGLConfig cfg; EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no EGL config\n"); return 1;
   }
   static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   const EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
   if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE ||
       !eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "EGL context/surface failed (0x%x)\n", eglGetError());
      return 1;
   }
   printf("GPULOAD renderer %s\n", (const char *)glGetString(GL_RENDERER));
   printf("GPULOAD resolution %dx%d, %.1fs/scene\n", W, H, secs);
   fflush(stdout);
   glViewport(0, 0, W, H);

   /* fullscreen quad */
   static const GLfloat quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
   GLuint qbo;
   glGenBuffers(1, &qbo);
   glBindBuffer(GL_ARRAY_BUFFER, qbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

   /* checkerboard texture for the bandwidth scene */
   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   {
      const int TS = 1024;
      unsigned char *px = malloc((size_t)TS * TS * 4);
      for (int y = 0; y < TS; y++)
         for (int x = 0; x < TS; x++) {
            unsigned char *p = px + 4 * (y * TS + x);
            p[0] = (unsigned char)(x * 255 / TS);
            p[1] = (unsigned char)(y * 255 / TS);
            p[2] = (unsigned char)((x ^ y) & 0xff);
            p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TS, TS, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, px);
      free(px);
   }
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

   double score = 1.0;
   int scenes = 0;

   GLuint p_alu = program(quad_vs, alu_fs);
   score *= run_scene("alu", p_alu, glGetUniformLocation(p_alu, "t"), secs,
                      draw_quad);
   scenes++;

   GLuint p_tex = program(quad_vs, tex_fs);
   glUseProgram(p_tex);
   glUniform1i(glGetUniformLocation(p_tex, "tex"), 0);
   score *= run_scene("texture", p_tex, glGetUniformLocation(p_tex, "t"), secs,
                      draw_quad);
   scenes++;

   GLuint p_over = program(quad_vs, over_fs);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   score *= run_scene("overdraw", p_over, glGetUniformLocation(p_over, "t"),
                      secs, draw_overdraw);
   glDisable(GL_BLEND);
   scenes++;

   /* geometry: 512x512 grid = 522k triangles/frame */
   {
      const int G = 512;
      GLfloat *verts = malloc(sizeof(GLfloat) * 2 * (G + 1) * (G + 1));
      for (int y = 0; y <= G; y++)
         for (int x = 0; x <= G; x++) {
            verts[2 * (y * (G + 1) + x) + 0] = x * 2.0f / G - 1.0f;
            verts[2 * (y * (G + 1) + x) + 1] = y * 2.0f / G - 1.0f;
         }
      /* index in 128x512 strips to stay under 65536 vertices per strip zone;
       * simpler: one index buffer of GL_UNSIGNED_SHORT won't fit (G+1)^2 =
       * 263169 verts, so use a 255x255 grid chunk drawn 4 times instead
       */
      const int C = 255;
      GLushort *idx = malloc(sizeof(GLushort) * 6 * C * C);
      GLsizei ni = 0;
      for (int y = 0; y < C; y++)
         for (int x = 0; x < C; x++) {
            const GLushort a = (GLushort)(y * (C + 1) + x);
            const GLushort b = a + 1;
            const GLushort c = (GLushort)(a + C + 1);
            const GLushort d = c + 1;
            idx[ni++] = a; idx[ni++] = b; idx[ni++] = c;
            idx[ni++] = b; idx[ni++] = d; idx[ni++] = c;
         }
      GLfloat *cverts = malloc(sizeof(GLfloat) * 2 * (C + 1) * (C + 1));
      for (int y = 0; y <= C; y++)
         for (int x = 0; x <= C; x++) {
            cverts[2 * (y * (C + 1) + x) + 0] = x * 2.0f / C - 1.0f;
            cverts[2 * (y * (C + 1) + x) + 1] = y * 2.0f / C - 1.0f;
         }
      GLuint vbo, ibo;
      glGenBuffers(1, &vbo);
      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glBufferData(GL_ARRAY_BUFFER,
                   sizeof(GLfloat) * 2 * (C + 1) * (C + 1), cverts,
                   GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
      glGenBuffers(1, &ibo);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * ni, idx,
                   GL_STATIC_DRAW);
      grid_indices = ni;
      free(verts); free(idx); free(cverts);
      glEnable(GL_DEPTH_TEST);

      GLuint p_grid = program(grid_vs, grid_fs);
      score *= run_scene("geometry", p_grid,
                         glGetUniformLocation(p_grid, "t"), secs, draw_grid);
      scenes++;
   }

   printf("GPULOAD score %.1f\n", pow(score, 1.0 / scenes));
   return 0;
}
