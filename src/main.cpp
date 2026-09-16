// =============================================================================
//  Sage - Assistente de Voz Nativo e Autonomo
//  ESP32-S3 DevKitC-1 N16R8 | PlatformIO + Arduino
//
//  Fluxo:
//    IDLE --(voz ou botao)--> LISTENING --> PROCESSING --> SPEAKING --> IDLE
//                                             |
//                                   STT (Groq/Whisper)
//                                   LLM (Gemini)
//                                   TTS (ElevenLabs/Google)
//
//  As chaves de API ficam em include/secrets.h (copie de secrets.h.example).
// =============================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "AppState.h"
#include "AudioBuffer.h"
#include "AudioPlayer.h"
#include "AudioRecorder.h"
#include "DisplayManager.h"
#include "LLMClient.h"
#include "NetworkManager.h"
#include "STTClient.h"
#include "TTSClient.h"
#include "config.h"

// =============================================================================
//  Modulos
// =============================================================================
static NetworkManager net;
static DisplayManager display;
static AudioRecorder  recorder;
static AudioPlayer    player;
static STTClient      stt;
static LLMClient      llm;
static TTSClient      tts;

static AudioBuffer recBuffer;   // WAV gravado do usuario
static AudioBuffer ttsBuffer;   // audio da resposta

// =============================================================================
//  Estado
// =============================================================================
static AppState g_state          = STATE_BOOT;
static uint32_t g_stateEnteredMs = 0;
static bool     g_pushToTalk     = false;   // gravacao iniciada pelo botao
static uint32_t g_cooldownUntil  = 0;       // ignora o mic logo apos falar
static String   g_question;
static String   g_answer;
static String   g_lastError;

// =============================================================================
//  Utilitarios
// =============================================================================

static void setState(AppState next) {
    if (g_state == next) return;
    Serial.printf("\n>>> %s -> %s\n", appStateName(g_state), appStateName(next));
    g_state          = next;
    g_stateEnteredMs = millis();
    display.setState(next);
}

static void fail(const String& msg) {
    g_lastError = msg;
    Serial.printf("[ERRO] %s\n", msg.c_str());
    display.showError(msg);
    g_state          = STATE_ERROR;
    g_stateEnteredMs = millis();
}

static bool buttonPressed() {
    static bool     lastRaw   = false;
    static bool     stable    = false;
    static uint32_t lastEdge  = 0;

    const bool raw = (digitalRead(PIN_BUTTON) == BUTTON_ACTIVE_LEVEL);
    if (raw != lastRaw) {
        lastRaw  = raw;
        lastEdge = millis();
    }
    if ((millis() - lastEdge) > 30 && stable != raw) {
        stable = raw;
        if (stable) return true;    // dispara apenas na borda de descida
    }
    return false;
}

static bool buttonHeld() {
    return digitalRead(PIN_BUTTON) == BUTTON_ACTIVE_LEVEL;
}

static void printMemory(const char* tag) {
    Serial.printf("[MEM] %-12s heap: %6u livre / %6u min | PSRAM: %7u livre de %7u\n",
                  tag,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)ESP.getPsramSize());
}

static void printBanner() {
    Serial.println();
    Serial.println(F("============================================================"));
    Serial.println(F("  " DEVICE_NAME " - Assistente de Voz  |  v" FIRMWARE_VERSION));
    Serial.println(F("============================================================"));
    Serial.printf("  Chip .........: %s rev %d, %d nucleo(s) @ %u MHz\n",
                  ESP.getChipModel(), (int)ESP.getChipRevision(),
                  (int)ESP.getChipCores(), (unsigned)getCpuFrequencyMhz());
    Serial.printf("  Flash ........: %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.printf("  PSRAM ........: %s (%u MB)\n",
                  psramFound() ? "detectada" : "NAO DETECTADA!",
                  (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
    Serial.printf("  Arduino core .: %d.%d.%d\n", ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
    Serial.println(F("------------------------------------------------------------"));
}

// =============================================================================
//  Console serial (bring-up e testes sem microfone)
// =============================================================================

static void handleSerialConsole() {
    if (!Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) return;

    if (line == "help" || line == "?") {
        Serial.println(F("Comandos: ask <texto> | say <texto> | listen | cal | "
                         "reset | status | vol <0-100>"));
    } else if (line.startsWith("ask ")) {
        g_question = line.substring(4);
        setState(STATE_PROCESSING);
    } else if (line.startsWith("say ")) {
        g_answer = line.substring(4);
        String err;
        display.setProcessingStep(STEP_TTS);
        setState(STATE_PROCESSING);
        if (tts.synthesize(g_answer, ttsBuffer, err)) setState(STATE_SPEAKING);
        else                                          fail(err);
    } else if (line == "listen") {
        g_pushToTalk = false;
        setState(STATE_LISTENING);
    } else if (line == "cal") {
        recorder.calibrateNoiseFloor();
    } else if (line == "reset") {
        llm.resetHistory();
    } else if (line == "status") {
        printMemory("status");
        Serial.printf("[STATUS] estado=%s wifi=%s ip=%s rssi=%d piso_ruido=%.0f\n",
                      appStateName(g_state), net.isConnected() ? "on" : "off",
                      net.ip().c_str(), net.rssi(), recorder.noiseFloor());
    } else if (line.startsWith("vol ")) {
        const int v = line.substring(4).toInt();
        player.setVolume(v / 100.0f);
        Serial.printf("[STATUS] volume = %d%%\n", v);
    } else {
        Serial.println(F("Comando desconhecido. Digite 'help'."));
    }
}

// =============================================================================
//  Estados
// =============================================================================

static void onIdle() {
    // Anima o VU com o ruido ambiente e observa o gatilho de voz.
    const float level = recorder.poll();
    display.setLevel(level);

    if (millis() < g_cooldownUntil) return;   // ignora o eco do proprio TTS

    if (buttonPressed()) {
        g_pushToTalk = true;
        setState(STATE_LISTENING);
        return;
    }
    if (recorder.voiceDetected()) {
        Serial.println(F("[VAD] Voz detectada."));
        g_pushToTalk = false;
        setState(STATE_LISTENING);
        return;
    }

    static uint32_t lastReport = 0;
    if (millis() - lastReport > 60000) {
        lastReport = millis();
        printMemory("idle");
    }
}

static void onListening() {
    // O bipe so faz sentido no push-to-talk. Com gatilho por voz o usuario ja
    // comecou a falar, e limpar o microfone aqui cortaria a primeira silaba.
    if (g_pushToTalk) {
#if ENABLE_START_BEEP
        player.beep();
#endif
        recorder.flush();          // descarta o proprio bipe do pre-roll
    }

    RecordOptions opt;
    opt.useVad = !g_pushToTalk;   // no push-to-talk quem manda e o botao

    const RecordResult result = recorder.record(
        recBuffer, opt,
        [](float level, uint32_t /*elapsedMs*/) { display.setLevel(level); },
        []() { return g_pushToTalk && !buttonHeld(); });

    display.setLevel(0.0f);

    if (result != REC_OK && result != REC_BUFFER_FULL) {
        Serial.printf("[Rec] Descartado: %s\n", AudioRecorder::resultName(result));
        recorder.flush();
        setState(STATE_IDLE);
        return;
    }

    setState(STATE_PROCESSING);
}

static void onProcessing() {
    String err;

    // ---- 1. Transcricao (pulada quando a pergunta veio do console) ----------
    if (g_question.isEmpty()) {
        display.setProcessingStep(STEP_STT);
        if (!stt.transcribe(recBuffer, g_question, err)) {
            fail(err);
            return;
        }
    }
    display.setMessage(g_question);

    // ---- 2. LLM -------------------------------------------------------------
    display.setProcessingStep(STEP_LLM);
    if (!llm.ask(g_question, g_answer, err)) {
        fail(err);
        return;
    }
    display.setMessage(g_answer);

    // ---- 3. TTS -------------------------------------------------------------
    display.setProcessingStep(STEP_TTS);
    if (!tts.synthesize(g_answer, ttsBuffer, err,
                        [](size_t /*bytes*/) { /* progresso, se quiser animar */ })) {
        fail(err);
        return;
    }

    Serial.printf("[Tempo] STT %u ms + LLM %u ms + TTS %u ms = %u ms\n",
                  (unsigned)stt.lastLatencyMs(), (unsigned)llm.lastLatencyMs(),
                  (unsigned)tts.lastLatencyMs(),
                  (unsigned)(stt.lastLatencyMs() + llm.lastLatencyMs() +
                             tts.lastLatencyMs()));

    setState(STATE_SPEAKING);
}

static void onSpeaking() {
    display.setMessage(g_answer);

    player.play(ttsBuffer, [](float /*progress*/, float level) {
        display.setLevel(level);
    });

    display.setLevel(0.0f);
    player.silence();

    // Limpa o que o microfone captou do proprio alto-falante e da um respiro
    // antes de voltar a escutar, senao a resposta dispara o VAD de novo.
    recorder.flush();
    g_cooldownUntil = millis() + 600;

    g_question = "";
    g_answer   = "";
    display.setMessage("");

    printMemory("pos-ciclo");
    setState(STATE_IDLE);
}

static void onError() {
    if (millis() - g_stateEnteredMs < ERROR_DISPLAY_MS) return;

    g_question = "";
    g_answer   = "";
    display.setMessage("");
    recorder.flush();
    g_cooldownUntil = millis() + 300;
    setState(STATE_IDLE);
}

static void onWifiState() {
    static uint32_t lastSubtitleMs = 0;
    if (millis() - lastSubtitleMs > 1000) {   // evita realocar String a cada loop
        lastSubtitleMs = millis();
        display.setSubtitle(String(F("Reconectando ao Wi-Fi...")));
    }
    if (net.isConnected()) {
        display.setSubtitle("");
        setState(STATE_IDLE);
    }
}

// =============================================================================
//  setup()
// =============================================================================

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(100);
    delay(300);                       // tempo para o monitor serial abrir

    printBanner();

    // ---- Display -------------------------------------------------------------
    if (!display.begin()) {
        Serial.println(F("[BOOT] Seguindo sem display."));
    } else {
        display.splash(DEVICE_NAME, "Assistente de voz");
        delay(1200);
        display.startRenderTask();
    }
    display.setState(STATE_BOOT);

    // ---- PSRAM ----------------------------------------------------------------
    if (!psramFound()) {
        Serial.println(F("[BOOT] AVISO: PSRAM nao detectada! Verifique "
                         "board_build.arduino.memory_type = qio_opi"));
    }

    display.setSubtitle(String(F("Alocando buffers...")));
    if (!recBuffer.allocate(REC_BUFFER_BYTES) || !ttsBuffer.allocate(TTS_BUFFER_BYTES)) {
        Serial.println(F("[BOOT] ERRO FATAL: sem memoria para os buffers de audio."));
        display.showError(F("Sem memoria de audio"));
        while (true) delay(1000);
    }
    printMemory("pos-buffers");

    // ---- Botao ----------------------------------------------------------------
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // ---- Audio -----------------------------------------------------------------
    display.setSubtitle(String(F("Iniciando audio...")));
    if (!player.begin(TTSClient::outputSampleRate())) {
        Serial.println(F("[BOOT] ERRO: alto-falante I2S nao inicializou."));
    }
    player.setVolume(SPK_DEFAULT_VOLUME);

    if (!recorder.begin()) {
        Serial.println(F("[BOOT] ERRO FATAL: microfone I2S nao inicializou."));
        display.showError(F("Falha no microfone"));
        while (true) delay(1000);
    }

    // ---- Wi-Fi ------------------------------------------------------------------
    setState(STATE_WIFI);
    display.setSubtitle(String(F("Conectando ao Wi-Fi...")));
    net.begin(WIFI_SSID, WIFI_PASSWORD);

    if (net.connectBlocking(WIFI_CONNECT_TIMEOUT_MS)) {
        display.setWifi(true, net.signalBars());
        display.setSubtitle(net.ip());
        net.syncTime();
    } else {
        Serial.println(F("[BOOT] Sem Wi-Fi por enquanto; o loop continua tentando."));
    }

    // ---- Calibracao do VAD --------------------------------------------------------
    display.setSubtitle(String(F("Calibrando microfone...")));
    recorder.calibrateNoiseFloor();

    // ---- Pronto --------------------------------------------------------------------
    llm.setSystemPrompt(LLM_SYSTEM_PROMPT);
    display.setSubtitle("");

    printMemory("pos-setup");
    Serial.println(F("------------------------------------------------------------"));
    Serial.println(F("  Pronto. Fale perto do microfone ou segure o botao BOOT."));
    Serial.println(F("  Digite 'help' no monitor serial para os comandos de teste."));
    Serial.println(F("============================================================\n"));

    player.beep(660, 80);
    delay(60);
    player.beep(990, 110);

    recorder.flush();
    setState(STATE_IDLE);
}

// =============================================================================
//  loop()
// =============================================================================

void loop() {
    net.loop();
    handleSerialConsole();

    static uint32_t lastWifiUiMs = 0;
    if (millis() - lastWifiUiMs > 500) {
        lastWifiUiMs = millis();
        display.setWifi(net.isConnected(), net.signalBars());
    }

    // Perda de conexao: qualquer estado ocioso volta para a tela de Wi-Fi.
    if (!net.isConnected() && (g_state == STATE_IDLE)) {
        setState(STATE_WIFI);
    }

    switch (g_state) {
        case STATE_WIFI:       onWifiState();  break;
        case STATE_IDLE:       onIdle();       break;
        case STATE_LISTENING:  onListening();  break;
        case STATE_PROCESSING: onProcessing(); break;
        case STATE_SPEAKING:   onSpeaking();   break;
        case STATE_ERROR:      onError();      break;
        default:               setState(STATE_IDLE); break;
    }

    delay(1);   // cede a CPU para o Wi-Fi e para a task do display
}
