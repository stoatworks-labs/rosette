#pragma once

/**
    The passes, as GLSL source.

    Two fragment shaders and one vertex shader, in the order they run:

    1. **separate**  picture size, mipmapped. RGB to CMYK per pixel, with
                     the black generation and the total ink limit, weighted
                     by alpha so a transparent pixel carries no ink. The mip
                     chain on this buffer is what lets the print pass sample
                     the tone of a whole cell in one fetch.
    2. **print**     output size, straight to the host's framebuffer. Four
                     lattices, four dots, the ink model, the paper, Mix.

    `kSpotLibrary` is the spot functions, a fragment rather than a shader:
    no `#version`, no `main`. `PrintShaderSource()` assembles the print pass
    around it and `SpotProbeShaderSource()` assembles the harness's probe
    around the same string, so `rztest --spot` runs the text the plugin runs.
    A test that compiled its own transcription of the spot functions would
    agree with itself perfectly and prove nothing.
*/

#include <string>

namespace rosette
{

extern const char* const kVertexShader;
extern const char* const kSeparateShader;

/// The spot functions, as GLSL. Not a complete shader.
extern const char* const kSpotLibrary;

/// The print pass, assembled around kSpotLibrary.
std::string PrintShaderSource();

/// One cell point per pixel across a square target, writing `spot()` for the
/// shape in the `Shape` uniform to red, so it can be read straight back and
/// compared against `Screen.cpp`. Exists only for `rztest --spot`.
std::string SpotProbeShaderSource();

} // namespace rosette
