#include <glad/glad.h>
#include "FontRenderer.h"
#include <iostream>

const char* vertexShaderSource = "#version 330 core\n"
    "layout (location = 0) in vec4 vertex;\n"
    "out vec2 TexCoords;\n"
    "uniform mat4 projection;\n"
    "void main() {\n"
    "    gl_Position = projection * vec4(vertex.xy, 0.0, 1.0);\n"
    "    TexCoords = vertex.zw;\n"
    "}";

const char* fragmentShaderSource = "#version 330 core\n"
    "in vec2 TexCoords;\n"
    "out vec4 color;\n"
    "uniform sampler2D text;\n"
    "uniform vec4 textColor;\n"
    "void main() {\n"
    "    float sampled = texture(text, TexCoords).r;\n"
    "    color = vec4(textColor.rgb, textColor.a * sampled);\n"
    "}";

FontRenderer::FontRenderer(const std::string& fontPath, int fontSize) {
    if (FT_Init_FreeType(&m_ft)) {
        std::cerr << "ERROR::FREETYPE: Could not init FreeType Library" << std::endl;
        return;
    }
    if (FT_New_Face(m_ft, fontPath.c_str(), 0, &m_face)) {
        std::cerr << "ERROR::FREETYPE: Failed to load font: " << fontPath << std::endl;
        return;
    }
    FT_Set_Pixel_Sizes(m_face, 0, fontSize);
    loadGlyphs();

    unsigned int vertex, fragment;
    vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vertexShaderSource, NULL);
    glCompileShader(vertex);

    fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragment);

    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertex);
    glAttachShader(m_shaderProgram, fragment);
    glLinkProgram(m_shaderProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 6 * 4, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

FontRenderer::~FontRenderer() {
    glDeleteProgram(m_shaderProgram);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    for (auto const& [key, val] : m_characters) {
        glDeleteTextures(1, &val.TextureID);
    }
    FT_Done_Face(m_face);
    FT_Done_FreeType(m_ft);
}

void FontRenderer::loadGlyphs() {
    m_characters.clear();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (unsigned char c = 0; c < 128; c++) {
        if (FT_Load_Char(m_face, c, FT_LOAD_RENDER)) continue;
        unsigned int texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
            m_face->glyph->bitmap.width, m_face->glyph->bitmap.rows,
            0, GL_RED, GL_UNSIGNED_BYTE, m_face->glyph->bitmap.buffer);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        Character character = {
            texture,
            (int)m_face->glyph->bitmap.width, (int)m_face->glyph->bitmap.rows,
            (int)m_face->glyph->bitmap_left, (int)m_face->glyph->bitmap_top,
            (unsigned int)m_face->glyph->advance.x
        };
        m_characters[c] = character;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FontRenderer::renderText(const std::string& text, float x, float y, float scale,
                               float r, float g, float b, float a) {
    glUseProgram(m_shaderProgram);
    glUniform4f(glGetUniformLocation(m_shaderProgram, "textColor"), r, g, b, a);

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    float width = (float)viewport[2];
    float height = (float)viewport[3];

    float projection[16] = {
        2.0f/width, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f/height, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };
    glUniformMatrix4fv(glGetUniformLocation(m_shaderProgram, "projection"), 1, GL_FALSE, projection);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_vao);

    for (char c_char : text) {
        auto it = m_characters.find(c_char);
        if (it == m_characters.end()) continue;
        Character ch = it->second;

        float xpos = x + ch.BearingX * scale;
        float ypos = y + (m_face->size->metrics.ascender >> 6) * scale - ch.BearingY * scale;
        float w = ch.SizeX * scale;
        float h = ch.SizeY * scale;

        float vertices[6][4] = {
            { xpos,     ypos + h, 0.0f, 1.0f },
            { xpos,     ypos,     0.0f, 0.0f },
            { xpos + w, ypos,     1.0f, 0.0f },
            { xpos,     ypos + h, 0.0f, 1.0f },
            { xpos + w, ypos,     1.0f, 0.0f },
            { xpos + w, ypos + h, 1.0f, 1.0f }
        };

        glBindTexture(GL_TEXTURE_2D, ch.TextureID);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        x += (ch.Advance >> 6) * scale;
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

float FontRenderer::measureText(const std::string& text, float scale) const {
    float width = 0.0f;
    for (char c : text) {
        auto it = m_characters.find(c);
        if (it != m_characters.end()) {
            width += (it->second.Advance >> 6) * scale;
        }
    }
    return width;
}
