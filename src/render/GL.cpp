#include "render/GL.h"
#include <SDL.h>
#include <cstring>

#define CA_GL_DEFINE(ret, name, args) PFN_##name ca_##name = nullptr;
CA_GL_FUNCS(CA_GL_DEFINE)
#undef CA_GL_DEFINE

namespace ca {

bool loadGL(const char** missing) {
#define CA_GL_LOAD(ret, name, args)                                         \
    ca_##name = reinterpret_cast<PFN_##name>(SDL_GL_GetProcAddress(#name)); \
    if (!ca_##name) { if (missing) *missing = #name; return false; }
    CA_GL_FUNCS(CA_GL_LOAD)
#undef CA_GL_LOAD
    return true;
}

bool hasGLExtension(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (e && std::strcmp(e, name) == 0) return true;
    }
    return false;
}

} // namespace ca
