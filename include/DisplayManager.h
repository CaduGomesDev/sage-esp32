// =============================================================================
//  DisplayManager.h - Rosto animado no OLED I2C
//
//  O display nao mostra mais texto de status: o estado da aplicacao e expresso
//  por um rosto (olhos + boca) desenhado com primitivas simples do Adafruit_GFX.
//
//  Desempenho:
//   - O desenho roda em uma task FreeRTOS dedicada (core 0). As chamadas de
//     STT/LLM/TTS bloqueiam o loop() por segundos; sem task propria a animacao
//     congelaria justamente quando ela e util.
//   - Todo o quadro e montado em RAM e enviado ao painel em um unico flush I2C.
//     O quadro so e enviado quando muda de fato (comparacao com um shadow
//     buffer), entao um rosto parado custa zero trafego no barramento.
//   - Nada aqui bloqueia: sem delay(), sem espera ativa. O I2S e o DMA de audio
//     rodam no core 1 e nao disputam nada com esta task.
//
//  Regra de ouro: somente a task de render toca no barramento I2C. O resto do
//  firmware usa os setters, protegidos por mutex.
//
//  A interface publica e a mesma de antes - so a implementacao mudou.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "AppState.h"
#include "config.h"

#if OLED_DRIVER == OLED_DRIVER_SH1106
#  include <Adafruit_SH110X.h>
using OledDriver = Adafruit_SH1106G;
#  define OLED_COLOR_ON  SH110X_WHITE
#  define OLED_COLOR_OFF SH110X_BLACK
#else
#  include <Adafruit_SSD1306.h>
using OledDriver = Adafruit_SSD1306;
#  define OLED_COLOR_ON  SSD1306_WHITE
#  define OLED_COLOR_OFF SSD1306_BLACK
#endif

// Legenda de uma linha no rodape com o texto de setMessage()/setSubtitle().
// Desligada por padrao: o rosto e a interface. Mensagens de erro aparecem
// mesmo com isto em 0, senao a tela de erro nao diria o que houve.
#ifndef FACE_SHOW_CAPTION
#  define FACE_SHOW_CAPTION 0
#endif

class DisplayManager {
public:
    bool begin();
    void startRenderTask();
    void stopRenderTask();

    // ---- Setters seguros para chamar de qualquer task -----------------------
    void setState(AppState state);
    void setProcessingStep(ProcessingStep step);
    void setWifi(bool connected, uint8_t bars);
    void setLevel(float level01);          // 0.0..1.0 -> olhos/boca reagem
    void setMessage(const String& msg);    // transcricao / resposta
    void setSubtitle(const String& text);  // linha auxiliar
    void showError(const String& msg);

    // Telas estaticas usadas antes da task existir.
    void splash(const char* line1, const char* line2);
    void bootLine(const char* text);

    // Renderiza um quadro manualmente (quando a task nao esta rodando).
    void renderOnce();

private:
    // Geometria do rosto. Os campos sao interpolados quadro a quadro, entao a
    // troca de estado vira uma transicao suave em vez de um corte seco.
    struct Face {
        float eyeW;      // largura do olho (ou diametro do arco)
        float eyeH;      // altura do olho
        float eyeR;      // raio dos cantos
        float spacing;   // distancia entre os centros dos olhos
        float centerY;   // linha dos olhos
    };

    static void taskTrampoline(void* arg);
    void        renderLoop();
    void        drawFrame();

    // ---- Animacao ------------------------------------------------------------
    Face targetFace(AppState state) const;
    void approach(Face& current, const Face& target, float k);
    void updateBlink(AppState state, uint32_t now);
    void updateGaze(AppState state, uint32_t now);

    // ---- Primitivas do rosto -------------------------------------------------
    void drawEyesRounded(const Face& f, float openness);  // IDLE / LISTENING
    void drawEyesHappy(const Face& f);                    // SPEAKING  ^ ^
    void drawEyesBars(const Face& f, float phase);        // PROCESSING
    void drawEyesCross(const Face& f);                    // ERRO / SEM WI-FI
    void drawMouth(float open);
    void drawMouthFrown();
    void drawListeningWave(float level);
    void drawStepDots(ProcessingStep step);

    // Arco parametrico limitado por angulo (graus, 0 = direita, -90 = topo).
    // drawCircleHelper so desenha quadrantes inteiros, o que deixaria laterais
    // verticais longas onde queremos uma curva rasa.
    void drawArc(int cx, int cy, int r, float startDeg, float endDeg,
                 uint8_t thickness);

    // ---- Icones e texto auxiliar ----------------------------------------------
    void drawWifiIcon(int x, int y, bool connected, uint8_t bars);
    void drawAlertIcon(int x, int y);
    void drawCaption(const String& text);
    void drawWrappedText(const String& text, int x, int y, int maxWidth,
                         int maxLines, uint8_t textSize = 1);

    // Envia ao painel apenas se o quadro mudou. Retorna true se houve flush.
    bool flushIfChanged();
    void flushForced();

    OledDriver*       _oled    = nullptr;
    SemaphoreHandle_t _mutex   = nullptr;
    TaskHandle_t      _task    = nullptr;
    volatile bool     _running = false;
    bool              _ready   = false;

    // ---- Estado compartilhado (protegido por _mutex) --------------------------
    AppState       _state    = STATE_BOOT;
    ProcessingStep _step     = STEP_NONE;
    bool           _wifiOk   = false;
    uint8_t        _wifiBars = 0;
    float          _level    = 0.0f;
    String         _message;
    String         _subtitle;
    String         _error;

    // ---- Estado exclusivo da task de render ------------------------------------
    uint32_t _frame        = 0;
    float    _levelSmooth  = 0.0f;
    Face     _face         = {26, 30, 11, 50, 33};

    float    _blink        = 1.0f;   // 1 = olho aberto, 0 = fechado
    uint32_t _nextBlinkMs  = 0;
    uint32_t _blinkStartMs = 0;
    uint8_t  _blinksLeft   = 0;

    float    _gazeX = 0.0f, _gazeY = 0.0f;    // olhar atual
    float    _gazeTX = 0.0f, _gazeTY = 0.0f;  // alvo do olhar
    uint32_t _nextGazeMs = 0;

    uint8_t* _shadow      = nullptr;  // ultimo quadro enviado ao painel
    size_t   _shadowBytes = 0;
};
