#include "DragLabel.h"

// stb_easy_font is a single-header public-domain ASCII bitmap font
// that emits GL_QUADS vertex data — perfect for a tiny floating
// label where pulling in a real text engine would be overkill.
#include "stb_easy_font.h"

#include "GLFW/glfw3.h"

#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  include <OpenGL/gl.h>
#  pragma clang diagnostic pop
#else
#  include <GL/gl.h>
#endif

#include <cstring>

namespace ocs::draglabel
{
namespace
{

// Window footprint. stb_easy_font renders at 8 px/char, so 26 chars
// fit inside 220 px with room for padding. The filename is trimmed
// on display if it's longer.
constexpr int kWinW = 240;
constexpr int kWinH = 28;

GLFWwindow* gWindow = nullptr;
std::string gText;

/// Paint one frame into the label's back buffer and flip. Saves and
/// restores the thread's current GL context so axmol's renderer (if
/// it has one — Metal on macOS, GL elsewhere) is untouched.
void paint()
{
    if (!gWindow) return;
    GLFWwindow* prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(gWindow);

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(gWindow, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);

    // Background — dark, slight transparency via the transparent
    // framebuffer hint so OS compositing shows through at the edges.
    glClearColor(0.13f, 0.13f, 0.15f, 0.92f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Work in window-pixel units so the text stays a constant size
    // regardless of HiDPI framebuffer scale.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, kWinW, kWinH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Blue accent border — same color we use for selected Mode
    // buttons so the label clearly belongs to the OCS app.
    glLineWidth(2.0f);
    glColor4f(0.30f, 0.55f, 0.90f, 1.0f);
    glBegin(GL_LINE_LOOP);
        glVertex2f(1.0f,            1.0f);
        glVertex2f((float)kWinW - 1, 1.0f);
        glVertex2f((float)kWinW - 1, (float)kWinH - 1);
        glVertex2f(1.0f,            (float)kWinH - 1);
    glEnd();

    // Filename text. stb_easy_font emits quads in interleaved format
    // (x,y,z:float + color:uint8[4] = 16 bytes). We use 2D
    // glVertexPointer with stride 16 to skip the z/color fields.
    if (!gText.empty())
    {
        static char vbuf[100000];
        const int quads = stb_easy_font_print(
            10.0f, 10.0f,
            const_cast<char*>(gText.c_str()),
            nullptr,
            vbuf, (int)sizeof(vbuf));
        if (quads > 0)
        {
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(2, GL_FLOAT, 16, vbuf);
            glDrawArrays(GL_QUADS, 0, quads * 4);
            glDisableClientState(GL_VERTEX_ARRAY);
        }
    }

    glfwSwapBuffers(gWindow);

    if (prev != gWindow) glfwMakeContextCurrent(prev);
}

/// Create the native window once, with hints that make it behave
/// like a drag tooltip: borderless, floating above all others,
/// non-focusable, mouse-passthrough so it never intercepts input.
bool ensureInit()
{
    if (gWindow) return true;

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_DECORATED,               GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE,               GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,                GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW,           GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED,                 GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE,                 GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#ifdef GLFW_MOUSE_PASSTHROUGH
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH,       GLFW_TRUE);
#endif
    // Legacy GL 2.1 context → fixed-function + client vertex arrays
    // work on macOS (compat), Windows, and Linux with zero shader
    // boilerplate. stb_easy_font expects this pipeline.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

    gWindow = glfwCreateWindow(kWinW, kWinH, "ocs-drag-label",
                               nullptr, nullptr);
    // Reset hints so the next glfwCreateWindow (if any) starts clean.
    glfwDefaultWindowHints();
    if (!gWindow) return false;
    // Disable vsync on the label's context — otherwise each paint()
    // blocks up to 16 ms on the refresh, starving axmol's main loop
    // and losing the mouse-release event that should trigger tear-off.
    GLFWwindow* prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(gWindow);
    glfwSwapInterval(0);
    if (prev != gWindow) glfwMakeContextCurrent(prev);
    return true;
}

}  // anon

void show(const std::string& text, float screenX, float screenY)
{
    if (!ensureInit()) return;
    gText = text;
    glfwSetWindowPos(gWindow, (int)screenX, (int)screenY);
    // Paint BEFORE showing so the first frame the user sees already
    // contains the text — avoids a single-frame flash of uninit
    // framebuffer garbage.
    paint();
    glfwShowWindow(gWindow);
}

void moveTo(float screenX, float screenY)
{
    if (!gWindow) return;
    glfwSetWindowPos(gWindow, (int)screenX, (int)screenY);
    // Some compositors mark the window contents dirty on a move —
    // cheap to re-emit the frame.
    paint();
}

void hide()
{
    if (!gWindow) return;
    glfwHideWindow(gWindow);
}

void shutdown()
{
    if (gWindow)
    {
        glfwDestroyWindow(gWindow);
        gWindow = nullptr;
    }
}

}  // namespace ocs::draglabel
