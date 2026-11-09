// GNU-Upscaler - Modern ImGui Widget System
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "GnuUi.h"

#include <menu/menu_common.h>
#include <imgui/imgui_internal.h>

#include <cmath>
#include <cstdio>

namespace GnuUi
{
namespace
{
void TooltipBody(const char* body, std::initializer_list<const char*> footnotes, const char* ini)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    if (body != nullptr && *body)
        ImGui::TextUnformatted(body);
    bool first = true;
    for (const char* note : footnotes)
    {
        if (note == nullptr || !*note)
            continue;
        if (first)
            ImGui::Spacing();
        first = false;
        ImGui::TextUnformatted(note);
    }
    if (ini != nullptr && *ini)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("ini setting: %s", ini);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool HelpHovered()
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        return false;
    if (ImGui::IsItemActive())
        return false;
    const ImGuiContext& g = *GImGui;
    const ImGuiLastItemData& it = g.LastItemData;
    if (!g.NavHighlightItemUnderNav && it.NavRect.Max.x < it.Rect.Max.x - 1.0f && g.IO.MousePos.x < it.NavRect.Max.x)
        return false;
    return true;
}

inline ImVec4 Transparent() { return ImVec4(0.0f, 0.0f, 0.0f, 0.0f); }

struct SliderLook
{
    bool skip = true;
    ImGuiWindow* window = nullptr;
    ImVec2 pos {};
    float width = 0.0f;
    ImGuiID id = 0;
    bool hid = false;
};

SliderLook BeginSliderLook(const char* label)
{
    SliderLook l;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window == nullptr || window->SkipItems)
        return l;
    l.skip = false;
    l.window = window;
    l.pos = window->DC.CursorPos;
    l.width = ImGui::CalcItemWidth();
    l.id = window->GetID(label);
    l.hid = !ImGui::TempInputIsActive(l.id);
    if (l.hid)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Transparent());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Transparent());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Transparent());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Transparent());
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, Transparent());
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Transparent());
    }
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 0.0f);
    return l;
}

float FillRatio(ImGuiDataType type, const void* v, const void* vMin, const void* vMax, ImGuiSliderFlags flags)
{
    if (type == ImGuiDataType_S32)
    {
        const double a = *static_cast<const int*>(vMin), b = *static_cast<const int*>(vMax);
        const double x = *static_cast<const int*>(v);
        if (b <= a)
            return 1.0f;
        return static_cast<float>(ImClamp((x - a + 1.0) / (b - a + 1.0), 0.0, 1.0));
    }
    const double a = *static_cast<const float*>(vMin), b = *static_cast<const float*>(vMax);
    const double x = *static_cast<const float*>(v);
    if (!(b > a) || !std::isfinite(x))
        return 0.0f;
    if ((flags & ImGuiSliderFlags_Logarithmic) && a > 0.0)
        return static_cast<float>(ImClamp(std::log(ImMax(x, a) / a) / std::log(b / a), 0.0, 1.0));
    return static_cast<float>(ImClamp((x - a) / (b - a), 0.0, 1.0));
}

void EndSliderLook(const SliderLook& l, const char* label, ImGuiDataType type, const void* v, const void* vMin,
                   const void* vMax, const char* format, ImGuiSliderFlags flags)
{
    if (l.skip)
        return;
    ImGui::PopStyleVar();
    if (l.hid)
        ImGui::PopStyleColor(6);
    if (!l.hid)
        return;
    ImGuiContext& g = *GImGui;
    if (ImGui::TempInputIsActive(l.id))
        return;
    if (g.LastItemData.ID != l.id || !(g.LastItemData.StatusFlags & ImGuiItemStatusFlags_Visible))
        return;
    const ImGuiStyle& style = g.Style;
    ImDrawList* dl = l.window->DrawList;
    const float s = MenuCommon::MockPx();
    const ImRect frame(l.pos, l.pos + ImVec2(l.width, g.FontSize + style.FramePadding.y * 2.0f));
    const bool active = g.ActiveId == l.id;
    const bool hovered = active || g.HoveredId == l.id;

    // Track: 14 mock px, centered in frame
    const float trackH = ImFloor(Px(14.0f));
    const float cy = ImFloor(frame.GetCenter().y);
    const ImRect track(frame.Min.x, cy - ImFloor(trackH * 0.5f), frame.Max.x, cy - ImFloor(trackH * 0.5f) + trackH);
    const float rounding = style.FrameRounding;
    dl->AddRectFilled(track.Min, track.Max,
                      ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered
                                                                                  : ImGuiCol_FrameBg),
                      rounding);

    // Fill track from left to value ratio
    const float t = FillRatio(type, v, vMin, vMax, flags);
    const float fx = track.Min.x + ImFloor(t * track.GetWidth() + 0.5f);
    if (fx > track.Min.x + 0.5f)
    {
        const ImDrawFlags corners =
            fx >= track.Max.x - rounding ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        dl->AddRectFilled(track.Min, ImVec2(fx, track.Max.y),
                          MenuCommon::ThemeColorU32(hovered ? MenuColor::FillActive : MenuColor::Fill), rounding,
                          corners);
    }

    // Value text right-aligned inside track
    char buf[64];
    const char* end = buf + ImGui::DataTypeFormatString(buf, IM_ARRAYSIZE(buf), type, v,
                                                        format != nullptr ? format : ImGui::DataTypeGetInfo(type)->PrintFmt);
    const float vs = ImFloor(g.FontSize * (11.0f / 12.5f) + 0.5f);
    const ImVec2 ts = g.Font->CalcTextSizeA(vs, FLT_MAX, 0.0f, buf, end);
    const ImVec2 tp(ImMax(track.Min.x + 2.0f * s, track.Max.x - 6.0f * s - ts.x), ImFloor(cy - ts.y * 0.5f));
    const ImVec4 clip(track.Min.x, track.Min.y, track.Max.x, track.Max.y);
    dl->AddText(g.Font, vs, tp, MenuCommon::ThemeColorU32(MenuColor::Value), buf, end, 0.0f, &clip);

    // Label placed to the right
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    if (labelEnd != label)
        ImGui::RenderText(ImVec2(frame.Max.x + style.ItemInnerSpacing.x, frame.Min.y + style.FramePadding.y), label,
                          labelEnd, false);
}

struct ComboLook
{
    bool draw = false;
    ImDrawList* dl = nullptr;
    ImRect bb {};
    ImGuiID id = 0;
    bool wasOpen = false;
};

ComboLook BeginComboLook(const char* label, ImGuiComboFlags flags)
{
    ComboLook c;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window == nullptr || window->SkipItems)
        return c;
    if ((flags & (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_NoPreview | ImGuiComboFlags_WidthFitPreview)) != 0)
        return c;
    if (!(g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth))
        ImGui::SetNextItemWidth(ComboWidth());
    const float w = ImGui::CalcItemWidth();
    c.bb = ImRect(window->DC.CursorPos, window->DC.CursorPos + ImVec2(w, ImGui::GetFrameHeight()));
    c.draw = ImGui::IsRectVisible(c.bb.Min, c.bb.Max);
    c.dl = window->DrawList;
    c.id = window->GetID(label);
    c.wasOpen = ImGui::IsPopupOpen(ImHashStr("##ComboPopup", 0, c.id), ImGuiPopupFlags_None);
    return c;
}

void EndComboLook(const ComboLook& c, bool openNow)
{
    if (!c.draw)
        return;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const float arrow = ImGui::GetFrameHeight();
    if (c.bb.GetWidth() <= arrow)
        return;
    const bool hovered = g.HoveredId == c.id;
    const ImVec2 a(c.bb.Max.x - arrow, c.bb.Min.y);
    ImVec4 cover = ImLerp(style.Colors[ImGuiCol_WindowBg],
                          style.Colors[(openNow || c.wasOpen || hovered) ? ImGuiCol_ButtonHovered : ImGuiCol_Button],
                          style.Alpha);
    cover.w = 1.0f;
    c.dl->AddRectFilled(a, c.bb.Max, ImGui::ColorConvertFloat4ToU32(cover), style.FrameRounding,
                        ImDrawFlags_RoundCornersRight);
    const ImVec2 vs = ImGui::CalcTextSize("v");
    c.dl->AddText(ImVec2(c.bb.Max.x - style.FramePadding.x - vs.x, c.bb.Min.y + style.FramePadding.y),
                  ImGui::GetColorU32(ImGuiCol_Text), "v");
}
} // namespace

float Px(float mockPx) { return mockPx * MenuCommon::MockPx(); }

float ControlWidth() { return ImFloor(Px(190.0f)); }

float ComboWidth() { return ImMax(ImGui::CalcItemWidth() - ImFloor(Px(16.0f)), Px(40.0f)); }

bool FillSlider(const char* label, float* v, float vMin, float vMax, const char* format, ImGuiSliderFlags flags)
{
    const SliderLook look = BeginSliderLook(label);
    const bool changed = ImGui::SliderFloat(label, v, vMin, vMax, format, flags);
    EndSliderLook(look, label, ImGuiDataType_Float, v, &vMin, &vMax, format, flags);
    return changed;
}

bool FillSlider(const char* label, int* v, int vMin, int vMax, const char* format, ImGuiSliderFlags flags)
{
    const SliderLook look = BeginSliderLook(label);
    const bool changed = ImGui::SliderInt(label, v, vMin, vMax, format, flags);
    EndSliderLook(look, label, ImGuiDataType_S32, v, &vMin, &vMax, format, flags);
    return changed;
}

bool Checkbox(const char* label, bool* v)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window == nullptr || window->SkipItems)
        return ImGui::Checkbox(label, v);
    const ImGuiStyle& style = g.Style;
    const float frameH = ImGui::GetFrameHeight();
    const float box = ImFloor(Px(12.0f));
    const float inset = (frameH - box) * 0.5f;
    const ImVec2 pos = window->DC.CursorPos;
    const ImGuiID id = window->GetID(label);
    const bool on = *v;

    ImGui::PushStyleColor(ImGuiCol_FrameBg, Transparent());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Transparent());
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Transparent());
    ImGui::PushStyleColor(ImGuiCol_CheckMark, Transparent());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
                        ImVec2(ImMax(style.ItemInnerSpacing.x - (frameH - box), 0.0f), style.ItemInnerSpacing.y));
    const bool pressed = ImGui::Checkbox(label, v);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    if (g.LastItemData.ID == id && (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_Visible))
    {
        const bool hovered = g.HoveredId == id;
        const bool ticked = pressed ? *v : on;
        const ImVec2 bMin(pos.x, ImFloor(pos.y + inset));
        const ImVec2 bMax(bMin.x + box, bMin.y + box);
        const float r = ImMax(1.0f, Px(2.0f));
        if (ticked)
        {
            window->DrawList->AddRectFilled(bMin, bMax, MenuCommon::ThemeColorU32(MenuColor::CheckOn), r);
        }
        else
        {
            if (hovered)
                window->DrawList->AddRectFilled(bMin, bMax, MenuCommon::ThemeColorU32(MenuColor::FrameHovered), r);
            window->DrawList->AddRect(bMin, bMax,
                                      MenuCommon::ThemeColorU32(hovered ? MenuColor::Text : MenuColor::CheckOutline),
                                      r, 0, 1.0f);
        }
    }
    return pressed;
}

bool Button(const char* label, const ImVec2& size, bool selected)
{
    int colors = 1;
    ImGui::PushStyleColor(ImGuiCol_Border,
                          MenuCommon::ThemeColor(selected ? MenuColor::Selected : MenuColor::ButtonBorder));
    if (selected)
    {
        const ImVec4 fill = MenuCommon::ThemeColor(MenuColor::Selected);
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, MenuCommon::ThemeColor(MenuColor::CheckOn));
        colors += 3;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImFloor(Px(10.0f)), ImGui::GetStyle().FramePadding.y));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(colors);
    return pressed;
}

bool Combo(const char* label, int* current, const char* itemsSeparatedByZeros, int popupMaxHeightInItems)
{
    const ComboLook look = BeginComboLook(label, ImGuiComboFlags_None);
    const bool changed = ImGui::Combo(label, current, itemsSeparatedByZeros, popupMaxHeightInItems);
    EndComboLook(look, false);
    return changed;
}

bool Combo(const char* label, int* current, const char* const items[], int itemsCount, int popupMaxHeightInItems)
{
    const ComboLook look = BeginComboLook(label, ImGuiComboFlags_None);
    const bool changed = ImGui::Combo(label, current, items, itemsCount, popupMaxHeightInItems);
    EndComboLook(look, false);
    return changed;
}

bool BeginCombo(const char* label, const char* previewValue, ImGuiComboFlags flags)
{
    const ComboLook look = BeginComboLook(label, flags);
    const bool open = ImGui::BeginCombo(label, previewValue, flags);
    EndComboLook(look, open);
    return open;
}

bool TreeNode(const char* label, ImGuiTreeNodeFlags flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window == nullptr || window->SkipItems)
        return ImGui::TreeNodeEx(label, flags | ImGuiTreeNodeFlags_FramePadding);
    const ImGuiStyle& style = g.Style;
    const ImVec2 pos = window->DC.CursorPos;
    const float baseOffset = window->DC.CurrLineTextBaseOffset;
    const ImVec2 pad(ImFloor(Px(1.5f) + 0.5f), style.FramePadding.y);
    const ImGuiID id = window->GetID(label);

    ImGui::PushStyleColor(ImGuiCol_Text, Transparent());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    const bool open = ImGui::TreeNodeEx(label, flags | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (g.LastItemData.ID == id && (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_Visible))
    {
        const float textY = pos.y + ImMax(pad.y, baseOffset);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        window->DrawList->AddText(ImVec2(pos.x + pad.x, textY), col, open ? "v" : ">");
        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        if (labelEnd != label)
            window->DrawList->AddText(ImVec2(pos.x + g.FontSize + pad.x * 2.0f, textY), col, label, labelEnd);
    }
    return open;
}

void SectionHeader(const char* label)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window == nullptr || window->SkipItems)
        return;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImFloor(Px(9.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, Transparent());
    ImGui::PushStyleColor(ImGuiCol_Separator, Transparent());
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextBorderSize, 0.0f);
    ImGui::SeparatorText(label);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    window->DrawList->AddText(mn, MenuCommon::ThemeColorU32(MenuColor::Section), label, labelEnd);
    const float y = ImFloor(mn.y + g.FontSize + Px(2.0f));
    window->DrawList->AddLine(ImVec2(mn.x, y), ImVec2(ImMax(mx.x, window->WorkRect.Max.x), y),
                              MenuCommon::ThemeColorU32(MenuColor::Rule), 1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImFloor(Px(4.0f)));
}

void DimTag(const char* text)
{
    if (text == nullptr || !*text)
        return;
    ImGui::SameLine(0.0f, ImFloor(Px(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, MenuCommon::ThemeColor(MenuColor::Tag));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void LabelWithHelp(const char* label, const char* body, std::initializer_list<const char*> footnotes, const char* ini)
{
    ImGui::SameLine();
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    HelpForLastItem(body, footnotes, ini);
}

void Help(const char* body, std::initializer_list<const char*> footnotes, const char* ini)
{
    if (HelpHovered())
        TooltipBody(body, footnotes, ini);
}

void HelpForLastItem(const char* body, std::initializer_list<const char*> footnotes, const char* ini)
{
    if (HelpHovered())
        TooltipBody(body, footnotes, ini);
}

bool LastItemHelpHovered() { return HelpHovered(); }

bool ResetAfterSlider(const char* label, bool show)
{
    if (!show)
        return false;
    ImGuiContext& g = *GImGui;
    const ImGuiLastItemData slider = g.LastItemData;
    ImGui::SameLine();
    const std::string resetId = std::string("Reset##") + label;
    const bool pressed = Button(resetId.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", "Back to default");
    g.LastItemData = slider;
    return pressed;
}

int Segmented(const char* id, std::span<const char* const> labels, int active, bool fillWidth)
{
    int clicked = -1;
    const int n = static_cast<int>(labels.size());
    if (n <= 0)
        return clicked;
    ImGui::PushID(id);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = fillWidth ? (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * float(n - 1)) / float(n) : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (i)
            ImGui::SameLine();
        ImGui::PushID(i);
        if (Button(labels[i], ImVec2(w, 0.0f), i == active))
            clicked = i;
        ImGui::PopID();
    }
    ImGui::PopID();
    return clicked;
}
} // namespace GnuUi
