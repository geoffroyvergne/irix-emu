#pragma once

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION 1 // the legacy profile is what the bridge uses
#endif
#include <OpenGL/gl.h>

namespace indy {

// The internal format the host stores a guest texture in. A sized format of
// fewer than 8 bits per component (GL_RGB5, GL_RGBA4, ...) becomes its 8-bit
// equivalent; any other format is kept.
//
// A program asks for such formats to save texture memory: Quake III does
// with r_colorbits 16 (r_texturebits 0 follows it). GL lets an implementation
// store a texture at another resolution than asked for (an internal format
// is a request), and the host renders into RGBA8 buffers anyway. Kept at 4
// or 5 bits, a texture loses its low values: 8-bit data is truncated, so
// values below 16 (4 bits) or 8 (5 bits) become 0. Dark textures drawn
// additively, and lightmaps, then come out darker or not at all.
[[nodiscard]] constexpr GLint hostTextureFormat(GLint internalFormat) noexcept {
    switch (internalFormat) {
    case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5:
        return GL_RGB8;
    case GL_RGBA2: case GL_RGBA4: case GL_RGB5_A1:
        return GL_RGBA8;
    case GL_ALPHA4:
        return GL_ALPHA8;
    case GL_LUMINANCE4:
        return GL_LUMINANCE8;
    case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2:
        return GL_LUMINANCE8_ALPHA8;
    case GL_INTENSITY4:
        return GL_INTENSITY8;
    default:
        return internalFormat;
    }
}

} // namespace indy
