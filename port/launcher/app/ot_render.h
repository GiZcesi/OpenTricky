/*
 * ot_render.h -- the launcher's glue between RmlUi and SDL3 + OpenGL 3:
 *  - textures: files (PNG, through SDL) and pictures made at run time,
 *    named "ot:<name>" in the markup (disc pictures, theme, icons);
 *  - files: read through SDL (UTF-8 paths); style sheets and documents get
 *    the theme's tokens ($acc$, $track$, ...) replaced as they are read;
 *  - icons drawn in code (no picture file needed).
 */
#ifndef OT_RENDER_H
#define OT_RENDER_H

#include <RmlUi/Core.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

/* RGBA bytes, straight alpha, rows top-down. */
struct OtPixels {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};

/* Makes the picture of an "ot:" texture name; false = no such picture. */
using OtTextureMaker = std::function<bool(const std::string &name, OtPixels &out)>;

class OtRenderInterface : public RenderInterface_GL3 {
public:
    OtTextureMaker maker;
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override;
};

class OtSystemInterface : public SystemInterface_SDL {
public:
    explicit OtSystemInterface(SDL_Window *w) : SystemInterface_SDL(w) {}
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override;
};

class OtFileInterface : public Rml::FileInterface {
public:
    /* "$name$" -> value, applied to *.rcss and *.rml files. */
    std::map<std::string, std::string> tokens;

    Rml::FileHandle Open(const Rml::String &path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;
};

/* Sidebar icons, white on transparent, `size` pixels square:
 * "home", "settings", "update", "online", "bug". */
bool ot_draw_icon(const std::string &name, int size, OtPixels &out);

#endif /* OT_RENDER_H */
