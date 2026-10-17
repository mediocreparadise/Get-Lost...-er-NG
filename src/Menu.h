#pragma once

namespace Menu
{
	// Registers the Blank Map pages with SKSE Menu Framework (no-op if it isn't loaded).
	// Call after kDataLoaded: all plugins must be loaded before the framework can be found.
	void Register();
}
