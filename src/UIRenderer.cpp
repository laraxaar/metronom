#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "UIRenderer.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =====================================================================
// Geometry Shaders (GL 3.3 Core — no glBegin/glEnd)
// =====================================================================
static const char* geoVertSrc = "#version 330 core\n"
    "layout (location = 0) in vec2 aPos;\n"
    "uniform mat4 projection;\n"
    "void main() {\n"
    "    gl_Position = projection * vec4(aPos, 0.0, 1.0);\n"
    "}";

static const char* geoFragSrc = "#version 330 core\n"
    "out vec4 FragColor;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "    FragColor = uColor;\n"
    "}";

// =====================================================================
// Color palette — premium dark theme
// =====================================================================
namespace Color {
    // Backgrounds
    constexpr float BG[]         = {0.06f, 0.06f, 0.09f};
    constexpr float Panel[]      = {0.10f, 0.10f, 0.14f};
    constexpr float PanelLight[] = {0.14f, 0.14f, 0.19f};
    constexpr float Card[]       = {0.12f, 0.12f, 0.17f};
    constexpr float CardHover[]  = {0.16f, 0.16f, 0.22f};

    // Accent colors
    constexpr float Accent[]     = {0.30f, 0.55f, 1.00f};  // Bright blue
    constexpr float AccentDim[]  = {0.15f, 0.30f, 0.60f};
    constexpr float Green[]      = {0.20f, 0.85f, 0.45f};
    constexpr float Orange[]     = {1.00f, 0.60f, 0.15f};
    constexpr float Red[]        = {0.95f, 0.25f, 0.25f};
    constexpr float Yellow[]     = {1.00f, 0.85f, 0.20f};
    constexpr float Purple[]     = {0.60f, 0.35f, 0.95f};

    // Text
    constexpr float TextBright[] = {0.95f, 0.95f, 0.97f};
    constexpr float TextDim[]    = {0.50f, 0.50f, 0.58f};
    constexpr float TextMuted[]  = {0.35f, 0.35f, 0.40f};
}

// =====================================================================
// Constructor / Destructor
// =====================================================================

UIRenderer::UIRenderer(GLFWwindow* window) : m_window(window) {
    m_fontRenderer = std::make_unique<FontRenderer>("C:/Windows/Fonts/segoeui.ttf", 48);
    initShaders();
}

UIRenderer::~UIRenderer() {
    if (m_geoShader) glDeleteProgram(m_geoShader);
    if (m_geoVAO)    glDeleteVertexArrays(1, &m_geoVAO);
    if (m_geoVBO)    glDeleteBuffers(1, &m_geoVBO);
}

void UIRenderer::initShaders() {
    unsigned int vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &geoVertSrc, NULL);
    glCompileShader(vert);

    unsigned int frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &geoFragSrc, NULL);
    glCompileShader(frag);

    m_geoShader = glCreateProgram();
    glAttachShader(m_geoShader, vert);
    glAttachShader(m_geoShader, frag);
    glLinkProgram(m_geoShader);
    glDeleteShader(vert);
    glDeleteShader(frag);

    glGenVertexArrays(1, &m_geoVAO);
    glGenBuffers(1, &m_geoVBO);
}

// =====================================================================
// Frame setup
// =====================================================================

void UIRenderer::newFrame() {
    int width, height;
    glfwGetFramebufferSize(m_window, &width, &height);
    glViewport(0, 0, width, height);
}

// =====================================================================
// Geometry drawing (GL 3.3 Core)
// =====================================================================

void UIRenderer::drawRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    int vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float sw = (float)vp[2], sh = (float)vp[3];

    float projection[16] = {
        2.0f/sw, 0, 0, 0,
        0, -2.0f/sh, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    };

    glUseProgram(m_geoShader);
    glUniformMatrix4fv(glGetUniformLocation(m_geoShader, "projection"), 1, GL_FALSE, projection);
    glUniform4f(glGetUniformLocation(m_geoShader, "uColor"), r, g, b, a);

    float verts[] = {
        x,     y,
        x + w, y,
        x + w, y + h,
        x,     y,
        x + w, y + h,
        x,     y + h
    };

    glBindVertexArray(m_geoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_geoVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void UIRenderer::drawRoundedRect(float x, float y, float w, float h, float radius, float r, float g, float b, float a) {
    int vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float sw = (float)vp[2], sh = (float)vp[3];

    float projection[16] = {
        2.0f/sw, 0, 0, 0,
        0, -2.0f/sh, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    };

    glUseProgram(m_geoShader);
    glUniformMatrix4fv(glGetUniformLocation(m_geoShader, "projection"), 1, GL_FALSE, projection);
    glUniform4f(glGetUniformLocation(m_geoShader, "uColor"), r, g, b, a);

    const int segs = 8;
    std::vector<float> verts;
    verts.reserve((segs * 4 + 2) * 2 * 3);  // triangle fan as triangles

    // Center point
    float cx = x + w / 2.0f;
    float cy = y + h / 2.0f;

    // Generate rounded rect vertices as triangle fan from center
    auto addCorner = [&](float cornerX, float cornerY, float startAngle) {
        for (int i = 0; i < segs; ++i) {
            float a1 = startAngle + (float)i * (float)(M_PI / 2.0) / segs;
            float a2 = startAngle + (float)(i + 1) * (float)(M_PI / 2.0) / segs;
            verts.push_back(cx); verts.push_back(cy);
            verts.push_back(cornerX + cosf(a1) * radius); verts.push_back(cornerY + sinf(a1) * radius);
            verts.push_back(cornerX + cosf(a2) * radius); verts.push_back(cornerY + sinf(a2) * radius);
        }
    };

    // Four corners
    addCorner(x + radius,     y + radius,     (float)M_PI);             // top-left
    addCorner(x + w - radius, y + radius,     (float)(M_PI * 1.5));     // top-right
    addCorner(x + w - radius, y + h - radius, 0.0f);                   // bottom-right
    addCorner(x + radius,     y + h - radius, (float)(M_PI * 0.5));    // bottom-left

    // Fill the rectangular areas between corners
    // Top
    float tv[] = { cx, cy, x + radius, y, x + w - radius, y,
                   cx, cy, x + w - radius, y, x + w - radius, y + radius,
                   cx, cy, x + w - radius, y + radius, x + w, y + radius };
    for (float v : tv) verts.push_back(v);

    // Right  
    float rv[] = { cx, cy, x + w, y + radius, x + w, y + h - radius,
                   cx, cy, x + w, y + h - radius, x + w - radius, y + h - radius };
    for (float v : rv) verts.push_back(v);

    // Bottom
    float bv[] = { cx, cy, x + w - radius, y + h, x + radius, y + h,
                   cx, cy, x + radius, y + h, x + radius, y + h - radius };
    for (float v : bv) verts.push_back(v);

    // Left
    float lv[] = { cx, cy, x, y + h - radius, x, y + radius,
                   cx, cy, x, y + radius, x + radius, y + radius };
    for (float v : lv) verts.push_back(v);

    // Central fill
    float cf[] = { cx, cy, x + radius, y + radius, x + w - radius, y + radius,
                   cx, cy, x + w - radius, y + radius, x + w - radius, y + h - radius,
                   cx, cy, x + w - radius, y + h - radius, x + radius, y + h - radius,
                   cx, cy, x + radius, y + h - radius, x + radius, y + radius };
    for (float v : cf) verts.push_back(v);

    glBindVertexArray(m_geoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_geoVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glDrawArrays(GL_TRIANGLES, 0, (int)(verts.size() / 2));
    glBindVertexArray(0);
}

void UIRenderer::drawCircle(float cx, float cy, float radius, float r, float g, float b, float a) {
    int vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float sw = (float)vp[2], sh = (float)vp[3];

    float projection[16] = {
        2.0f/sw, 0, 0, 0,
        0, -2.0f/sh, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    };

    glUseProgram(m_geoShader);
    glUniformMatrix4fv(glGetUniformLocation(m_geoShader, "projection"), 1, GL_FALSE, projection);
    glUniform4f(glGetUniformLocation(m_geoShader, "uColor"), r, g, b, a);

    const int segs = 32;
    std::vector<float> verts;
    for (int i = 0; i < segs; ++i) {
        float a1 = 2.0f * (float)M_PI * i / segs;
        float a2 = 2.0f * (float)M_PI * (i + 1) / segs;
        verts.push_back(cx); verts.push_back(cy);
        verts.push_back(cx + cosf(a1) * radius); verts.push_back(cy + sinf(a1) * radius);
        verts.push_back(cx + cosf(a2) * radius); verts.push_back(cy + sinf(a2) * radius);
    }

    glBindVertexArray(m_geoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_geoVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glDrawArrays(GL_TRIANGLES, 0, (int)(verts.size() / 2));
    glBindVertexArray(0);
}

// =====================================================================
// UI Helpers
// =====================================================================

bool UIRenderer::isHovered(float x, float y, float w, float h, const UIState& state) const {
    return state.mouseX > x && state.mouseX < x + w && state.mouseY > y && state.mouseY < y + h;
}

void UIRenderer::drawText(float x, float y, const std::string& text, float size, float r, float g, float b, float a) {
    m_fontRenderer->renderText(text, x, y, size, r, g, b, a);
}

void UIRenderer::drawTextCentered(float cx, float y, const std::string& text, float size, float r, float g, float b, float a) {
    float tw = m_fontRenderer->measureText(text, size);
    m_fontRenderer->renderText(text, cx - tw / 2.0f, y, size, r, g, b, a);
}

bool UIRenderer::drawButton(float x, float y, float w, float h, const std::string& text, bool active, const UIState& state, float fontSize) {
    bool hovered = isHovered(x, y, w, h, state);

    if (active) {
        drawRoundedRect(x, y, w, h, 6, Color::Accent[0], Color::Accent[1], Color::Accent[2], 0.9f);
    } else if (hovered) {
        drawRoundedRect(x, y, w, h, 6, Color::CardHover[0], Color::CardHover[1], Color::CardHover[2]);
    } else {
        drawRoundedRect(x, y, w, h, 6, Color::Card[0], Color::Card[1], Color::Card[2]);
    }

    // Center text
    float tw = m_fontRenderer->measureText(text, fontSize);
    float tx = x + (w - tw) / 2.0f;
    float ty = y + h / 2.0f - fontSize * 24.0f * 0.4f;
    drawText(tx, ty, text, fontSize,
        active ? 1.0f : (hovered ? 0.9f : 0.75f),
        active ? 1.0f : (hovered ? 0.9f : 0.75f),
        active ? 1.0f : (hovered ? 0.95f : 0.80f));

    return hovered && state.mousePressed;
}

bool UIRenderer::drawSmallButton(float x, float y, float w, float h, const std::string& text, bool active, const UIState& state) {
    return drawButton(x, y, w, h, text, active, state, 0.28f);
}

void UIRenderer::drawFader(float x, float y, float w, float h, float value, float peak, const std::string& label, const UIState& state, float r, float g, float b) {
    // Track background
    drawRoundedRect(x, y, w, h, 4, Color::Panel[0], Color::Panel[1], Color::Panel[2]);

    // Filled portion (bottom-up)
    float fillH = h * value;
    float fy = y + h - fillH;
    drawRoundedRect(x + 2, fy, w - 4, fillH - 2, 3, r, g, b, 0.7f);

    // Peak indicator line
    float peakY = y + h * (1.0f - peak);
    if (peak > 0.01f) {
        drawRect(x + 1, peakY, w - 2, 2,
            peak > 0.9f ? 1.0f : (peak > 0.6f ? 1.0f : r),
            peak > 0.9f ? 0.2f : (peak > 0.6f ? 0.7f : g),
            peak > 0.9f ? 0.2f : (peak > 0.6f ? 0.1f : b));
    }

    // Label below
    drawTextCentered(x + w / 2.0f, y + h + 6, label, 0.22f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
}

void UIRenderer::drawPeakMeter(float x, float y, float w, float h, float level) {
    drawRoundedRect(x, y, w, h, 2, 0.08f, 0.08f, 0.10f);

    int numLeds = 12;
    float ledH = (h - 4) / numLeds;
    float gap = 2.0f;

    for (int i = 0; i < numLeds; ++i) {
        float ledY = y + h - (i + 1) * ledH - 2;
        float threshold = (float)(i + 1) / numLeds;
        bool lit = level >= threshold;

        float r, g, b;
        if (i >= 10)      { r = 0.95f; g = 0.15f; b = 0.15f; }  // red
        else if (i >= 7)  { r = 1.00f; g = 0.75f; b = 0.10f; }  // yellow
        else              { r = 0.15f; g = 0.80f; b = 0.35f; }  // green

        if (!lit) { r *= 0.15f; g *= 0.15f; b *= 0.15f; }

        drawRect(x + 2, ledY, w - 4, ledH - gap, r, g, b);
    }
}

// =====================================================================
// Main Render
// =====================================================================

UIEvent UIRenderer::render(const UIState& state) {
    UIEvent event;

    glClearColor(Color::BG[0], Color::BG[1], Color::BG[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float W = (float)state.windowWidth;
    float H = (float)state.windowHeight;

    // --- TOP NAVIGATION BAR ---
    float navH = 48;
    drawRect(0, 0, W, navH, Color::Panel[0], Color::Panel[1], Color::Panel[2]);

    // App title
    drawText(16, 10, "M E T R O N O M", 0.38f, Color::Accent[0], Color::Accent[1], Color::Accent[2]);

    // Tab buttons
    const char* tabs[] = { "METRONOME", "TUNER", "MIXER" };
    float tabX = 260;
    for (int i = 0; i < 3; ++i) {
        float tw = 120;
        bool active = (m_activeTab == i);
        bool hovered = isHovered(tabX, 0, tw, navH, state);

        if (active) {
            drawRect(tabX, navH - 3, tw, 3, Color::Accent[0], Color::Accent[1], Color::Accent[2]);
        }

        drawTextCentered(tabX + tw / 2, 14, tabs[i], 0.28f,
            active ? Color::TextBright[0] : (hovered ? 0.7f : Color::TextDim[0]),
            active ? Color::TextBright[1] : (hovered ? 0.7f : Color::TextDim[1]),
            active ? Color::TextBright[2] : (hovered ? 0.7f : Color::TextDim[2]));

        if (hovered && state.mousePressed) {
            m_activeTab = i;
        }
        tabX += tw + 8;
    }

    // CPU load
    std::ostringstream cpuStr;
    cpuStr << "CPU " << std::fixed << std::setprecision(0) << state.cpuLoad << "%";
    drawText(W - 100, 14, cpuStr.str(), 0.24f, Color::TextMuted[0], Color::TextMuted[1], Color::TextMuted[2]);

    // --- CONTENT AREA ---
    float contentY = navH + 12;
    float contentH = H - contentY - 12;
    float pad = 16;

    UIEvent headerEvt = renderHeader(state, pad, contentY, W - pad * 2);
    if (headerEvt.type != UIEventType::None) event = headerEvt;

    float sectionY = contentY + 140;
    float sectionH = contentH - 140;

    if (m_activeTab == 0) {
        UIEvent matrixEvt = renderMatrix(state, pad, sectionY, W - pad * 2, sectionH);
        if (matrixEvt.type != UIEventType::None) event = matrixEvt;
    } else if (m_activeTab == 1) {
        UIEvent tunerEvt = renderTuner(state, pad, sectionY, W - pad * 2, sectionH);
        if (tunerEvt.type != UIEventType::None) event = tunerEvt;
    } else if (m_activeTab == 2) {
        UIEvent mixerEvt = renderMixer(state, pad, sectionY, W - pad * 2, sectionH);
        if (mixerEvt.type != UIEventType::None) event = mixerEvt;
    }

    return event;
}

// =====================================================================
// Header: BPM display, Play/Stop, TAP, Time Sig, Subdivision
// =====================================================================

UIEvent UIRenderer::renderHeader(const UIState& state, float x, float y, float w) {
    UIEvent event;

    // BPM panel background
    drawRoundedRect(x, y, w, 120, 10, Color::Panel[0], Color::Panel[1], Color::Panel[2]);

    // --- BPM Display ---
    std::ostringstream bpmStr;
    bpmStr << std::fixed << std::setprecision(1) << state.bpm;
    drawText(x + 24, y + 12, bpmStr.str(), 1.4f, Color::TextBright[0], Color::TextBright[1], Color::TextBright[2]);
    drawText(x + 24 + m_fontRenderer->measureText(bpmStr.str(), 1.4f) + 8, y + 40, "BPM", 0.4f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    // --- BPM +/- buttons ---
    float btnX = x + 280;
    float btnY = y + 16;
    if (drawSmallButton(btnX, btnY, 42, 38, "-1", false, state)) {
        event.type = UIEventType::BpmChange; event.value = -1.0f;
    }
    if (drawSmallButton(btnX + 48, btnY, 42, 38, "+1", false, state)) {
        event.type = UIEventType::BpmChange; event.value = 1.0f;
    }
    if (drawSmallButton(btnX, btnY + 46, 42, 38, "-5", false, state)) {
        event.type = UIEventType::BpmChange; event.value = -5.0f;
    }
    if (drawSmallButton(btnX + 48, btnY + 46, 42, 38, "+5", false, state)) {
        event.type = UIEventType::BpmChange; event.value = 5.0f;
    }

    // --- TAP button ---
    float tapX = btnX + 110;
    if (drawButton(tapX, y + 18, 90, 82, "TAP", false, state, 0.45f)) {
        event.type = UIEventType::TapTempo;
    }

    // --- PLAY / STOP button ---
    float playX = tapX + 110;
    if (drawButton(playX, y + 18, 120, 82, state.playing ? "STOP" : "PLAY", state.playing, state, 0.45f)) {
        event.type = UIEventType::TogglePlay;
    }

    // --- Time Signature ---
    float tsX = playX + 140;
    drawText(tsX, y + 10, "TIME SIG", 0.22f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    const int sigs[] = {3, 4, 5, 6, 7};
    for (int i = 0; i < 5; ++i) {
        std::string label = std::to_string(sigs[i]) + "/4";
        if (drawSmallButton(tsX + i * 52, y + 34, 46, 32, label, state.timeSigTop == sigs[i], state)) {
            event.type = UIEventType::TimeSigChange;
            event.intValue = sigs[i];
        }
    }

    // --- Subdivision ---
    drawText(tsX, y + 74, "GRID", 0.22f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    const int subs[] = {4, 8, 12, 16, 20};
    const char* subLabels[] = {"1/4", "1/8", "TRI", "1/16", "QUIN"};
    for (int i = 0; i < 5; ++i) {
        if (drawSmallButton(tsX + i * 52, y + 88, 46, 24, subLabels[i], state.subdivision == subs[i], state)) {
            event.type = UIEventType::SubdivisionChange;
            event.intValue = subs[i];
        }
    }

    // --- Score / Accuracy ---
    float accX = w - 180;
    drawText(x + accX, y + 14, "SCORE", 0.22f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    drawText(x + accX, y + 32, state.scoreRank, 1.0f, Color::Green[0], Color::Green[1], Color::Green[2]);
    std::ostringstream accStr;
    accStr << std::fixed << std::setprecision(0) << state.accuracy << "%";
    drawText(x + accX + 80, y + 56, accStr.str(), 0.32f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    return event;
}

// =====================================================================
// Matrix: Step grid with playhead
// =====================================================================

UIEvent UIRenderer::renderMatrix(const UIState& state, float x, float y, float w, float h) {
    UIEvent event;

    drawRoundedRect(x, y, w, (std::min)(h, 200.0f), 10, Color::Panel[0], Color::Panel[1], Color::Panel[2]);

    drawText(x + 16, y + 12, "RHYTHM GRID", 0.26f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    int numSteps = state.gridNumSteps;
    if (numSteps < 1) numSteps = 4;
    if (numSteps > 32) numSteps = 32;

    float gridY = y + 44;
    float gridH = 100;
    float totalPad = 44 + numSteps * 4;  // 4px gap between steps
    float stepW = (w - totalPad) / numSteps;
    if (stepW > 70) stepW = 70;
    if (stepW < 24) stepW = 24;

    float totalGridW = numSteps * (stepW + 4) - 4;
    float startX = x + (w - totalGridW) / 2.0f;  // center the grid

    for (int i = 0; i < numSteps; ++i) {
        float sx = startX + i * (stepW + 4);
        bool isPlayhead = (i == state.gridCurrentStep) && state.playing;
        bool hovered = isHovered(sx, gridY, stepW, gridH, state);
        uint8_t stepState = state.gridStates[i];

        float r, g, b, a;
        if (stepState == 0) { // Accent
            r = Color::Orange[0]; g = Color::Orange[1]; b = Color::Orange[2]; a = isPlayhead ? 1.0f : 0.85f;
        } else if (stepState == 1) { // Normal
            r = Color::Accent[0]; g = Color::Accent[1]; b = Color::Accent[2]; a = isPlayhead ? 1.0f : 0.65f;
        } else { // Muted
            r = Color::Card[0]; g = Color::Card[1]; b = Color::Card[2]; a = 0.7f;
        }

        // Playhead glow
        if (isPlayhead) {
            drawRoundedRect(sx - 2, gridY - 2, stepW + 4, gridH + 4, 8, 1.0f, 1.0f, 1.0f, 0.15f);
        }

        if (hovered && stepState == 2) { a = 0.9f; r = 0.22f; g = 0.22f; b = 0.28f; }
        else if (hovered) { a = 1.0f; }

        drawRoundedRect(sx, gridY, stepW, gridH, 6, r, g, b, a);

        // Step number
        std::string num = std::to_string(i + 1);
        drawTextCentered(sx + stepW / 2, gridY + gridH / 2 - 8, num, 0.30f,
            stepState == 2 ? Color::TextMuted[0] : 1.0f,
            stepState == 2 ? Color::TextMuted[1] : 1.0f,
            stepState == 2 ? Color::TextMuted[2] : 1.0f);

        // State label
        const char* labels[] = {"ACC", "NRM", "---"};
        drawTextCentered(sx + stepW / 2, gridY + gridH - 22, labels[stepState], 0.18f,
            Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

        // Click to cycle
        if (hovered && state.mousePressed) {
            event.type = UIEventType::GridStepCycle;
            event.intValue = i;
        }
    }

    // Legend
    float legY = gridY + gridH + 16;
    drawCircle(startX + 6, legY + 6, 5, Color::Orange[0], Color::Orange[1], Color::Orange[2]);
    drawText(startX + 16, legY - 2, "Accent", 0.20f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    drawCircle(startX + 86, legY + 6, 5, Color::Accent[0], Color::Accent[1], Color::Accent[2]);
    drawText(startX + 96, legY - 2, "Normal", 0.20f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    drawCircle(startX + 166, legY + 6, 5, 0.25f, 0.25f, 0.30f);
    drawText(startX + 176, legY - 2, "Muted", 0.20f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    return event;
}

// =====================================================================
// Tuner Section
// =====================================================================

UIEvent UIRenderer::renderTuner(const UIState& state, float x, float y, float w, float h) {
    UIEvent event;

    drawRoundedRect(x, y, w, (std::min)(h, 360.0f), 10, Color::Panel[0], Color::Panel[1], Color::Panel[2]);

    // Tuner mode selector
    drawText(x + 16, y + 12, "TUNING MODE", 0.22f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    const char* modes[] = {"CHROM", "STD", "DROP D", "DROP C", "BASS", "5-STR", "DROP A"};
    for (int i = 0; i < 7; ++i) {
        if (drawSmallButton(x + 16 + i * 88, y + 36, 80, 30, modes[i], state.tunerMode == i, state)) {
            event.type = UIEventType::TunerModeChange;
            event.intValue = i;
        }
    }

    // --- Big note display ---
    float noteY = y + 90;
    drawText(x + w / 2 - 60, noteY, state.noteName, 2.0f,
        Color::Accent[0], Color::Accent[1], Color::Accent[2]);

    // Frequency
    std::ostringstream freqStr;
    freqStr << std::fixed << std::setprecision(1) << state.currentFreq << " Hz";
    drawTextCentered(x + w / 2, noteY + 90, freqStr.str(), 0.35f,
        Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    // --- Cents deviation bar ---
    float barY = noteY + 130;
    float barW = w - 80;
    float barX = x + 40;
    float barH = 24;

    // Background bar
    drawRoundedRect(barX, barY, barW, barH, 4, Color::Card[0], Color::Card[1], Color::Card[2]);

    // Center line
    drawRect(barX + barW / 2 - 1, barY, 2, barH, Color::TextMuted[0], Color::TextMuted[1], Color::TextMuted[2]);

    // Cents indicator
    float cents = state.diffCents;
    if (cents < -50.0f) cents = -50.0f;
    if (cents > 50.0f) cents = 50.0f;
    float indicatorX = barX + (barW / 2) + (cents / 50.0f) * (barW / 2 - 8);

    // Color: green when close to zero, red when far
    float absCents = std::abs(cents);
    float ir, ig, ib;
    if (absCents < 3.0f) { ir = Color::Green[0]; ig = Color::Green[1]; ib = Color::Green[2]; }
    else if (absCents < 15.0f) { ir = Color::Yellow[0]; ig = Color::Yellow[1]; ib = Color::Yellow[2]; }
    else { ir = Color::Red[0]; ig = Color::Red[1]; ib = Color::Red[2]; }

    if (state.tunerConfidence > 0.3f) {
        drawCircle(indicatorX, barY + barH / 2, 8, ir, ig, ib);
    }

    // Cents labels
    drawText(barX - 24, barY + 2, "-50", 0.20f, Color::TextMuted[0], Color::TextMuted[1], Color::TextMuted[2]);
    drawTextCentered(barX + barW / 2, barY + barH + 4, "0", 0.20f, Color::TextMuted[0], Color::TextMuted[1], Color::TextMuted[2]);
    drawText(barX + barW + 4, barY + 2, "+50", 0.20f, Color::TextMuted[0], Color::TextMuted[1], Color::TextMuted[2]);

    // Cents value text
    std::ostringstream centsStr;
    centsStr << (cents >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << cents << " cents";
    drawTextCentered(x + w / 2, barY + barH + 24, centsStr.str(), 0.28f,
        ir, ig, ib);

    // Signal level
    drawText(x + 16, barY + barH + 60, "SIGNAL", 0.20f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);
    float sigW = 200;
    float sigX = x + 80;
    float sigY = barY + barH + 56;
    drawRoundedRect(sigX, sigY, sigW, 14, 3, Color::Card[0], Color::Card[1], Color::Card[2]);
    float sigLevel = state.tunerSignalLevel;
    if (sigLevel > 1.0f) sigLevel = 1.0f;
    if (sigLevel > 0.005f) {
        drawRoundedRect(sigX + 2, sigY + 2, (sigW - 4) * sigLevel, 10, 2,
            Color::Green[0], Color::Green[1], Color::Green[2], 0.8f);
    }

    return event;
}

// =====================================================================
// Mixer Panel
// =====================================================================

UIEvent UIRenderer::renderMixer(const UIState& state, float x, float y, float w, float h) {
    UIEvent event;
    float panelH = (std::min)(h, 400.0f);

    drawRoundedRect(x, y, w, panelH, 10, Color::Panel[0], Color::Panel[1], Color::Panel[2]);
    drawText(x + 16, y + 12, "MIXER", 0.26f, Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

    // Four faders + peak meters, centered
    struct FaderInfo {
        const char* label;
        float value;
        float peak;
        float r, g, b;
        UIEventType evtType;
    };

    FaderInfo faders[] = {
        {"MASTER", state.masterVol, state.masterPeak, Color::TextBright[0], Color::TextBright[1], Color::TextBright[2], UIEventType::MixerMasterChange},
        {"CLICK",  state.clickVol,  state.clickPeak,  Color::Accent[0], Color::Accent[1], Color::Accent[2], UIEventType::MixerClickChange},
        {"ACCENT", state.accentVol, state.clickPeak,  Color::Orange[0], Color::Orange[1], Color::Orange[2], UIEventType::MixerAccentChange},
        {"INPUT",  state.inputVol,  state.inputPeak,  Color::Green[0], Color::Green[1], Color::Green[2], UIEventType::MixerInputChange},
    };

    float faderW = 36;
    float faderH = panelH - 100;
    float gapBetween = 80;
    float totalFadersW = 4 * faderW + 4 * 20 + 3 * gapBetween; // fader + meter + gap
    float startFX = x + (w - totalFadersW) / 2;
    float faderY = y + 50;

    for (int i = 0; i < 4; ++i) {
        float fx = startFX + i * (faderW + 20 + gapBetween);

        // Fader
        drawFader(fx, faderY, faderW, faderH, faders[i].value, faders[i].peak,
                  faders[i].label, state, faders[i].r, faders[i].g, faders[i].b);

        // Peak meter
        drawPeakMeter(fx + faderW + 6, faderY, 14, faderH, faders[i].peak);

        // Volume percentage
        std::ostringstream volStr;
        volStr << (int)(faders[i].value * 100) << "%";
        drawTextCentered(fx + faderW / 2, faderY - 18, volStr.str(), 0.22f,
            Color::TextDim[0], Color::TextDim[1], Color::TextDim[2]);

        // +/- buttons below fader
        float btnY = faderY + faderH + 30;
        if (drawSmallButton(fx - 4, btnY, 20, 20, "-", false, state)) {
            event.type = faders[i].evtType;
            event.value = -0.05f;
        }
        if (drawSmallButton(fx + faderW - 16, btnY, 20, 20, "+", false, state)) {
            event.type = faders[i].evtType;
            event.value = 0.05f;
        }
    }

    return event;
}
