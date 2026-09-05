// src/version.h
#pragma once

// The plugin version string, reported in two places that must never drift
// from each other: GoldHEN's own plugin metadata (src/main.cpp, surfaced by
// GoldHEN itself) and the HELLO reply's "plugin=" field (src/net/dispatch.cpp),
// which the VS Code extension parses and displays directly in its status
// bar. Each of those files used to `#define` its own copy; on drift the
// editor would confidently display a version the plugin is not. One
// definition, included by both, makes that impossible.
#define PLUGIN_VERSION "0.3-m3"
