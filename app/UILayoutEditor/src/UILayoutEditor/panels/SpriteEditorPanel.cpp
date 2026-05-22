// Sprite Editor panel: NLP-driven mutation surface for the currently
// selected node. The user types a natural-language prompt ("paint it
// terracotta", "rotate 30 degrees", "hide", "scale 1.5") and the
// editor dispatches it through either the host-installed
// SpriteNlpHandler (LLM-backed) or a tiny local pattern matcher that
// covers the most-used verbs by translating to existing RPC commands.

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/PlistAtlas.h"

#include "imgui/imgui.h"

#include "axmol.h"
#include "ImGui/ImGuiPresenter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace opencs
{

namespace
{

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool matchColor(const std::string& tok, int& R, int& G, int& B)
{
    struct C { const char* name; int r, g, b; };
    static const C palette[] = {
        {"red",        220,  60,  60},
        {"green",       80, 200,  90},
        {"blue",        80, 130, 220},
        {"yellow",     245, 200,  60},
        {"orange",     240, 150,  70},
        {"purple",     170, 100, 200},
        {"pink",       240, 150, 200},
        {"white",      245, 245, 245},
        {"black",       20,  20,  20},
        {"gray",       128, 128, 128},
        {"grey",       128, 128, 128},
        {"cyan",       110, 235, 255},
        {"magenta",    240,  90, 200},
        {"terracotta", 197, 123,  87},
        {"soil",       107,  68,  35},
    };
    for (const auto& c : palette)
        if (tok == c.name) { R = c.r; G = c.g; B = c.b; return true; }
    return false;
}

std::string localDispatch(const std::string& prompt, Editor& ed)
{
    if (!ed.doc()) return "no document loaded";
    if (!ed.selected().valid()) return "no selection";
    const auto sel = ed.selected();
    const std::string lp = lower(prompt);

    std::vector<std::string> toks;
    {
        std::istringstream ss(lp);
        std::string t;
        while (ss >> t) toks.push_back(std::move(t));
    }

    auto findNumber = [&](float& out) {
        for (const auto& t : toks)
        {
            if (t.empty()) continue;
            const char c = t.front();
            if (std::isdigit((unsigned char)c) || c == '-' || c == '.')
            { out = std::strtof(t.c_str(), nullptr); return true; }
        }
        return false;
    };

    auto path = ed.pathTo(sel);
    std::string pathStr;
    for (std::size_t i = 0; i < path.size(); ++i)
    { if (i) pathStr.push_back(','); pathStr += std::to_string(path[i]); }
    if (pathStr.empty()) pathStr = "(root)";

    if (lp.find("hide") != std::string::npos)
        return ed.handleInspectCommand("setvisible " + pathStr + " 0");
    if (lp.find("show") != std::string::npos)
        return ed.handleInspectCommand("setvisible " + pathStr + " 1");
    if (lp.find("delete") != std::string::npos ||
        lp.find("remove") != std::string::npos)
        return ed.handleInspectCommand("delete " + pathStr);

    if (lp.find("rotate") != std::string::npos)
    {
        float deg = 0.0f;
        if (!findNumber(deg)) return "rotate needs a number of degrees";
        return ed.handleInspectCommand(
            "rotate " + std::to_string(deg));
    }
    if (lp.find("scale") != std::string::npos)
    {
        float s = 1.0f;
        if (!findNumber(s)) return "scale needs a number";
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4f", s);
        return ed.handleInspectCommand(std::string("scale ") + buf);
    }
    if (lp.find("resize") != std::string::npos ||
        lp.find("size") != std::string::npos)
    {
        float w = 0.0f;
        if (!findNumber(w)) return "size needs a width";
        char buf[64]; std::snprintf(buf, sizeof(buf), "%.4f", w);
        return ed.handleInspectCommand(std::string("size ") + buf);
    }

    int R = -1, G = 0, B = 0;
    for (const auto& t : toks)
        if (matchColor(t, R, G, B)) break;
    if (R >= 0)
    {
        const std::string rgb =
            std::to_string(R) + " " + std::to_string(G) + " " + std::to_string(B);
        // Panel + the word "background" or "bg" → SingleColor; else
        // CColor. Keeps the same verb consistent across node kinds.
        if (sel.ctype().find("Panel") != std::string::npos &&
            (lp.find("background") != std::string::npos ||
             lp.find("bg")         != std::string::npos))
            return ed.handleInspectCommand(
                "setbgcolor " + pathStr + " " + rgb + " 255");
        return ed.handleInspectCommand(
            "setcolor " + pathStr + " " + rgb + " 255");
    }

    return "no local match — configure an NLP handler for richer prompts";
}

}  // namespace

std::string Editor::dispatchSpriteNlp(const std::string& prompt)
{
    if (_spriteNlpHandler) return _spriteNlpHandler(prompt, *this);
    return localDispatch(prompt, *this);
}

// Selection-tool state. Static so the rect persists across frames
// independent of which Editor instance is rendering (only one panel
// instance is alive at a time in practice).
namespace
{
enum class Tool { None, RectSelect, ColorPick };
static Tool   sTool = Tool::RectSelect;
static bool   sDragActive   = false;
static ImVec2 sDragStartImg{0,0};   // image-local pixels (texture space)
static ImVec2 sDragEndImg  {0,0};
static bool   sHaveSel      = false;
static float  sZoom         = 1.0f;

/// Resolve the sprite path to display: pick the first FileData /
/// equivalent reference on the selected node. Returns absolute path
/// + an optional plist (atlas) sibling. Empty path = no image to show.
std::pair<std::filesystem::path, std::filesystem::path>
resolveSpritePath(const Editor& ed, const opencs::Node& n)
{
    namespace fs = std::filesystem;
    if (!n.valid()) return {};
    auto refs = n.imageRefs();
    if (refs.empty()) return {};
    const auto& r = refs.front();
    if (r.path.empty()) return {};
    const fs::path root = ed.assetsRoot();
    fs::path img;
    fs::path plist;
    if (!r.plist.empty())
    {
        // Atlas frame: plist sibling exists under the assets root, the
        // .png lives next to it (same stem). Build atlas-png path.
        plist = root / r.plist;
        fs::path png = plist; png.replace_extension(".png");
        img = png;
    }
    else
    {
        img = root / r.path;
    }
    std::error_code ec;
    if (!fs::exists(img, ec)) return {};
    return {img, plist};
}

}  // namespace

void drawSpriteEditorBody(Editor& editor)
{
    const auto sel = editor.selected();
    if (sel.valid())
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                           "target: %s  (%s)",
                           sel.name().c_str(), sel.ctype().c_str());
    else
        ImGui::TextDisabled("no node selected — pick one first");

    // ---- Tool palette -----------------------------------------------
    ImGui::Spacing();
    auto toolBtn = [&](const char* label, Tool t) {
        const bool active = (sTool == t);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.30f, 0.62f, 0.92f, 1.0f));
        if (ImGui::SmallButton(label)) sTool = t;
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    toolBtn("None",      Tool::None);
    toolBtn("Rect",      Tool::RectSelect);
    toolBtn("Picker",    Tool::ColorPick);
    if (sHaveSel && ImGui::SmallButton("Clear"))
    { sHaveSel = false; sDragActive = false; }
    ImGui::NewLine();

    // ---- Sprite view ------------------------------------------------
    auto [imgPath, plistPath] = resolveSpritePath(editor, sel);
    if (imgPath.empty())
    {
        ImGui::TextDisabled(
            "no sprite to display (selection has no FileData ref)");
    }
    else
    {
        // Cache Texture2D by path so addImage isn't called every frame.
        static std::string  sLoadedPath;
        static ax::Texture2D* sTex = nullptr;
        if (imgPath.string() != sLoadedPath)
        {
            sTex = ax::Director::getInstance()
                       ->getTextureCache()
                       ->addImage(imgPath.string());
            sLoadedPath = imgPath.string();
            sHaveSel = false;   // selection invalid after switch
        }

        if (sTex)
        {
            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("zoom", &sZoom, 0.1f, 8.0f, "%.2fx");

            const auto szPx = sTex->getContentSizeInPixels();
            const float fitMaxW = ImGui::GetContentRegionAvail().x - 20;
            const float fitScale = (szPx.width > 0)
                ? std::min(1.0f, fitMaxW / szPx.width) : 1.0f;
            const float dispW = szPx.width  * fitScale * sZoom;
            const float dispH = szPx.height * fitScale * sZoom;
            const float pxToDisp = (dispW > 0 && szPx.width > 0)
                ? dispW / szPx.width : 1.0f;

            ImGui::BeginChild("##spriteCanvas", ImVec2(0, 0), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();

            auto [texID, ref] = ax::extension::ImGuiPresenter::getInstance()
                                    ->useTexture(sTex);
            (void)ref;
            if (texID) ImGui::Image(texID, ImVec2(dispW, dispH));

            // Selection overlay + drag input. InvisibleButton sits ON
            // TOP of the image so it captures clicks without ImGui
            // routing them elsewhere. We position the cursor back to
            // p0 and overlap by the same width/height.
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("##selOverlay",
                                   ImVec2(dispW, dispH));
            const bool hovered = ImGui::IsItemHovered();
            const bool held    = ImGui::IsItemActive();
            const ImVec2 mouse = ImGui::GetMousePos();
            const auto toImg = [&](const ImVec2& s) -> ImVec2 {
                return ImVec2((s.x - p0.x) / pxToDisp,
                              (s.y - p0.y) / pxToDisp);
            };

            if (sTool == Tool::RectSelect)
            {
                if (held && !sDragActive)
                {
                    sDragActive   = true;
                    sDragStartImg = toImg(mouse);
                    sDragEndImg   = sDragStartImg;
                    sHaveSel      = false;
                }
                else if (held && sDragActive)
                {
                    sDragEndImg = toImg(mouse);
                }
                else if (!held && sDragActive)
                {
                    sDragActive = false;
                    sHaveSel = (sDragStartImg.x != sDragEndImg.x ||
                                sDragStartImg.y != sDragEndImg.y);
                }
            }
            else if (sTool == Tool::ColorPick)
            {
                if (held)
                {
                    sDragEndImg = toImg(mouse);
                    sHaveSel    = true;
                    sDragStartImg = sDragEndImg;
                }
            }

            // Draw the live selection rect.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto rectInDisp = [&](ImVec2 a, ImVec2 b,
                                  ImVec2& outA, ImVec2& outB) {
                outA = ImVec2(p0.x + std::min(a.x, b.x) * pxToDisp,
                              p0.y + std::min(a.y, b.y) * pxToDisp);
                outB = ImVec2(p0.x + std::max(a.x, b.x) * pxToDisp,
                              p0.y + std::max(a.y, b.y) * pxToDisp);
            };
            if (sTool == Tool::RectSelect && (sDragActive || sHaveSel))
            {
                ImVec2 a, b;
                rectInDisp(sDragStartImg, sDragEndImg, a, b);
                dl->AddRectFilled(a, b, IM_COL32(110, 235, 255, 40));
                dl->AddRect(a, b, IM_COL32(110, 235, 255, 255), 0, 0, 1.5f);
            }
            if (sTool == Tool::ColorPick && sHaveSel)
            {
                const ImVec2 c(p0.x + sDragStartImg.x * pxToDisp,
                               p0.y + sDragStartImg.y * pxToDisp);
                dl->AddCircle(c, 6.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
                dl->AddCircle(c, 4.0f, IM_COL32(0, 0, 0, 255), 0, 1.0f);
            }
            ImGui::EndChild();

            // ---- Read-out ------------------------------------------
            ImGui::TextDisabled("texture: %.0fx%.0f px  ·  display: %.0fx%.0f",
                                szPx.width, szPx.height, dispW, dispH);
            if (sHaveSel)
            {
                if (sTool == Tool::RectSelect)
                {
                    const float x = std::min(sDragStartImg.x, sDragEndImg.x);
                    const float y = std::min(sDragStartImg.y, sDragEndImg.y);
                    const float w = std::abs(sDragEndImg.x - sDragStartImg.x);
                    const float h = std::abs(sDragEndImg.y - sDragStartImg.y);
                    ImGui::Text("rect: x=%.0f y=%.0f  w=%.0f h=%.0f",
                                x, y, w, h);
                    if (ImGui::SmallButton("Copy as Scale9 origin"))
                    {
                        char tmp[128];
                        std::snprintf(tmp, sizeof(tmp),
                            "setattr %s Scale9OriginX %.0f Scale9OriginY %.0f "
                            "Scale9Width %.0f Scale9Height %.0f",
                            "(self)", x, y, w, h);
                        editor._spriteNlpReply = tmp;
                    }
                }
                else if (sTool == Tool::ColorPick)
                    ImGui::Text("pick: x=%.0f y=%.0f  "
                                "(actual pixel read needs CPU readback — TODO)",
                                sDragStartImg.x, sDragStartImg.y);
            }
        }
        else
        {
            ImGui::TextDisabled("failed to load texture: %s",
                                imgPath.string().c_str());
        }
    }

    // ---- NLP prompt --------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "type a prompt: \"hide\", \"red\", \"rotate 30\", "
        "\"scale 1.5\", \"size 200\", \"paint background terracotta\"");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-100.0f);
    const bool committed = ImGui::InputTextWithHint(
        "##prompt", "prompt…",
        editor._spriteNlpInput, sizeof(editor._spriteNlpInput),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool sendClicked = ImGui::Button("Send", ImVec2(80, 0));
    if ((committed || sendClicked) && editor._spriteNlpInput[0] != 0)
    {
        editor._spriteNlpReply = editor.dispatchSpriteNlp(editor._spriteNlpInput);
        editor._spriteNlpInput[0] = 0;
    }

    if (!editor._spriteNlpReply.empty())
    {
        ImGui::Spacing();
        ImGui::Text("reply:");
        ImGui::BeginChild("##nlpReply", ImVec2(0, 80), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(editor._spriteNlpReply.c_str());
        ImGui::EndChild();
    }
}

}  // namespace opencs
