// Cross-platform cursor-following label, shown while the user drags a
// scene tab outside the main OCS window so the gesture has a clear
// visual anchor (the .csd filename travels with the cursor).
//
// Implemented as a secondary borderless GLFW window with its own
// OpenGL 2.1 context — works on macOS / Windows / Linux without any
// platform-specific code, relying only on GLFW which axmol already
// depends on.

#pragma once

#include <string>

namespace ocs::draglabel
{

/// Show or refresh the label at `screenX, screenY` (top-left origin,
/// pixels). `text` is rendered inside via stb_easy_font. First call
/// lazily creates the underlying GLFW window.
void show(const std::string& text, float screenX, float screenY);

/// Move without re-rendering text. Cheap — just glfwSetWindowPos.
void moveTo(float screenX, float screenY);

/// Hide until the next show() call. The window stays alive to avoid
/// recreation churn during rapid drag starts.
void hide();

/// Destroy the window. Safe to call at shutdown.
void shutdown();

}  // namespace ocs::draglabel
