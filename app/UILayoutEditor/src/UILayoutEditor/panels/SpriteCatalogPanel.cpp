// Sprite Catalog panel: browse local Resources/ entries and search a
// host-installed online provider for additional sprites. When a sprite
// is clicked while a node is selected, the catalog applies it via the
// existing RPC mutation surface (`setfile` for ImageView,
// `setpanelimage` for PanelObjectData) so the canvas updates live.

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace opencs
{

namespace
{

/// Walk an AssetEntry tree, flatten to a vector of (label, absPath).
/// Only File + AtlasFrame leaves count — folders and atlas containers
/// are skipped (the user picks individual sprites here).
void flattenAssets(const std::vector<AssetEntry>& in,
                   std::vector<std::pair<std::string, std::string>>& out,
                   const std::string& parentLabel = "")
{
    for (const auto& e : in)
    {
        if (e.kind == AssetKind::File ||
            e.kind == AssetKind::AtlasFrame)
        {
            std::string label = parentLabel.empty()
                ? e.displayName : parentLabel + " / " + e.displayName;
            out.emplace_back(std::move(label), e.absPath.string());
        }
        if (!e.children.empty())
            flattenAssets(e.children,
                          out,
                          parentLabel.empty()
                              ? e.displayName
                              : parentLabel + " / " + e.displayName);
    }
}

/// Apply `relPath` to the currently-selected node via the right RPC
/// command for its ctype. ImageView → setfile, Panel → setpanelimage.
/// Anything else → status string, no mutation.
std::string applySpriteToSelection(Editor& ed,
                                   const std::string& relPath,
                                   const std::string& plist = "")
{
    if (!ed.selected().valid()) return "no node selected";
    auto path = ed.pathTo(ed.selected());
    std::string pathStr;
    for (std::size_t i = 0; i < path.size(); ++i)
    { if (i) pathStr.push_back(','); pathStr += std::to_string(path[i]); }
    if (pathStr.empty()) pathStr = "(root)";

    const std::string ctype = ed.selected().ctype();
    if (ctype.find("ImageView") != std::string::npos)
        return ed.handleInspectCommand(
            "setfile " + pathStr + " " + relPath +
            (plist.empty() ? "" : " " + plist));
    if (ctype.find("Panel") != std::string::npos)
        return ed.handleInspectCommand(
            "setpanelimage " + pathStr + " " + relPath +
            (plist.empty() ? "" : " " + plist));
    return "selected node (" + ctype +
           ") doesn't accept a sprite directly";
}

}  // namespace

void Editor::runSpriteSearch()
{
    _spriteSearchResults.clear();
    _spriteSearchStatus.clear();
    if (!_spriteSearchProvider)
    {
        _spriteSearchStatus =
            "no provider configured (Editor::setSpriteSearchProvider)";
        return;
    }
    try
    {
        _spriteSearchResults = _spriteSearchProvider(_spriteSearchQuery);
        _spriteSearchStatus  = _spriteSearchResults.empty()
            ? std::string("no results")
            : (std::to_string(_spriteSearchResults.size()) + " results");
    }
    catch (const std::exception& e)
    {
        _spriteSearchStatus = std::string("search error: ") + e.what();
    }
}

void drawSpriteCatalogBody(Editor& editor)
{
    if (ImGui::BeginTabBar("##catalogTabs"))
    {
        // ---- Local tab ------------------------------------------------
        if (ImGui::BeginTabItem("Local"))
        {
            std::vector<std::pair<std::string, std::string>> flat;
            flattenAssets(editor.assets(), flat);
            ImGui::TextDisabled(
                "%zu local sprite(s) under %s",
                flat.size(),
                editor.assetsRoot().filename().string().c_str());
            ImGui::Spacing();
            ImGui::BeginChild("##localGrid", ImVec2(0, 0), true);
            // Plain list for now — thumbnail loading needs an ImGui
            // texture upload path that's not exposed here yet. Each
            // row applies the asset on click via the selection-aware
            // dispatcher above.
            for (const auto& [label, abs] : flat)
            {
                ImGui::PushID(abs.c_str());
                if (ImGui::Selectable(label.c_str()))
                {
                    namespace fs = std::filesystem;
                    fs::path rel = fs::relative(
                        fs::path(abs), editor.assetsRoot());
                    auto reply = applySpriteToSelection(editor, rel.string());
                    editor._spriteSearchStatus = reply;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", abs.c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---- Online tab -----------------------------------------------
        if (ImGui::BeginTabItem("Online Search"))
        {
            ImGui::TextDisabled(
                "provider: %s",
                editor.hasSpriteSearchProvider()
                    ? "configured" : "NOT configured");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-100.0f);
            const bool committed = ImGui::InputTextWithHint(
                "##q", "search query…",
                editor._spriteSearchQuery,
                sizeof(editor._spriteSearchQuery),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool searchClicked =
                ImGui::Button("Search", ImVec2(80, 0));
            if ((committed || searchClicked) &&
                editor._spriteSearchQuery[0] != 0)
                editor.runSpriteSearch();

            ImGui::Spacing();
            if (!editor._spriteSearchStatus.empty())
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                                   "%s", editor._spriteSearchStatus.c_str());

            ImGui::Spacing();
            ImGui::BeginChild("##results", ImVec2(0, 0), true);
            for (const auto& r : editor._spriteSearchResults)
            {
                ImGui::PushID(r.downloadUrl.c_str());
                ImGui::Bullet();
                ImGui::SameLine();
                if (ImGui::Selectable(r.label.c_str(), false,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    // Import: derive a filename from the URL (or
                    // label) and route through the same `download`
                    // RPC the agent uses. After save, rescan the
                    // asset list, then apply to the selected node
                    // (if any) just like a Local-tab click.
                    namespace fs = std::filesystem;
                    std::string fname;
                    auto slash = r.downloadUrl.find_last_of('/');
                    if (slash != std::string::npos)
                        fname = r.downloadUrl.substr(slash + 1);
                    if (fname.empty()) fname = r.label;
                    // Strip query string + sanitise. Filename-unsafe
                    // chars folded to '_' so the writer side never
                    // chokes on the host's URL conventions.
                    auto q = fname.find('?');
                    if (q != std::string::npos) fname.resize(q);
                    for (char& c : fname)
                        if (!(std::isalnum((unsigned char)c) ||
                              c == '.' || c == '-' || c == '_')) c = '_';
                    const std::string rel = "imported/" + fname;
                    const std::string reply = editor.handleInspectCommand(
                        "download " + r.downloadUrl + " " + rel);
                    editor._spriteSearchStatus = reply;
                    if (reply.rfind("ok ", 0) == 0)
                    {
                        editor.rescanAssets();
                        // Apply to currently-selected node when one
                        // exists + accepts a sprite. Local-tab logic
                        // is identical, so reuse via a synthetic
                        // setfile / setpanelimage call.
                        if (editor.selected().valid())
                        {
                            auto path = editor.pathTo(editor.selected());
                            std::string pathStr;
                            for (std::size_t i = 0; i < path.size(); ++i)
                            { if (i) pathStr.push_back(','); pathStr += std::to_string(path[i]); }
                            if (pathStr.empty()) pathStr = "(root)";
                            const std::string ct = editor.selected().ctype();
                            const char* verb =
                                ct.find("ImageView") != std::string::npos ? "setfile"
                              : ct.find("Panel")     != std::string::npos ? "setpanelimage"
                              : nullptr;
                            if (verb)
                                editor.handleInspectCommand(
                                    std::string(verb) + " " + pathStr + " " + rel);
                        }
                    }
                }
                if (ImGui::IsItemHovered() && !r.thumbUrl.empty())
                    ImGui::SetTooltip("%s\n(%s)",
                                      r.thumbUrl.c_str(),
                                      r.provider.c_str());
                ImGui::PopID();
            }
            if (editor._spriteSearchResults.empty())
                ImGui::TextDisabled(
                    editor.hasSpriteSearchProvider()
                        ? "no results yet — enter a query"
                        : "wire a provider via Editor::setSpriteSearchProvider");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace opencs
