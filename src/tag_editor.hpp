// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "backend.hpp"

namespace ogg_input {
// The host owns the surrounding File Information window and broadcasts WM_USER
// field updates. This independently authored child handles only Advanced.
HWND CreateTagEditorPane(HINSTANCE module, HWND parent, const TrackInfo&, void (*saved)());
}
