#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace logger = SKSE::log;
namespace fs = std::filesystem;

Config               g_config;
std::recursive_mutex g_configMutex;
std::uint32_t        g_configVersion{ 0 };

namespace
{
	const char* kBaseIni = "Data/SKSE/Plugins/BlankMap.ini";       // hand-edited, ships with the mod
	const char* kMenuIni = "Data/SKSE/Plugins/BlankMap_Menu.ini";  // written by the in-game menu, loaded last

	std::int64_t g_stamp{ -1 };

	std::string Trim(const std::string& s)
	{
		const auto b = s.find_first_not_of(" \t\r\n");
		if (b == std::string::npos) return {};
		const auto e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}

	bool IsTrue(const std::string& v) { return v == "1" || ToLower(v) == "true"; }

	std::set<std::string> SplitList(const std::string& v)
	{
		std::set<std::string> out;
		std::stringstream     ss(v);
		std::string           item;
		while (std::getline(ss, item, ';')) {
			item = ToLower(Trim(item));
			if (!item.empty()) out.insert(item);
		}
		return out;
	}

	std::int64_t FileStamp(const char* a_path)
	{
		std::error_code ec;
		const auto      t = fs::last_write_time(a_path, ec);
		return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
	}

	std::int64_t CombinedStamp() { return FileStamp(kBaseIni) + FileStamp(kMenuIni); }

	void ApplyIni(const char* a_path)
	{
		std::ifstream f(a_path);
		if (!f) return;

		std::string line, section;
		while (std::getline(f, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#') continue;
			if (line.front() == '[' && line.back() == ']') {
				section = ToLower(line.substr(1, line.size() - 2));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) continue;
			const auto rawKey = Trim(line.substr(0, eq));
			const auto key = ToLower(rawKey);
			const auto val = Trim(line.substr(eq + 1));

			try {
				if (section == "hide") {
					if (g_config.groupHide.contains(key)) g_config.groupHide[key] = IsTrue(val);
					else logger::warn("Unknown marker group '{}'", key);
				} else if (section == "names") {
					if (key == "keeplocations") g_config.keepRaw = val;
					else if (key == "hidelocations") g_config.hideRaw = val;
				} else if (section == "general") {
					if (key == "logdecisions") g_config.logDecisions = IsTrue(val);
					else if (key == "keepplayermarker") g_config.keepPlayerMarker = IsTrue(val);
				} else if (section == "camera") {
					if (key == "recenter") g_config.recenter = IsTrue(val);
					else if (key == "mode") g_config.useFixed = (ToLower(val) == "fixed" || val == "1");
					else if (key == "startx") g_config.fixedX = std::stof(val);
					else if (key == "starty") g_config.fixedY = std::stof(val);
					else if (key == "localmaprecenter") g_config.localRecenter = IsTrue(val);
					else if (key == "localmapmode") g_config.localCorner = (ToLower(val) == "corner" || val == "1");
				}  // (an old [HUD] section from earlier versions is simply ignored)
			} catch (...) {
				logger::warn("Bad value for [{}] {}", section, rawKey);
			}
		}
	}
}

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

const std::vector<MarkerGroup>& MarkerGroups()
{
	static const std::vector<MarkerGroup> groups{
		{ "city", "Cities", { 1 } },
		{ "town", "Towns", { 2 } },
		{ "settlement", "Settlements", { 3 } },
		{ "cave", "Caves", { 4 } },
		{ "camp", "Camps", { 5 } },
		{ "fort", "Forts", { 6 } },
		{ "nordicruin", "Nordic ruins", { 7 } },
		{ "dwemerruin", "Dwemer ruins", { 8 } },
		{ "shipwreck", "Shipwrecks", { 9 } },
		{ "grove", "Groves", { 10 } },
		{ "landmark", "Landmarks", { 11 } },
		{ "dragonlair", "Dragon lairs", { 12 } },
		{ "farm", "Farms", { 13 } },
		{ "woodmill", "Wood mills", { 14 } },
		{ "mine", "Mines", { 15 } },
		{ "imperialcamp", "Imperial camps", { 16 } },
		{ "stormcloakcamp", "Stormcloak camps", { 17 } },
		{ "doomstone", "Standing stones", { 18 } },
		{ "wheatmill", "Wheat mills", { 19 } },
		{ "smelter", "Smelters", { 20 } },
		{ "stable", "Stables", { 21 } },
		{ "imperialtower", "Imperial towers", { 22 } },
		{ "clearing", "Clearings", { 23 } },
		{ "pass", "Passes", { 24 } },
		{ "altar", "Altars", { 25 } },
		{ "rock", "Rocks", { 26 } },
		{ "lighthouse", "Lighthouses", { 27 } },
		{ "orcstronghold", "Orc strongholds", { 28 } },
		{ "giantcamp", "Giant camps", { 29 } },
		{ "shack", "Shacks", { 30 } },
		{ "nordictower", "Nordic towers", { 31 } },
		{ "nordicdwelling", "Nordic dwellings", { 32 } },
		{ "docks", "Docks", { 33 } },
		{ "shrine", "Shrines", { 34 } },
		{ "holdcapitals", "Hold capitals and castles", { 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52 } },
		{ "dlcmarkers", "Dawnguard / Dragonborn markers", { 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65 } },
	};
	return groups;
}

Config::Config()
{
	for (const auto& g : MarkerGroups()) groupHide[g.key] = true;  // default: hide everything
}

void RebuildDerived()
{
	std::lock_guard lk(g_configMutex);
	g_config.hiddenTypes.clear();
	for (const auto& g : MarkerGroups()) {
		auto it = g_config.groupHide.find(g.key);
		if (it != g_config.groupHide.end() && it->second) {
			g_config.hiddenTypes.insert(g.ids.begin(), g.ids.end());
		}
	}
	g_config.keepNames = SplitList(g_config.keepRaw);
	g_config.hideNames = SplitList(g_config.hideRaw);
}

void LoadConfig()
{
	std::lock_guard lk(g_configMutex);
	g_config = Config{};
	ApplyIni(kBaseIni);
	ApplyIni(kMenuIni);  // in-game menu settings override the hand-edited INI
	RebuildDerived();
	g_stamp = CombinedStamp();
	++g_configVersion;
	logger::info("Config loaded: {} hidden type ids, keepPlayerMarker={}, recenter={}, localRecenter={}",
		g_config.hiddenTypes.size(), g_config.keepPlayerMarker, g_config.recenter, g_config.localRecenter);
}

void MaybeReloadConfig()
{
	if (CombinedStamp() != g_stamp) LoadConfig();
}

void SaveMenuConfig()
{
	std::lock_guard lk(g_configMutex);
	std::ofstream   f(kMenuIni, std::ios::trunc);
	if (!f) {
		logger::warn("Could not write {}", kMenuIni);
		return;
	}
	f << "; Written by the Blank Map in-game menu. Delete this file to go back to BlankMap.ini.\n\n[Hide]\n";
	for (const auto& g : MarkerGroups()) {
		auto it = g_config.groupHide.find(g.key);
		f << g.key << '=' << ((it != g_config.groupHide.end() && it->second) ? 1 : 0) << '\n';
	}
	f << "\n[Names]\nKeepLocations=" << g_config.keepRaw << "\nHideLocations=" << g_config.hideRaw << '\n';
	f << "\n[Camera]\nRecenter=" << g_config.recenter << "\nMode=" << (g_config.useFixed ? "fixed" : "center")
	  << "\nStartX=" << g_config.fixedX << "\nStartY=" << g_config.fixedY
	  << "\nLocalMapRecenter=" << g_config.localRecenter << "\nLocalMapMode=" << (g_config.localCorner ? "corner" : "center") << '\n';
	f << "\n[General]\nLogDecisions=" << g_config.logDecisions << "\nKeepPlayerMarker=" << g_config.keepPlayerMarker << '\n';
	f.close();
	g_stamp = CombinedStamp();  // our own write must not trigger a reload
}

void ResetConfigToDefaults()
{
	std::lock_guard lk(g_configMutex);
	g_config = Config{};
	RebuildDerived();
	++g_configVersion;
	SaveMenuConfig();
}
