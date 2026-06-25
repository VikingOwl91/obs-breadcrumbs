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

// Linux/Wayland only. Registers the 5 breadcrumb slots as global shortcuts via
// the org.freedesktop.portal.GlobalShortcuts portal, so they fire even when OBS
// is unfocused or hidden (Wayland forbids the usual background key grabs OBS
// uses on X11). Activations route into breadcrumbs_trigger_slot().
//
// No-ops if not running under Wayland or if the portal is unavailable. Must be
// called from the Qt UI thread.
void breadcrumbs_wayland_init();
void breadcrumbs_wayland_shutdown();
