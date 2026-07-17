#pragma once

namespace panorama
{
// Process-wide diagnostic switches for expensive validation and A/B testing.
// Configure these before creating or updating UI surfaces; style-worker reads
// assume the configuration is not changed concurrently with a frame.
struct PanoramaDiagnostics
{
    bool tree_guard = false;
    bool disable_style_index = false;
    bool disable_style_sharing = false;
};

// Replaces the active diagnostic configuration. This gives embedders a
// deterministic alternative to mutating process environment variables.
void set_panorama_diagnostics(PanoramaDiagnostics diagnostics);

// Returns a snapshot of the active diagnostic configuration.
[[nodiscard]] PanoramaDiagnostics panorama_diagnostics();

// Reads the generic PanoramaEngine environment switches without changing the
// active configuration:
//   PANORAMA_TREE_GUARD
//   PANORAMA_DISABLE_STYLE_INDEX
//   PANORAMA_DISABLE_STYLE_SHARING
// A variable is enabled when it exists and is not empty, "0", "false", or
// "off" (case-insensitive).
[[nodiscard]] PanoramaDiagnostics panorama_diagnostics_from_environment();
}
