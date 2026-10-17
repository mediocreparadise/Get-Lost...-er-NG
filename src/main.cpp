// Blank Map - hide world map markers, recenter map cameras, hide compass.
// SKSE / CommonLibSSE-NG. Original implementation.
//
// Settings: Data/SKSE/Plugins/BlankMap.ini, then overridden by BlankMap_Menu.ini
// (written by the in-game SKSE Menu Framework page). Both are re-checked every
// couple of seconds, so changes apply live.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
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

		static void OnFrame(RE::MapMenu* a_menu)
		{
			if (g_pendingFrames.load() <= 0) return;

			auto* rd = a_menu->GetRuntimeData2();  // null on VR
			if (!rd || !rd->controlsReady) return;

			bool  recenter, useFixed, localRecenter;
			float fixedX, fixedY;
			{
				std::lock_guard lk(g_configMutex);
				recenter = g_config.recenter;
				useFixed = g_config.useFixed;
				localRecenter = g_config.localRecenter;
				fixedX = g_config.fixedX;
				fixedY = g_config.fixedY;
			}

			if (recenter) {
				for (auto& state : rd->camera.unk68) {
					auto* world = state.get();
					if (!world) continue;

					float x = fixedX;
					float y = fixedY;
					if (!useFixed) {
						if (!world->mapData) continue;
						x = (world->mapData->minimumCoordinates.x + world->mapData->maximumCoordinates.x) * 0.5f;
						y = (world->mapData->minimumCoordinates.y + world->mapData->maximumCoordinates.y) * 0.5f;
					}

					if (!g_loggedCamera) {
						logger::info("Camera before: pos=({:.1f},{:.1f},{:.1f}) -> target=({:.1f},{:.1f}); transition active={}",
							world->currentPosition.x, world->currentPosition.y, world->currentPosition.z,
							x, y, rd->camera.unk80.get() != nullptr);
						if (world->mapData) {
							logger::info("Map bounds: min=({:.1f},{:.1f}) max=({:.1f},{:.1f})",
								world->mapData->minimumCoordinates.x, world->mapData->minimumCoordinates.y,
								world->mapData->maximumCoordinates.x, world->mapData->maximumCoordinates.y);
						}
					}
					world->currentPosition.x = x;
					world->currentPosition.y = y;
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

	// ---- HUD: compass hiding + live config reload -----------------------
	std::uint32_t g_hudFrames{ 0 };
	bool          g_compassHidden{ false };

	struct HudHook
	{
		static void Thunk(RE::HUDMenu* a_this, float a_interval, std::uint32_t a_time)
		{
			func(a_this, a_interval, a_time);
			OnFrame(a_this);
		}

		static inline REL::Relocation<decltype(Thunk)> func;

		static void OnFrame(RE::HUDMenu* a_menu)
		{
			++g_hudFrames;
			if (g_hudFrames % 120 == 0) MaybeReloadConfig();  // ~2s: pick up INI edits
			if (g_hudFrames % 30 != 0) return;
			if (!a_menu->uiMovie) return;

			bool hide;
			{
				std::lock_guard lk(g_configMutex);
				hide = g_config.hideCompass;
			}
			const std::string path = CompassPath() + "._visible";
			if (hide) {
				a_menu->uiMovie->SetVariable(path.c_str(), RE::GFxValue(false));
				g_compassHidden = true;
			} else if (g_compassHidden) {
				a_menu->uiMovie->SetVariable(path.c_str(), RE::GFxValue(true));
				g_compassHidden = false;
			}
		}

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
			func = vtbl.write_vfunc(0x5, Thunk);  // IMenu::AdvanceMovie
			logger::info("HUD hook installed");
		}
	};

	// ---- Map marker hiding ----------------------------------------------
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

			bool logDecisions;
			{
				std::lock_guard lk(g_configMutex);
				logDecisions = g_config.logDecisions;
			}

			std::size_t hidden = 0;
			for (auto& handle : player->GetPlayerRuntimeData().currentMapMarkers) {
				auto ref = handle.get();
				if (!ref) continue;
				auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				if (!extra || !extra->mapData) continue;
				if (!ShouldHide(extra->mapData)) continue;

				_saved.push_back({ handle, static_cast<std::uint8_t>(extra->mapData->flags.underlying()) });
				extra->mapData->flags.reset(RE::MapMarkerData::Flag::kVisible, RE::MapMarkerData::Flag::kShowAllHidden);
				++hidden;
				if (logDecisions) {
					logger::info("hid '{}' (type {})", extra->mapData->locationName.GetFullName(),
						static_cast<std::uint16_t>(extra->mapData->type.get()));
				}
			}
			logger::info("Map opened: hid {} markers", hidden);
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

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
		if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MapWatcher::GetSingleton());
				MapCameraHook::Install();
				HudHook::Install();
				Menu::Register();  // all plugins are loaded by now, so the framework can be found
				logger::info("Blank Map ready");
			}
		}
	});
	return true;
}
