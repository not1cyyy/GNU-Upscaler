// GNU-Upscaler - Modern ImGui Widget System
// Inspired by clean, human-designed overlay interfaces
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <imgui/imgui.h>

#include <initializer_list>
#include <span>
#include <string>

namespace GnuUi
{
// Scales a pixel measurement according to the current menu scaling factor.
float Px(float mockPx);

// Standardized 190 px control width across all tabs for consistent layout.
float ControlWidth();

// Standard combo box width (slightly indented).
float ComboWidth();

// Modern filled-track sliders (value right-aligned inside track, no clumsy thumb grab).
bool FillSlider(const char* label, float* v, float vMin, float vMax, const char* format = "%.2f",
                ImGuiSliderFlags flags = 0);
bool FillSlider(const char* label, int* v, int vMin, int vMax, const char* format = "%d",
                ImGuiSliderFlags flags = 0);

// Minimalist 12px square checkbox with solid accent fill when toggled.
bool Checkbox(const char* label, bool* v);

// Clean flat 18px button with accent outline; fills solid when selected.
bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), bool selected = false);

// Dropdown combos styled to seamlessly match the filled frames.
bool Combo(const char* label, int* current, const char* itemsSeparatedByZeros, int popupMaxHeightInItems = -1);
bool Combo(const char* label, int* current, const char* const items[], int itemsCount, int popupMaxHeightInItems = -1);
bool BeginCombo(const char* label, const char* previewValue, ImGuiComboFlags flags = 0);

// Modern disclosure tree nodes with '>' / 'v' indicators.
bool TreeNode(const char* label, ImGuiTreeNodeFlags flags = 0);

// Prominent section header with accent color and a full-width 1px rule.
void SectionHeader(const char* label);

// Subtle muted status tag placed next to labels (e.g. "active", "dx12", "1.5x").
void DimTag(const char* text);

// Label that shows descriptive help when hovered.
void LabelWithHelp(const char* label, const char* body, std::initializer_list<const char*> footnotes = {},
                   const char* ini = nullptr);

// Contextual help that appears on hovering the preceding label (no noisy '(?)' icons).
void Help(const char* body, std::initializer_list<const char*> footnotes = {}, const char* ini = nullptr);
void HelpForLastItem(const char* body, std::initializer_list<const char*> footnotes = {}, const char* ini = nullptr);
bool LastItemHelpHovered();

// Reset button placed inline after a slider to revert to defaults.
bool ResetAfterSlider(const char* label, bool show = true);

// Segmented multi-option toggle bar.
int Segmented(const char* id, std::span<const char* const> labels, int active, bool fillWidth = false);
} // namespace GnuUi
