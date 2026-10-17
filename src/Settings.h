#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// One checkbox in the menu / one INI key under [Hide].
struct MarkerGroup
{
	const char*                key;    // lowercase INI key
	const char*                label;  // shown in the menu
	std::vector<std::uint16_t> ids;    // MapMarkerData::type values
};
const std::vector<MarkerGroup>& MarkerGroups();

struct Config
{
	Config();

	std::map<std::string, bool> groupHide;  // group key -> hidden?
	std::string                 keepRaw;    // "Whiterun;Riverwood"
	std::string                 hideRaw;

	// Derived by RebuildDerived()
	std::set<std::uint16_t> hiddenTypes;
	std::set<std::string>   keepNames;
	std::set<std::string>   hideNames;

	bool  recenter{ true };  // world map camera
	int   startMode{ 2 };  // 0 = map center, 1 = fixed point, 2 = last position (falls back to center)
	bool  rememberZoom{ false };  // last-position mode also restores camera height (experimental)
	float fixedX{ 0.0f };
	float fixedY{ 0.0f };
	bool  localRecenter{ true };  // local map camera
	bool  localCorner{ false };

	// Black screen over the map while the open animation / recentering runs.
	bool  blackout{ true };
	float blackoutHold{ 0.75f };  // seconds to stay black after the recenter window ends
	float blackoutFade{ 0.35f };  // seconds to fade back in

	bool keepPlayerMarker{ true };  // never hide the player's own custom map marker

	bool logDecisions{ false };
};

// Where the world map camera was when the player last closed the map.
// Kept apart from Config so reloading settings never wipes it.
struct MapState
{
	bool  valid{ false };
	float x{ 0.0f };
	float y{ 0.0f };
	float z{ 0.0f };
};

extern MapState             g_mapState;  // guarded by g_configMutex
extern Config               g_config;
extern std::recursive_mutex g_configMutex;  // guards g_config (game thread + render thread)
extern std::uint32_t        g_configVersion;  // bumped whenever the config is (re)loaded

std::string ToLower(std::string s);

void LoadConfig();             // base INI, then menu INI on top
void MaybeReloadConfig();      // reloads if either file changed on disk
void RebuildDerived();         // call after editing groupHide / keepRaw / hideRaw
void SaveMenuConfig();         // writes Data/SKSE/Plugins/BlankMap_Menu.ini
void ResetConfigToDefaults();

void LoadMapState();   // Data/SKSE/Plugins/BlankMap_State.ini (written when the map closes)
void SaveMapState();
void ClearMapState();