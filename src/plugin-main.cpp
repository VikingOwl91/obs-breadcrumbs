/*
obs-breadcrumbs
Copyright (C) 2026 Christian Nachtigall <christian@nachtigall.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <plugin-support.h>
#include "breadcrumbs.hpp"
#include "breadcrumbs-config.hpp"
#if defined(__linux__)
#include "wayland-shortcuts.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

std::mutex g_mutex;

// Category names, protected by g_mutex. Sensible defaults for a game-run use case.
std::array<std::string, BREADCRUMBS_SLOTS> g_categories = {"Boss", "Death", "Loot", "Funny", "Mark"};

// Full path of the active recording, captured when recording starts. Empty when idle.
std::string g_record_path;

obs_hotkey_id g_hotkeys[BREADCRUMBS_SLOTS];

std::string config_path()
{
	char *p = obs_module_config_path("categories.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void load_config()
{
	std::string path = config_path();
	obs_data_t *data = obs_data_create_from_json_file(path.c_str());
	if (!data)
		return; // no config yet -> keep defaults

	obs_data_array_t *arr = obs_data_get_array(data, "categories");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		std::lock_guard<std::mutex> lock(g_mutex);
		for (size_t i = 0; i < BREADCRUMBS_SLOTS && i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			g_categories[i] = obs_data_get_string(item, "name");
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(data);
}

void save_config()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", g_categories[i].c_str());
			obs_data_array_push_back(arr, item);
			obs_data_release(item);
		}
	}
	obs_data_set_array(data, "categories", arr);

	std::string path = config_path();
	if (!obs_data_save_json(data, path.c_str()))
		obs_log(LOG_WARNING, "failed to save category config to %s", path.c_str());

	obs_data_array_release(arr);
	obs_data_release(data);
}

// --- hotkey binding persistence ---
//
// OBS's frontend does not reliably restore plugin-registered frontend hotkeys
// across restarts (it saves them to the profile, but doesn't re-apply them to
// our hotkeys on load). So we persist the bindings ourselves.

std::string hotkeys_path()
{
	char *p = obs_module_config_path("hotkeys.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save_hotkeys()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
		char name[32];
		snprintf(name, sizeof(name), "breadcrumbs.slot%zu", i + 1);
		obs_data_array_t *arr = obs_hotkey_save(g_hotkeys[i]);
		obs_data_set_array(data, name, arr);
		obs_data_array_release(arr);
	}
	obs_data_save_json(data, hotkeys_path().c_str());
	obs_data_release(data);
}

void load_hotkeys()
{
	obs_data_t *data = obs_data_create_from_json_file(hotkeys_path().c_str());
	if (!data)
		return;
	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
		char name[32];
		snprintf(name, sizeof(name), "breadcrumbs.slot%zu", i + 1);
		obs_data_array_t *arr = obs_data_get_array(data, name);
		if (arr) {
			obs_hotkey_load(g_hotkeys[i], arr);
			obs_data_array_release(arr);
		}
	}
	obs_data_release(data);
}

void on_frontend_save(obs_data_t *, bool saving, void *)
{
	if (saving)
		save_hotkeys();
}

// Turn "/foo/bar/run.mkv" into "/foo/bar/run.txt" (sidecar next to the recording).
std::string replace_ext_txt(const std::string &p)
{
	size_t slash = p.find_last_of("/\\");
	size_t dot = p.find_last_of('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		return p.substr(0, dot) + ".txt";
	return p + ".txt";
}

std::string sidecar_path()
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_record_path.empty())
			return replace_ext_txt(g_record_path);
	}
	// Fallback: we never saw a start event -> drop a generic file in the record folder.
	char *dir = obs_frontend_get_current_record_output_path();
	std::string s;
	if (dir && *dir) {
		s = dir;
		s += "/breadcrumbs.txt";
	}
	bfree(dir);
	return s;
}

void add_breadcrumb(size_t slot)
{
	if (slot >= BREADCRUMBS_SLOTS)
		return;

	if (!obs_frontend_recording_active()) {
		obs_log(LOG_INFO, "breadcrumb ignored: not recording");
		return;
	}

	// Frames written / FPS = position in the file. No frames advance while paused,
	// so this naturally matches the editor playhead even across pauses.
	obs_output_t *output = obs_frontend_get_recording_output();
	if (!output)
		return;
	int frames = obs_output_get_total_frames(output);
	obs_output_release(output);

	double fps = obs_get_active_fps();
	if (fps <= 0.0)
		fps = 30.0;

	int total = (int)((double)frames / fps);
	int h = total / 3600;
	int m = (total % 3600) / 60;
	int s = total % 60;

	char ts[16];
	snprintf(ts, sizeof(ts), "%02d:%02d:%02d", h, m, s);

	std::string category;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		category = g_categories[slot];
	}
	if (category.empty())
		category = "Marker " + std::to_string(slot + 1);

	std::string path = sidecar_path();
	if (path.empty()) {
		obs_log(LOG_WARNING, "breadcrumb dropped: could not determine recording path");
		return;
	}

	FILE *f = os_fopen(path.c_str(), "a");
	if (!f) {
		obs_log(LOG_WARNING, "breadcrumb dropped: could not open %s", path.c_str());
		return;
	}
	fprintf(f, "%s - %s\n", ts, category.c_str());
	fclose(f);

	obs_log(LOG_INFO, "breadcrumb: %s - %s -> %s", ts, category.c_str(), path.c_str());
}

void hotkey_pressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	add_breadcrumb((size_t)(uintptr_t)data);
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		std::string path;
		obs_output_t *output = obs_frontend_get_recording_output();
		if (output) {
			obs_data_t *settings = obs_output_get_settings(output);
			const char *p = obs_data_get_string(settings, "path");
			if (!p || !*p)
				p = obs_data_get_string(settings, "url");
			if (p)
				path = p;
			obs_data_release(settings);
			obs_output_release(output);
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		g_record_path = path;
		obs_log(LOG_INFO, "recording started: %s", g_record_path.c_str());
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_record_path.clear();
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Restore our hotkey bindings after OBS has finished its own load.
		load_hotkeys();
#if defined(__linux__)
		// On Wayland, OBS's own hotkeys can't fire while unfocused; register
		// global shortcuts via the desktop portal instead. No-op elsewhere.
		breadcrumbs_wayland_init();
#endif
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		save_hotkeys();
#if defined(__linux__)
		// Tear down the Qt/D-Bus portal object here, while the Qt app is still
		// alive. Doing it in obs_module_unload would run after Qt is destroyed
		// and crash OBS during shutdown.
		breadcrumbs_wayland_shutdown();
#endif
	}
}

void tools_menu_clicked(void *)
{
	breadcrumbs_open_config_dialog();
}

} // namespace

// --- shared accessors (declared in breadcrumbs.hpp) ---

std::array<std::string, BREADCRUMBS_SLOTS> breadcrumbs_get_categories()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_categories;
}

void breadcrumbs_set_categories(const std::array<std::string, BREADCRUMBS_SLOTS> &categories)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_categories = categories;
	}
	save_config();
}

void breadcrumbs_trigger_slot(size_t slot)
{
	add_breadcrumb(slot);
}

// --- module entry points ---

bool obs_module_load(void)
{
	load_config();

	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
		char name[32];
		char desc_key[32];
		snprintf(name, sizeof(name), "breadcrumbs.slot%zu", i + 1);
		snprintf(desc_key, sizeof(desc_key), "Breadcrumbs.Slot%zu", i + 1);
		g_hotkeys[i] = obs_hotkey_register_frontend(name, obs_module_text(desc_key), hotkey_pressed,
							    (void *)(uintptr_t)i);
	}

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_frontend_add_save_callback(on_frontend_save, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("Breadcrumbs.Menu"), tools_menu_clicked, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	// Note: the Wayland portal object is torn down on OBS_FRONTEND_EVENT_EXIT,
	// not here — by the time obs_module_unload runs, Qt is already gone.
	obs_frontend_remove_save_callback(on_frontend_save, nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++)
		obs_hotkey_unregister(g_hotkeys[i]);
	obs_log(LOG_INFO, "plugin unloaded");
}
