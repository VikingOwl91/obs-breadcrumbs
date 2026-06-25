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

#pragma once

#include <array>
#include <string>

// Number of category slots. Each slot has a user-defined name and a
// separately-bindable hotkey (configured in OBS Settings -> Hotkeys).
constexpr size_t BREADCRUMBS_SLOTS = 5;

// Thread-safe accessors shared between the core plugin and the Qt dialog.
std::array<std::string, BREADCRUMBS_SLOTS> breadcrumbs_get_categories();

// Replace all category names and persist them to the module config.
void breadcrumbs_set_categories(const std::array<std::string, BREADCRUMBS_SLOTS> &categories);

// Record a breadcrumb for the given slot (0-based), exactly as a hotkey press
// would. Used by alternative trigger paths such as the Wayland global-shortcuts
// portal. Safe to call from the Qt UI thread.
void breadcrumbs_trigger_slot(size_t slot);
