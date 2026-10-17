// Blank Map - hide world map markers (except the player's own), recenter / remember the map camera.
// SKSE / CommonLibSSE-NG. Original implementation.
//
// Settings: Data/SKSE/Plugins/BlankMap.ini, then overridden by BlankMap_Menu.ini
// (written by the in-game SKSE Menu Framework page). Both are re-checked every
// couple of seconds, so changes apply live.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <cmath>
#include <concepts>
#include <string>
#include <vector>

#include "Menu.h"
#include "Settings.h"

namespace
{
	namespace logger = SKSE::log;

	// ---- Map camera recentering ------------------------------------------
	std::atomic<int> g_pendingFrames{ 0 };
	std::atomic<int> g_transitionCap{ 0 };  // safety cap while the open animation is still running
	std::atomic<bool> g_mapOpen{ false };
	bool             g_loggedCamera{ false };
	bool             g_loggedLocal{ false };

	struct MapCameraHook
	{
		static void Thunk(RE::MapMenu* a_this, float a_interval, std::uint32_t a_time)
		{
			func(a_this, a_interval, a_time);
			OnFrame(a_this);
		}

		static inline REL::Relocation<decltype(Thunk)> func;

		static void RecenterLocalMap(RE::MapMenu* a_menu)
		{
			auto* rd = a_menu->GetRuntimeData();  // null on VR
			if (!rd) return;

			auto* cam = rd->localMapMenu.localCullingProcess.GetLocalMapCamera();
			if (!cam) return;
			auto* state = cam->defaultState.get();
			if (!state) return;

			const auto& mn = cam->minExtent;
			const auto& mx = cam->maxExtent;
			if (!(mx.x > mn.x) || !(mx.y > mn.y)) return;  // bounds not set yet

			float tx = (mn.x + mx.x) * 0.5f;
			float ty = (mn.y + mx.y) * 0.5f;
			if (g_config.localCorner) {
				tx = mn.x + (mx.x - mn.x) * 0.15f;
				ty = mn.y + (mx.y - mn.y) * 0.15f;
			}

			if (!g_loggedLocal) {
				logger::info("Local map before: initial=({:.1f},{:.1f},{:.1f}) translation=({:.1f},{:.1f},{:.1f}) bounds=({:.1f},{:.1f})-({:.1f},{:.1f}) -> target=({:.1f},{:.1f})",
					state->initialPosition.x, state->initialPosition.y, state->initialPosition.z,
					state->translation.x, state->translation.y, state->translation.z,
					mn.x, mn.y, mx.x, mx.y, tx, ty);
				g_loggedLocal = true;
			}
			// view position = initialPosition + translation (see SetDefaultStateTranslation)
			state->translation.x = tx - state->initialPosition.x;
			state->translation.y = ty - state->initialPosition.y;
		}

		// ---- last-position memory ---------------------------------------
		// What we last wrote to the camera. "The player moved it" is measured from here.
		static inline bool  s_haveApplied{ false };
		static inline float s_appliedX{ 0.0f }, s_appliedY{ 0.0f };
		// Where the player has panned to since the map settled.
		static inline bool  s_haveSeen{ false };
		static inline float s_seenX{ 0.0f }, s_seenY{ 0.0f }, s_seenZ{ 0.0f };
		static inline bool  s_loggedTrack{ false };

		static void BeginSession()
		{
			s_haveApplied = false;
			s_haveSeen = false;
			s_loggedTrack = false;
			g_mapOpen.store(true);
		}

		// Called when the map closes: persist the last place the player looked at.
		static void EndSession()
		{
			g_mapOpen.store(false);
			if (!s_haveSeen) return;  // never panned: the saved position stays as it was
			{
				std::lock_guard lk(g_configMutex);
				g_mapState = { true, s_seenX, s_seenY, s_seenZ };
			}
			SaveMapState();
			logger::info("Saved map position ({:.1f}, {:.1f}, {:.1f})", s_seenX, s_seenY, s_seenZ);
			s_haveSeen = false;
		}

		// After our overwrite window is over, watch for the player panning around.
		// There are two camera states and I don't know which one is live, so the one that
		// has moved furthest from what we wrote is taken to be the live one.
		static void TrackPosition(RE::MapMenu* a_menu)
		{
			if (!g_mapOpen.load() || !s_haveApplied) return;

			auto* rd = a_menu->GetRuntimeData2();  // null on VR
			if (!rd || !rd->controlsReady) return;

			int   best = -1;
			float bestDist = 1.0f;  // ignore sub-unit jitter
			for (int i = 0; i < 2; ++i) {
				auto* world = rd->camera.unk68[i].get();
				if (!world) continue;
				const float dx = world->currentPosition.x - s_appliedX;
				const float dy = world->currentPosition.y - s_appliedY;
				const float d = std::sqrt(dx * dx + dy * dy);
				if (d > bestDist) {
					bestDist = d;
					best = i;
				}
			}
			if (best < 0) return;

			auto* world = rd->camera.unk68[best].get();
			s_seenX = world->currentPosition.x;
			s_seenY = world->currentPosition.y;
			s_seenZ = world->currentPosition.z;
			s_haveSeen = true;

			if (!s_loggedTrack) {
				s_loggedTrack = true;
				logger::info("Tracking player pan: live state={} moved {:.1f} units from ({:.1f},{:.1f}); transition active={}",
					best, bestDist, s_appliedX, s_appliedY, rd->camera.unk80.get() != nullptr);
			}
		}

		static void OnFrame(RE::MapMenu* a_menu)
		{
			if (g_pendingFrames.load() <= 0) {
				TrackPosition(a_menu);
				return;
			}

			auto* rd = a_menu->GetRuntimeData2();  // null on VR
			if (!rd || !rd->controlsReady) return;

			bool  recenter, localRecenter, rememberZoom, savedValid;
			int   mode;
			float fixedX, fixedY, savedX, savedY, savedZ;
			{
				std::lock_guard lk(g_configMutex);
				recenter = g_config.recenter;
				mode = g_config.startMode;
				rememberZoom = g_config.rememberZoom;
				localRecenter = g_config.localRecenter;
				fixedX = g_config.fixedX;
				fixedY = g_config.fixedY;
				savedValid = g_mapState.valid;
				savedX = g_mapState.x;
				savedY = g_mapState.y;
				savedZ = g_mapState.z;
			}

			if (recenter) {
				for (auto& state : rd->camera.unk68) {
					auto* world = state.get();
					if (!world) continue;

					float x = 0.0f, y = 0.0f, z = world->currentPosition.z;
					bool  applyZ = false;
					if (mode == 1) {  // fixed point
						x = fixedX;
						y = fixedY;
					} else if (mode == 2 && savedValid) {  // last position
						x = savedX;
						y = savedY;
						if (rememberZoom) {
							z = savedZ;
							applyZ = true;
						}
					} else {  // map center (also the fallback when nothing is saved yet)
						if (!world->mapData) continue;
						x = (world->mapData->minimumCoordinates.x + world->mapData->maximumCoordinates.x) * 0.5f;
						y = (world->mapData->minimumCoordinates.y + world->mapData->maximumCoordinates.y) * 0.5f;
					}

					if (!g_loggedCamera) {
						logger::info("Camera before: pos=({:.1f},{:.1f},{:.1f}) -> target=({:.1f},{:.1f}) mode={} saved={}; transition active={}",
							world->currentPosition.x, world->currentPosition.y, world->currentPosition.z,
							x, y, mode, savedValid, rd->camera.unk80.get() != nullptr);
						if (world->mapData) {
							logger::info("Map bounds: min=({:.1f},{:.1f}) max=({:.1f},{:.1f})",
								world->mapData->minimumCoordinates.x, world->mapData->minimumCoordinates.y,
								world->mapData->maximumCoordinates.x, world->mapData->maximumCoordinates.y);
						}
					}
					world->currentPosition.x = x;
					world->currentPosition.y = y;
					if (applyZ) world->currentPosition.z = z;

					s_appliedX = x;
					s_appliedY = y;
					s_haveApplied = true;
				}
				g_loggedCamera = true;
			}
			if (localRecenter) RecenterLocalMap(a_menu);
			// The map's open animation (the pan/zoom toward the player) can outlast a fixed frame
			// window and then resume from our overwritten position. Keep re-applying while it is
			// still running (hard-capped so a stuck transition can't pin the camera forever).
			if (rd->camera.unk80.get() != nullptr && g_transitionCap.fetch_sub(1) > 0) return;
			g_pendingFrames.fetch_sub(1);
		}

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MapMenu[0] };
			func = vtbl.write_vfunc(0x5, Thunk);  // IMenu::AdvanceMovie
			logger::info("Map camera hook installed");
		}
	};

	// ---- Live config reload ---------------------------------------------
	// Piggybacks on the HUD's per-frame update to notice INI edits (~every 2 s).
	std::uint32_t g_hudFrames{ 0 };

	struct ConfigReloadHook
	{
		static void Thunk(RE::HUDMenu* a_this, float a_interval, std::uint32_t a_time)
		{
			func(a_this, a_interval, a_time);
			if (++g_hudFrames % 120 == 0) MaybeReloadConfig();
		}

		static inline REL::Relocation<decltype(Thunk)> func;

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
			func = vtbl.write_vfunc(0x5, Thunk);  // IMenu::AdvanceMovie
			logger::info("Config reload hook installed");
		}
	};

	// ---- Map marker hiding ----------------------------------------------
	// Some CommonLibSSE-NG versions name a "player set" marker type; only use it if this one does.
	template <class E>
	bool IsPlayerSetType(E a_type)
	{
		if constexpr (requires { E::kPlayerSet; }) {
			return a_type == E::kPlayerSet;
		} else {
			return false;
		}
	}

	// Same idea for the player's own custom-marker reference on the player's runtime data.
	template <class R>
	bool IsPlayerMarkerRef(const R& a_rd, RE::ObjectRefHandle a_handle)
	{
		if constexpr (requires { { a_rd.playerMapMarker } -> std::convertible_to<RE::ObjectRefHandle>; }) {
			return a_rd.playerMapMarker == a_handle;
		} else {
			return false;
		}
	}

	class MapWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MapWatcher* GetSingleton()
		{
			static MapWatcher inst;
			return &inst;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::MapMenu::MENU_NAME) {
				if (a_event->opening) {
					MaybeReloadConfig();
					MapCameraHook::BeginSession();
					Hide();
					bool any;
					{
						std::lock_guard lk(g_configMutex);
						any = g_config.recenter || g_config.localRecenter;
					}
					if (any) {
						g_loggedCamera = false;
						g_loggedLocal = false;
						g_pendingFrames.store(90);
						g_transitionCap.store(600);
					}
				} else {
					g_pendingFrames.store(0);
					g_transitionCap.store(0);
					MapCameraHook::EndSession();
					Restore();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		struct Saved
		{
			RE::ObjectRefHandle handle;
			std::uint8_t        flags;
		};
		std::vector<Saved> _saved;

		static bool ShouldHide(const RE::MapMarkerData* d)
		{
			std::lock_guard   lk(g_configMutex);
			const char*       full = d->locationName.GetFullName();
			const std::string name = ToLower(full ? full : "");
			if (!name.empty()) {
				if (g_config.keepNames.contains(name)) return false;
				if (g_config.hideNames.contains(name)) return true;
			}
			return g_config.hiddenTypes.contains(static_cast<std::uint16_t>(d->type.get()));
		}

		void Hide()
		{
			Restore();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return;

			bool logDecisions, keepPlayer;
			{
				std::lock_guard lk(g_configMutex);
				logDecisions = g_config.logDecisions;
				keepPlayer   = g_config.keepPlayerMarker;
			}

			std::size_t hidden = 0, keptPlayer = 0;
			for (auto& handle : player->GetPlayerRuntimeData().currentMapMarkers) {
				auto ref = handle.get();
				if (!ref) continue;
				auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				if (!extra || !extra->mapData) continue;
				if (keepPlayer && (IsPlayerMarkerRef(player->GetPlayerRuntimeData(), handle) || IsPlayerSetType(extra->mapData->type.get()))) {
					++keptPlayer;  // the player's own custom marker: never touched
					continue;
				}
				if (!ShouldHide(extra->mapData)) continue;

				_saved.push_back({ handle, static_cast<std::uint8_t>(extra->mapData->flags.underlying()) });
				extra->mapData->flags.reset(RE::MapMarkerData::Flag::kVisible, RE::MapMarkerData::Flag::kShowAllHidden);
				++hidden;
				if (logDecisions) {
					logger::info("hid '{}' (type {})", extra->mapData->locationName.GetFullName(),
						static_cast<std::uint16_t>(extra->mapData->type.get()));
				}
			}
			logger::info("Map opened: hid {} markers, kept {} player marker(s) visible", hidden, keptPlayer);
		}

		void Restore()
		{
			for (auto& s : _saved) {
				auto ref = s.handle.get();
				if (!ref) continue;
				auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				if (!extra || !extra->mapData) continue;
				extra->mapData->flags = static_cast<RE::MapMarkerData::Flag>(s.flags);
			}
			if (!_saved.empty()) logger::info("Map closed: restored {} markers", _saved.size());
			_saved.clear();
		}
	};

	void SetupLog()
	{
		auto path = logger::log_directory();
		if (!path) return;
		*path /= "BlankMap.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
		spdlog::set_default_logger(std::move(log));
		spdlog::set_level(spdlog::level::info);
		spdlog::flush_on(spdlog::level::info);
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	LoadConfig();
	LoadMapState();

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
		if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MapWatcher::GetSingleton());
				MapCameraHook::Install();
				ConfigReloadHook::Install();
				Menu::Register();  // all plugins are loaded by now, so the framework can be found
				logger::info("Blank Map ready");
			}
		}
	});
	return true;
}