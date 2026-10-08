/*
 * ot_render.cpp -- see ot_render.h.
 */
#include "ot_render.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

/* ── Textures ──────────────────────────────────────────────────────── */

static Rml::TextureHandle upload(RenderInterface_GL3 *ri, OtPixels &p, Rml::Vector2i &dim)
{
    /* RmlUi composites with premultiplied alpha. */
    for (size_t i = 0; i + 3 < p.rgba.size(); i += 4) {
        unsigned a = p.rgba[i + 3];
        for (int c = 0; c < 3; c++) p.rgba[i + c] = (unsigned char)((p.rgba[i + c] * a + 127) / 255);
    }
    dim = { p.w, p.h };
    return ri->RenderInterface_GL3::GenerateTexture({ p.rgba.data(), p.rgba.size() }, dim);
}

Rml::TextureHandle OtRenderInterface::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source)
{
    OtPixels p;
    if (source.compare(0, 3, "ot:") == 0) {
        if (!maker || !maker(source.substr(3), p) || p.w <= 0 || p.h <= 0) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "No picture for '%s'", source.c_str());
            return {};
        }
        return upload(this, p, dimensions);
    }
    SDL_Surface *s = SDL_LoadPNG(source.c_str());
    if (!s) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Cannot read picture '%s': %s", source.c_str(), SDL_GetError());
        return {};
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) return {};
    p.w = c->w;
    p.h = c->h;
    p.rgba.resize((size_t)p.w * p.h * 4);
    for (int y = 0; y < p.h; y++)
        memcpy(&p.rgba[(size_t)y * p.w * 4], (unsigned char *)c->pixels + (size_t)y * c->pitch, (size_t)p.w * 4);
    SDL_DestroySurface(c);
    return upload(this, p, dimensions);
}

/* ── Log ───────────────────────────────────────────────────────────── */

bool OtSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String &message)
{
    static const char *const names[] = { "always", "error", "assert", "warning", "info", "debug" };
    int t = (int)type;
    fprintf(stderr, "[rmlui %s] %s\n", (t >= 0 && t < 6) ? names[t] : "?", message.c_str());
    fflush(stderr);
    return true;
}

/* ── Files ─────────────────────────────────────────────────────────── */

namespace {
struct MemFile {
    std::string data;
    size_t pos = 0;
};
}

Rml::FileHandle OtFileInterface::Open(const Rml::String &path)
{
    size_t n = 0;
    void *d = SDL_LoadFile(path.c_str(), &n);
    if (!d) return 0;
    MemFile *f = new MemFile;
    f->data.assign((const char *)d, n);
    SDL_free(d);
    auto ends = [&](const char *e) {
        size_t n = strlen(e);
        return path.size() > n && path.compare(path.size() - n, n, e) == 0;
    };
    if (ends(".rcss") || ends(".rml")) {
        for (const auto &kv : tokens) {
            size_t at = 0;
            while ((at = f->data.find(kv.first, at)) != std::string::npos) {
                f->data.replace(at, kv.first.size(), kv.second);
                at += kv.second.size();
            }
        }
    }
    return (Rml::FileHandle)f;
}

void OtFileInterface::Close(Rml::FileHandle file) { delete (MemFile *)file; }

size_t OtFileInterface::Read(void *buffer, size_t size, Rml::FileHandle file)
{
    MemFile *f = (MemFile *)file;
    size_t n = std::min(size, f->data.size() - f->pos);
    memcpy(buffer, f->data.data() + f->pos, n);
    f->pos += n;
    return n;
}

bool OtFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
{
    MemFile *f = (MemFile *)file;
    long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (long)f->pos : (long)f->data.size();
    long p = base + offset;
    if (p < 0 || p > (long)f->data.size()) return false;
    f->pos = (size_t)p;
    return true;
}

size_t OtFileInterface::Tell(Rml::FileHandle file) { return ((MemFile *)file)->pos; }
size_t OtFileInterface::Length(Rml::FileHandle file) { return ((MemFile *)file)->data.size(); }

/* ── Icons ─────────────────────────────────────────────────────────
 * Drawn as 2-unit strokes on a 24 x 24 grid (the usual line-icon style),
 * antialiased with 4 x 4 samples per pixel. */

namespace {
struct V { float x, y; };
struct Shape {
    int kind;            /* 0 segment a-b, 1 ellipse at a with radii b, 2 filled disc at a radius b.x */
    V a, b;
};

float seg_dist(V p, V a, V b)
{
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
    float t = l > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

float shape_cover(const Shape &s, V p)
{
    const float half = 1.0f;
    if (s.kind == 0) return seg_dist(p, s.a, s.b) <= half ? 1.0f : 0.0f;
    float nx = (p.x - s.a.x) / s.b.x, ny = (p.y - s.a.y) / (s.kind == 2 ? s.b.x : s.b.y);
    float r = std::sqrt(nx * nx + ny * ny);
    if (s.kind == 2) return r <= 1.0f ? 1.0f : 0.0f;
    float d = std::fabs(r - 1.0f) * std::min(s.b.x, s.b.y);
    return d <= half ? 1.0f : 0.0f;
}

void poly(std::vector<Shape> &v, std::initializer_list<V> pts)
{
    const V *prev = nullptr;
    for (const V &p : pts) {
        if (prev) v.push_back({ 0, *prev, p });
        prev = &p;
    }
}
}

bool ot_draw_icon(const std::string &name, int size, OtPixels &out)
{
    std::vector<Shape> sh;
    if (name == "home") {
        poly(sh, { { 3, 11 }, { 12, 3.5f }, { 21, 11 } });
        poly(sh, { { 5.5f, 9.5f }, { 5.5f, 20.5f }, { 18.5f, 20.5f }, { 18.5f, 9.5f } });
        poly(sh, { { 10, 20.5f }, { 10, 14.5f }, { 14, 14.5f }, { 14, 20.5f } });
    } else if (name == "settings") {
        sh.push_back({ 1, { 12, 12 }, { 3.6f, 3.6f } });
        for (int i = 0; i < 8; i++) {
            float a = i * 3.14159265f / 4;
            sh.push_back({ 0, { 12 + 7.2f * std::cos(a), 12 + 7.2f * std::sin(a) },
                              { 12 + 9.2f * std::cos(a), 12 + 9.2f * std::sin(a) } });
        }
    } else if (name == "update") {
        poly(sh, { { 12, 3.5f }, { 12, 15 } });
        poly(sh, { { 7, 10.5f }, { 12, 15.5f }, { 17, 10.5f } });
        poly(sh, { { 4.5f, 20 }, { 19.5f, 20 } });
    } else if (name == "online") {
        sh.push_back({ 1, { 12, 12 }, { 9, 9 } });
        sh.push_back({ 1, { 12, 12 }, { 4, 9 } });
        poly(sh, { { 3, 12 }, { 21, 12 } });
    } else if (name == "bug") {
        sh.push_back({ 1, { 12, 14.5f }, { 5, 6 } });
        sh.push_back({ 1, { 12, 6.2f }, { 2.6f, 2.2f } });
        poly(sh, { { 12, 9 }, { 12, 20 } });
        poly(sh, { { 7, 12 }, { 3.5f, 10 } });
        poly(sh, { { 7, 16.5f }, { 3.5f, 18 } });
        poly(sh, { { 17, 12 }, { 20.5f, 10 } });
        poly(sh, { { 17, 16.5f }, { 20.5f, 18 } });
    } else {
        return false;
    }
    out.w = out.h = size;
    out.rgba.assign((size_t)size * size * 4, 0);
    const int ss = 4;
    float k = 24.0f / size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int hit = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    V p = { (x + (sx + 0.5f) / ss) * k, (y + (sy + 0.5f) / ss) * k };
                    for (const Shape &s : sh)
                        if (shape_cover(s, p) > 0) { hit++; break; }
                }
            unsigned char *o = &out.rgba[((size_t)y * size + x) * 4];
            o[0] = o[1] = o[2] = 255;
            o[3] = (unsigned char)(hit * 255 / (ss * ss));
        }
    return true;
}
