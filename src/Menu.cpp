// In-game settings pages for SKSE Menu Framework.
// Put the framework's SDK header next to this file as src/SKSEMenuFramework.h.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// The SDK header needs these and does not include them itself.
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

#include "SKSEMenuFramework.h"

#include "Menu.h"
#include "Settings.h"

namespace ui = ImGuiMCP;
namespace logger = SKSE::log;

namespace
{
	std::uint32_t g_seenVersion{ 0 };
	char          g_keepBuf[1024]{};
	char          g_hideBuf[1024]{};
	bool          g_dirty{ false };

	void CopyToBuf(char* a_buf, std::size_t a_size, const std::string& a_src)
	{
		std::snprintf(a_buf, a_size, "%s", a_src.c_str());
	}

	// Refresh text boxes when the config was reloaded behind the menu's back.
	void SyncBuffers()
	{
		if (g_seenVersion == g_configVersion) return;
		CopyToBuf(g_keepBuf, sizeof(g_keepBuf), g_config.keepRaw);
		CopyToBuf(g_hideBuf, sizeof(g_hideBuf), g_config.hideRaw);
		g_seenVersion = g_configVersion;
	}

	void Tip(const char* a_text)
	{
		if (ui::IsItemHovered()) ui::SetTooltip("%s", a_text);
	}

	// Save once the mouse is released so dragging a value doesn't write the file every frame.
	void SaveIfIdle()
	{
		if (g_dirty && !ui::IsAnyMouseDown()) {
			SaveMenuConfig();
			g_dirty = false;
		}
	}

	void __stdcall RenderMarkers()
	{
		std::lock_guard lk(g_configMutex);
		SyncBuffers();

		ui::TextWrapped("Checked marker types are removed from the world map (not just made transparent). "
						"Changes apply the next time you open the map.");
		ui::Spacing();

		if (ui::Button("Hide all")) {
			for (const auto& g : MarkerGroups()) g_config.groupHide[g.key] = true;
			RebuildDerived();
			g_dirty = true;
		}
		ui::SameLine();
		if (ui::Button("Show all")) {
			for (const auto& g : MarkerGroups()) g_config.groupHide[g.key] = false;
			RebuildDerived();
			g_dirty = true;
		}
		ui::Separator();

		int i = 0;
		for (const auto& g : MarkerGroups()) {
			const int col = i % 3;
			if (col != 0) ui::SameLine(static_cast<float>(col) * 250.0f);
			bool v = g_config.groupHide[g.key];
			if (ui::Checkbox(g.label, &v)) {
				g_config.groupHide[g.key] = v;
				RebuildDerived();
				g_dirty = true;
			}
			++i;
		}

		ui::Spacing();
		ui::SeparatorText("Exceptions");
		ui::TextWrapped("Location names separated by semicolons, for example: Whiterun; Riverwood. "
						"These override the type checkboxes above.");

		if (ui::InputText("Always show##keep", g_keepBuf, sizeof(g_keepBuf))) {
			g_config.keepRaw = g_keepBuf;
			RebuildDerived();
			g_dirty = true;
		}
		if (ui::InputText("Always hide##hide", g_hideBuf, sizeof(g_hideBuf))) {
			g_config.hideRaw = g_hideBuf;
			RebuildDerived();
			g_dirty = true;
		}

		SaveIfIdle();
	}

	void __stdcall RenderCamera()
	{
		std::lock_guard lk(g_configMutex);
		SyncBuffers();

		ui::SeparatorText("World map");
		if (ui::Checkbox("Don't open the map on me##world", &g_config.recenter)) g_dirty = true;
		Tip("The world map opens at a chosen view instead of on the player.");

		int startView = g_config.useFixed ? 1 : 0;
		ui::BeginDisabled(!g_config.recenter);
		if (ui::Combo("Start view##world", &startView, "Map center\0Fixed point\0")) {
			g_config.useFixed = (startView == 1);
			g_dirty = true;
		}

		ui::BeginDisabled(!g_config.useFixed);
		if (ui::DragFloat("Fixed point X", &g_config.fixedX, 100.0f, -200000.0f, 200000.0f, "%.0f")) g_dirty = true;
		if (ui::DragFloat("Fixed point Y", &g_config.fixedY, 100.0f, -200000.0f, 200000.0f, "%.0f")) g_dirty = true;
		ui::EndDisabled();
		ui::EndDisabled();
		ui::TextDisabled("World units. BlankMap.log prints the map bounds each time you open the map.");

		ui::Spacing();
		ui::SeparatorText("Local map (interiors)");
		if (ui::Checkbox("Don't open the local map on me##local", &g_config.localRecenter)) g_dirty = true;

		int localMode = g_config.localCorner ? 1 : 0;
		ui::BeginDisabled(!g_config.localRecenter);
		if (ui::Combo("Local map start##local", &localMode, "Area center\0Corner\0")) {
			g_config.localCorner = (localMode == 1);
			g_dirty = true;
		}
		ui::EndDisabled();

		SaveIfIdle();
	}

	void __stdcall RenderGeneral()
	{
		std::lock_guard lk(g_configMutex);
		SyncBuffers();

		ui::SeparatorText("Player markers");
		if (ui::Checkbox("Always show my custom map marker", &g_config.keepPlayerMarker)) g_dirty = true;
		Tip("The marker you place yourself on the map is never hidden. Applies the next time you open the map.");

		ui::Spacing();
		ui::SeparatorText("Developer");
		if (ui::Checkbox("Log every hidden marker", &g_config.logDecisions)) g_dirty = true;
		ui::Spacing();
		if (ui::Button("Reset everything to defaults")) {
			ResetConfigToDefaults();
			g_dirty = false;
		}

		SaveIfIdle();
	}
}

void Menu::Register()
{
	if (!GetMenuFrameworkModule()) {  // not IsInstalled(): that checks the disk and misses MO2's virtual files
		logger::info("SKSE Menu Framework not loaded - in-game menu skipped (BlankMap.ini still works)");
		return;
	}

	SKSEMenuFramework::SetSection("Blank Map");
	SKSEMenuFramework::AddSectionItem("Map Markers", RenderMarkers);
	SKSEMenuFramework::AddSectionItem("Camera", RenderCamera);
	SKSEMenuFramework::AddSectionItem("General", RenderGeneral);
	logger::info("Registered with SKSE Menu Framework");
}
