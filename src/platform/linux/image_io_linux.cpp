// Linux: png_load and png_save (image_io.h) through stb_image (third_party/stb).
// PNG and JPEG (the menus' pictures), with straight alpha as WIC gives it.
#include "image_io.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <cstring>

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    int n = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return w > 0 && h > 0;
}

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}
