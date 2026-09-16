#include "DisplayManager.h"

#include <Wire.h>
#include <math.h>
#include <string.h>

namespace {

constexpr int CHAR_W = 6;    // fonte padrao 5x7 + 1 px de espaco
constexpr int LINE_H = 8;
constexpr int CX     = OLED_WIDTH / 2;

constexpr uint32_t BLINK_MS      = 150;   // duracao de uma piscada completa
constexpr uint8_t  DOUBLE_BLINK_PCT = 20; // chance de piscada dupla

constexpr int   ARC_STEPS = 14;    // segmentos por arco: suave o bastante a 128x64
constexpr float ARC_FROM  = -145.0f;  // angulos do arco dos olhos felizes
constexpr float ARC_TO    = -35.0f;   // (-90 graus = topo do circulo)

inline float lerp(float a, float b, float k) { return a + (b - a) * k; }

} // namespace

// =============================================================================
//  Ciclo de vida
// =============================================================================

bool DisplayManager::begin() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_I2C_FREQ);

    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        Serial.println(F("[Display] ERRO: nao foi possivel criar o mutex."));
        return false;
    }

    // Os dois ultimos argumentos mantem o I2C em 400 kHz DEPOIS do init: o
    // padrao da biblioteca volta para 100 kHz e derruba o frame rate.
    _oled = new OledDriver(OLED_WIDTH, OLED_HEIGHT, &Wire, -1,
                           OLED_I2C_FREQ, OLED_I2C_FREQ);

#if OLED_DRIVER == OLED_DRIVER_SH1106
    const bool ok = _oled->begin(OLED_I2C_ADDRESS, true);
#else
    const bool ok = _oled->begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
#endif

    if (!ok) {
        Serial.printf("[Display] ERRO: OLED nao respondeu em 0x%02X. "
                      "Confira SDA=%d SCL=%d e o endereco I2C.\n",
                      OLED_I2C_ADDRESS, PIN_OLED_SDA, PIN_OLED_SCL);
        delete _oled;
        _oled = nullptr;
        return false;
    }

    // Shadow buffer para so mandar ao painel os quadros que mudaram.
    _shadowBytes = (size_t)OLED_WIDTH * ((OLED_HEIGHT + 7) / 8);
    _shadow      = (uint8_t*)malloc(_shadowBytes);
    if (_shadow) memset(_shadow, 0xFF, _shadowBytes);   // forca o 1o flush

    _oled->clearDisplay();
    _oled->setTextColor(OLED_COLOR_ON);
    _oled->setTextWrap(false);
    _oled->display();

    _ready = true;
    Serial.println(F("[Display] OLED inicializado."));
    return true;
}

void DisplayManager::startRenderTask() {
    if (!_ready || _running) return;
    _running = true;
    xTaskCreatePinnedToCore(taskTrampoline, "display", DISPLAY_TASK_STACK, this,
                            1, &_task, DISPLAY_TASK_CORE);
    Serial.println(F("[Display] Task de render iniciada."));
}

void DisplayManager::stopRenderTask() {
    if (!_running) return;
    _running = false;
    while (_task != nullptr) delay(10);
}

void DisplayManager::taskTrampoline(void* arg) {
    static_cast<DisplayManager*>(arg)->renderLoop();
}

void DisplayManager::renderLoop() {
    const TickType_t period = pdMS_TO_TICKS(1000 / DISPLAY_FPS);
    TickType_t last = xTaskGetTickCount();

    while (_running) {
        drawFrame();
        // vTaskDelayUntil mantem a cadencia estavel e devolve a CPU entre os
        // quadros: nada aqui faz espera ativa.
        vTaskDelayUntil(&last, period);
    }

    _task = nullptr;
    vTaskDelete(nullptr);
}

// =============================================================================
//  Setters (thread-safe)
// =============================================================================

#define LOCK()   if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY)
#define UNLOCK() if (_mutex) xSemaphoreGive(_mutex)

void DisplayManager::setState(AppState state) {
    LOCK();
    if (_state != state) {
        _state = state;
        if (state != STATE_ERROR)      _error = "";
        if (state != STATE_PROCESSING) _step  = STEP_NONE;
    }
    UNLOCK();
}

void DisplayManager::setProcessingStep(ProcessingStep step) {
    LOCK();
    _step = step;
    UNLOCK();
}

void DisplayManager::setWifi(bool connected, uint8_t bars) {
    LOCK();
    _wifiOk   = connected;
    _wifiBars = bars;
    UNLOCK();
}

void DisplayManager::setLevel(float level01) {
    LOCK();
    _level = constrain(level01, 0.0f, 1.0f);
    UNLOCK();
}

void DisplayManager::setMessage(const String& msg) {
    LOCK();
    _message = msg;
    UNLOCK();
}

void DisplayManager::setSubtitle(const String& text) {
    LOCK();
    _subtitle = text;
    UNLOCK();
}

void DisplayManager::showError(const String& msg) {
    LOCK();
    _error = msg;
    _state = STATE_ERROR;
    UNLOCK();
}

// =============================================================================
//  Telas estaticas (boot, antes da task existir)
// =============================================================================

void DisplayManager::splash(const char* line1, const char* line2) {
    if (!_ready) return;
    _oled->clearDisplay();

    // Um par de olhos acordando, para o boot ja ter a cara do assistente.
    _oled->fillRoundRect(CX - 25 - 13, 12, 26, 22, 10, OLED_COLOR_ON);
    _oled->fillRoundRect(CX + 25 - 13, 12, 26, 22, 10, OLED_COLOR_ON);

    _oled->setTextSize(1);
    const int w1 = strlen(line1) * CHAR_W * 2;
    _oled->setTextSize(2);
    _oled->setCursor((OLED_WIDTH - w1) / 2, 40);
    _oled->print(line1);

    _oled->setTextSize(1);
    const int w2 = strlen(line2) * CHAR_W;
    _oled->setCursor((OLED_WIDTH - w2) / 2, OLED_HEIGHT - 7);
    _oled->print(line2);

    flushForced();
}

void DisplayManager::bootLine(const char* text) {
    if (!_ready) return;
    _oled->clearDisplay();
    _oled->setTextSize(1);
    _oled->setCursor(0, 0);
    _oled->print(F(DEVICE_NAME));
    _oled->drawFastHLine(0, 10, OLED_WIDTH, OLED_COLOR_ON);
    drawWrappedText(String(text), 0, 20, OLED_WIDTH, 4, 1);
    flushForced();
}

void DisplayManager::renderOnce() {
    drawFrame();
}

// =============================================================================
//  Render principal
// =============================================================================

void DisplayManager::drawFrame() {
    if (!_ready) return;

    // Copia o estado compartilhado e solta o mutex antes de desenhar: o resto
    // do firmware nunca fica esperando o display.
    AppState       state;
    ProcessingStep step;
    bool           wifiOk;
    uint8_t        bars;
    float          level;
    String         message, subtitle, error;

    LOCK();
    state    = _state;
    step     = _step;
    wifiOk   = _wifiOk;
    bars     = _wifiBars;
    level    = _level;
    message  = _message;
    subtitle = _subtitle;
    error    = _error;
    UNLOCK();

    _frame++;
    const uint32_t now = millis();

    // Ataque rapido, decaimento lento: a boca e a onda acompanham a voz sem
    // tremer a cada bloco de audio.
    _levelSmooth = (level > _levelSmooth) ? lerp(_levelSmooth, level, 0.60f)
                                          : lerp(_levelSmooth, level, 0.18f);

    // Sem Wi-Fi o rosto fica "tonto" mesmo com a aplicacao ociosa. STATE_BOOT
    // fica de fora: ainda nem tentamos conectar, nao ha o que reclamar.
    const bool offline = !wifiOk && (state == STATE_IDLE || state == STATE_WIFI);
    const bool dizzy   = offline || (state == STATE_ERROR);

    // A geometria segue o rosto que sera desenhado, nao o estado logico: sem
    // isso o "X X" do modo offline herdaria o tamanho dos olhos do IDLE.
    const AppState visual = dizzy ? STATE_ERROR : state;

    updateBlink(visual, now);
    updateGaze(visual, now);
    approach(_face, targetFace(visual), 0.25f);

    _oled->clearDisplay();

    if (dizzy) {
        drawEyesCross(_face);
        drawMouthFrown();
    } else {
        switch (state) {
            case STATE_BOOT:
            case STATE_WIFI:
                drawEyesRounded(_face, _blink);
                break;

            case STATE_IDLE:
                drawEyesRounded(_face, _blink);
                break;

            case STATE_LISTENING:
                drawEyesRounded(_face, _blink);
                drawListeningWave(_levelSmooth);
                break;

            case STATE_PROCESSING:
                drawEyesBars(_face, _frame * 0.16f);
                drawStepDots(step);
                break;

            case STATE_SPEAKING:
                drawEyesHappy(_face);
                drawMouth(_levelSmooth);
                break;

            case STATE_ERROR:   // tratado acima
                break;
        }
    }

    drawWifiIcon(OLED_WIDTH - 13, 1, wifiOk, bars);
    if (dizzy) drawAlertIcon(2, 1);

    // O texto do erro sobrevive mesmo com a legenda desligada: um rosto "X X"
    // sem dizer o que falhou nao ajuda ninguem a depurar.
    if (state == STATE_ERROR && error.length()) {
        drawCaption(error);
    }
#if FACE_SHOW_CAPTION
    else if (message.length())  drawCaption(message);
    else if (subtitle.length()) drawCaption(subtitle);
#else
    (void)message;
    (void)subtitle;
#endif

    flushIfChanged();
}

// =============================================================================
//  Animacao
// =============================================================================

DisplayManager::Face DisplayManager::targetFace(AppState state) const {
    //            eyeW  eyeH  eyeR  spacing  centerY
    switch (state) {
        case STATE_BOOT:       return {26.0f,  6.0f,  3.0f, 50.0f, 34.0f};  // sonolento
        case STATE_WIFI:       return {24.0f, 24.0f,  0.0f, 50.0f, 26.0f};  // "X X"
        case STATE_ERROR:      return {24.0f, 24.0f,  0.0f, 50.0f, 26.0f};
        case STATE_LISTENING:  return {32.0f, 36.0f, 14.0f, 56.0f, 26.0f};  // arregalado
        case STATE_PROCESSING: return {30.0f,  7.0f,  3.5f, 52.0f, 32.0f};  // barras
        case STATE_SPEAKING:   return {30.0f, 28.0f, 12.0f, 54.0f, 32.0f};  // "^ ^"
        case STATE_IDLE:
        default:               return {26.0f, 30.0f, 11.0f, 50.0f, 33.0f};
    }
}

void DisplayManager::approach(Face& c, const Face& t, float k) {
    c.eyeW    = lerp(c.eyeW,    t.eyeW,    k);
    c.eyeH    = lerp(c.eyeH,    t.eyeH,    k);
    c.eyeR    = lerp(c.eyeR,    t.eyeR,    k);
    c.spacing = lerp(c.spacing, t.spacing, k);
    c.centerY = lerp(c.centerY, t.centerY, k);
}

void DisplayManager::updateBlink(AppState state, uint32_t now) {
    // So piscam os olhos que tem palpebra (redondos).
    const bool canBlink = (state == STATE_IDLE || state == STATE_LISTENING);
    if (!canBlink) {
        _blink       = 1.0f;
        _blinksLeft  = 0;
        _nextBlinkMs = now + 2000;
        return;
    }

    if (_blinksLeft == 0 && now >= _nextBlinkMs) {
        // 3 a 5 s ocioso; mais espacado quando esta prestando atencao.
        const long lo = (state == STATE_IDLE) ? 3000 : 5000;
        const long hi = (state == STATE_IDLE) ? 5000 : 8000;
        _nextBlinkMs  = now + (uint32_t)random(lo, hi);
        _blinkStartMs = now;
        _blinksLeft   = (random(100) < DOUBLE_BLINK_PCT) ? 2 : 1;
    }

    if (_blinksLeft == 0) {
        _blink = 1.0f;
        return;
    }

    const uint32_t dt = now - _blinkStartMs;
    if (dt >= BLINK_MS) {
        _blinksLeft--;
        _blink = 1.0f;
        if (_blinksLeft > 0) _blinkStartMs = now;   // encadeia a segunda
        return;
    }

    // Triangular: fecha na primeira metade, abre na segunda.
    const float half = BLINK_MS / 2.0f;
    _blink = (dt < half) ? (1.0f - dt / half) : ((dt - half) / half);
    _blink = constrain(_blink, 0.0f, 1.0f);
}

void DisplayManager::updateGaze(AppState state, uint32_t now) {
    if (state != STATE_IDLE) {
        _gazeTX = 0.0f;
        _gazeTY = 0.0f;
    } else if (now >= _nextGazeMs) {
        // Pequenas sacadas: o rosto "olha em volta" enquanto espera.
        _nextGazeMs = now + (uint32_t)random(1500, 3500);
        _gazeTX = (float)random(-6, 7);
        _gazeTY = (float)random(-3, 4);
    }
    _gazeX = lerp(_gazeX, _gazeTX, 0.18f);
    _gazeY = lerp(_gazeY, _gazeTY, 0.18f);
}

// =============================================================================
//  Olhos
// =============================================================================

void DisplayManager::drawEyesRounded(const Face& f, float openness) {
    const int w  = (int)f.eyeW;
    int       h  = (int)(f.eyeH * openness);
    if (h < 2) h = 2;                                  // olho fechado = tracinho

    int r = (int)f.eyeR;
    r = min(r, min(w, h) / 2);                         // fillRoundRect exige isso
    if (r < 0) r = 0;

    const int cy = (int)(f.centerY + _gazeY);
    const int lx = (int)(CX - f.spacing / 2 + _gazeX);
    const int rx = (int)(CX + f.spacing / 2 + _gazeX);

    _oled->fillRoundRect(lx - w / 2, cy - h / 2, w, h, r, OLED_COLOR_ON);
    _oled->fillRoundRect(rx - w / 2, cy - h / 2, w, h, r, OLED_COLOR_ON);
}

void DisplayManager::drawEyesHappy(const Face& f) {
    // Arco raso com a curva para cima: "^ ^". O fator 0.61 compensa o recorte
    // angular para que a largura visivel do arco fique proxima de eyeW.
    const int r  = (int)(f.eyeW * 0.61f);
    const int cy = (int)(f.centerY + f.eyeH / 4);      // centro do arco abaixo
    const int lx = (int)(CX - f.spacing / 2);
    const int rx = (int)(CX + f.spacing / 2);

    drawArc(lx, cy, r, ARC_FROM, ARC_TO, 3);
    drawArc(rx, cy, r, ARC_FROM, ARC_TO, 3);
}

void DisplayManager::drawEyesBars(const Face& f, float phase) {
    // Duas barras horizontais subindo e descendo em contrafase: efeito de
    // carregamento sem precisar de sprite nem de texto.
    const int w = (int)f.eyeW;
    const int h = max(4, (int)f.eyeH);
    const int r = h / 2;
    const int amp = 8;

    const int lx = (int)(CX - f.spacing / 2);
    const int rx = (int)(CX + f.spacing / 2);
    const int ly = (int)(f.centerY + sinf(phase) * amp);
    const int ry = (int)(f.centerY - sinf(phase) * amp);

    _oled->fillRoundRect(lx - w / 2, ly - h / 2, w, h, r, OLED_COLOR_ON);
    _oled->fillRoundRect(rx - w / 2, ry - h / 2, w, h, r, OLED_COLOR_ON);
}

void DisplayManager::drawEyesCross(const Face& f) {
    const int s  = (int)(f.eyeW / 2);      // meia diagonal
    const int cy = (int)f.centerY;
    const int xs[2] = {(int)(CX - f.spacing / 2), (int)(CX + f.spacing / 2)};

    for (int i = 0; i < 2; i++) {
        const int cx = xs[i];
        for (int t = -1; t <= 1; t++) {    // espessura de 3 px
            _oled->drawLine(cx - s, cy - s + t, cx + s, cy + s + t, OLED_COLOR_ON);
            _oled->drawLine(cx + s, cy - s + t, cx - s, cy + s + t, OLED_COLOR_ON);
        }
    }
}

// =============================================================================
//  Boca e elementos de apoio
// =============================================================================

void DisplayManager::drawMouth(float open) {
    // Abertura acompanha a amplitude do audio que esta tocando.
    const int w = 26;
    const int h = 3 + (int)(open * 11.0f);
    const int r = min(h / 2, 8);
    _oled->fillRoundRect(CX - w / 2, 49 - h / 2, w, h, r, OLED_COLOR_ON);
}

void DisplayManager::drawMouthFrown() {
    // Centro abaixo da boca: a curva abre para baixo nas pontas = boca virada.
    drawArc(CX, 58, 14, -140.0f, -40.0f, 2);
}

void DisplayManager::drawListeningWave(float level) {
    const int   baseY = 55;
    const int   x0    = 18;
    const int   x1    = OLED_WIDTH - 18;
    const float amp   = 2.0f + level * 6.0f;
    const float span  = (float)(x1 - x0);

    int prevY = baseY;
    for (int x = x0; x <= x1; x += 2) {
        const float t   = (x - x0) / span;               // 0..1
        const float env = sinf(t * PI);                  // suaviza as pontas
        const int   y   = baseY - (int)(sinf(t * 6.0f * PI - _frame * 0.35f) * amp * env);
        if (x > x0) _oled->drawLine(x - 2, prevY, x, y, OLED_COLOR_ON);
        prevY = y;
    }
}

void DisplayManager::drawStepDots(ProcessingStep step) {
    // Tres pontos: STT -> LLM -> TTS. Diz em que etapa esta sem escrever nada.
    const int active = (step == STEP_STT) ? 0 : (step == STEP_LLM) ? 1
                     : (step == STEP_TTS) ? 2 : -1;
    const int gap = 14;

    for (int i = 0; i < 3; i++) {
        const int x = CX - gap + i * gap;
        if (i == active) _oled->fillCircle(x, 56, 3, OLED_COLOR_ON);
        else             _oled->drawCircle(x, 56, 2, OLED_COLOR_ON);
    }
}

void DisplayManager::drawArc(int cx, int cy, int r, float startDeg, float endDeg,
                             uint8_t thickness) {
    int px[ARC_STEPS + 1], py[ARC_STEPS + 1];

    // Amostra a curva uma unica vez (sin/cos sao o custo real aqui).
    // lroundf, nao (int): truncar em direcao a zero deforma o apice do arco,
    // que e justamente onde o olho "^" tem que ficar simetrico.
    for (int i = 0; i <= ARC_STEPS; i++) {
        const float a = (float)((startDeg + (endDeg - startDeg) * i / ARC_STEPS) *
                                DEG_TO_RAD);
        px[i] = cx + (int)lroundf(cosf(a) * r);
        py[i] = cy + (int)lroundf(sinf(a) * r);
    }

    // A espessura vira deslocamento em Y, nao raios concentricos: em uma curva
    // quase horizontal isso da um traco solido, sem os buracos que o
    // arredondamento para inteiro abre entre circunferencias vizinhas.
    for (uint8_t t = 0; t < thickness; t++) {
        for (int i = 1; i <= ARC_STEPS; i++) {
            _oled->drawLine(px[i - 1], py[i - 1] + t, px[i], py[i] + t, OLED_COLOR_ON);
        }
    }
}

// =============================================================================
//  Icones e texto
// =============================================================================

void DisplayManager::drawWifiIcon(int x, int y, bool connected, uint8_t bars) {
    if (!connected) {
        _oled->drawLine(x + 2, y + 1, x + 10, y + 9, OLED_COLOR_ON);
        _oled->drawLine(x + 10, y + 1, x + 2, y + 9, OLED_COLOR_ON);
        return;
    }
    for (uint8_t i = 0; i < 4; i++) {
        const int h  = 2 + i * 2;          // 2,4,6,8 px
        const int bx = x + i * 3;
        const int by = y + 9 - h;
        if (i < bars) _oled->fillRect(bx, by, 2, h, OLED_COLOR_ON);
        else          _oled->drawRect(bx, by, 2, h, OLED_COLOR_ON);
    }
}

void DisplayManager::drawAlertIcon(int x, int y) {
    _oled->drawTriangle(x + 5, y, x, y + 10, x + 10, y + 10, OLED_COLOR_ON);
    _oled->drawFastVLine(x + 5, y + 4, 3, OLED_COLOR_ON);
    _oled->drawPixel(x + 5, y + 8, OLED_COLOR_ON);
}

void DisplayManager::drawCaption(const String& text) {
    const unsigned int maxChars = OLED_WIDTH / CHAR_W;   // 21 caracteres

    String s = text;
    if (s.length() > maxChars) s = s.substring(0, maxChars - 1) + ".";

    _oled->setTextSize(1);
    _oled->setCursor((OLED_WIDTH - (int)s.length() * CHAR_W) / 2, OLED_HEIGHT - 7);
    _oled->print(s);
}

void DisplayManager::drawWrappedText(const String& text, int x, int y,
                                     int maxWidth, int maxLines, uint8_t textSize) {
    _oled->setTextSize(textSize);
    const int charW      = CHAR_W * textSize;
    const int lineH      = LINE_H * textSize;
    const int maxPerLine = maxWidth / charW;
    if (maxPerLine <= 0) return;

    int line = 0;
    unsigned int pos = 0;

    while (pos < text.length() && line < maxLines) {
        int take = min<int>(maxPerLine, text.length() - pos);

        if ((int)(pos + take) < (int)text.length()) {
            int brk = take;
            while (brk > 0 && text[pos + brk] != ' ') brk--;
            if (brk > maxPerLine / 3) take = brk;
        }

        String chunk = text.substring(pos, pos + take);
        chunk.trim();

        if (line == maxLines - 1 && (pos + take) < text.length()) {
            if (chunk.length() > (unsigned)(maxPerLine - 3))
                chunk = chunk.substring(0, maxPerLine - 3);
            chunk += "...";
        }

        _oled->setCursor(x, y + line * lineH);
        _oled->print(chunk);

        pos += take;
        while (pos < text.length() && text[pos] == ' ') pos++;
        line++;
    }

    _oled->setTextSize(1);
}

// =============================================================================
//  Flush
// =============================================================================

bool DisplayManager::flushIfChanged() {
    uint8_t* buf = _oled->getBuffer();
    if (!buf || !_shadow) {           // sem shadow: manda sempre
        _oled->display();
        return true;
    }
    if (memcmp(buf, _shadow, _shadowBytes) == 0) {
        return false;                 // quadro identico: economiza ~23 ms de I2C
    }
    memcpy(_shadow, buf, _shadowBytes);
    _oled->display();
    return true;
}

void DisplayManager::flushForced() {
    _oled->display();
    uint8_t* buf = _oled->getBuffer();
    if (buf && _shadow) memcpy(_shadow, buf, _shadowBytes);
}

#undef LOCK
#undef UNLOCK
