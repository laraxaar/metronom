#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <string>
#include <map>
#include <memory>

struct Character {
    unsigned int TextureID;
    int   SizeX, SizeY;
    int   BearingX, BearingY;
    unsigned int Advance;
};

class FontRenderer {
public:
    FontRenderer(const std::string& fontPath, int fontSize);
    ~FontRenderer();

    void renderText(const std::string& text, float x, float y, float scale,
                    float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

    /** @brief Measure text width at given scale without rendering. */
    float measureText(const std::string& text, float scale) const;

private:
    void loadGlyphs();

    FT_Library m_ft;
    FT_Face m_face;
    std::map<char, Character> m_characters;

    unsigned int m_shaderProgram;
    unsigned int m_vao, m_vbo;
};