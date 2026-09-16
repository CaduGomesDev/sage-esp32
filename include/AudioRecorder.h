// =============================================================================
//  AudioRecorder.h - Captura I2S do INMP441 -> PCM 16 bits / 16 kHz / mono
//
//  - Le blocos de 32 bits (o INMP441 e um mic de 24 bits alinhado a esquerda)
//    e converte para int16 com ganho digital configuravel.
//  - VAD por energia RMS com piso de ruido calibrado no boot.
//  - Pre-roll: mantem os ultimos ~300 ms em um ring buffer para nao cortar
//    a primeira silaba da frase.
//  - Grava direto em um AudioBuffer da PSRAM, ja com cabecalho WAV.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <functional>

#include "AudioBuffer.h"
#include "config.h"

struct RecordOptions {
    bool     useVad      = true;                        // encerra no silencio
    uint32_t maxMs       = REC_MAX_SECONDS * 1000UL;
    uint32_t silenceMs   = VAD_SILENCE_MS;
    uint32_t minSpeechMs = VAD_MIN_SPEECH_MS;
    bool     writeWavHeader = true;
};

enum RecordResult : uint8_t {
    REC_OK = 0,
    REC_TOO_SHORT,      // usuario nao falou o suficiente
    REC_BUFFER_FULL,    // atingiu o limite da PSRAM reservada
    REC_NOT_READY,      // driver I2S nao inicializado
    REC_I2S_ERROR
};

class AudioRecorder {
public:
    // level: 0.0..1.0 (para o VU do display) | elapsedMs: tempo de gravacao
    using LevelCallback = std::function<void(float level, uint32_t elapsedMs)>;
    using AbortCallback = std::function<bool()>;   // true = parar agora

    bool begin();
    void end();
    bool isReady() const { return _ready; }

    // Mede o ruido ambiente. Chame com o ambiente em silencio.
    void calibrateNoiseFloor(uint32_t durationMs = VAD_CALIBRATION_MS);
    float noiseFloor() const { return _noiseFloor; }
    void  setNoiseFloor(float rms) { _noiseFloor = rms; }

    // Le um bloco do microfone e devolve o nivel normalizado (0..1).
    // Use no estado IDLE para alimentar a animacao e o gatilho de voz.
    float poll();

    // true se o ultimo poll() passou do limiar de disparo.
    bool voiceDetected() const { return _voiceActive; }

    // Descarta o audio acumulado nos buffers DMA (evita eco do proprio TTS).
    void flush();

    RecordResult record(AudioBuffer& out,
                        const RecordOptions& opt   = RecordOptions(),
                        LevelCallback        onLevel = nullptr,
                        AbortCallback        abortIf = nullptr);

    static const char* resultName(RecordResult r);

private:
    size_t readBlock();                 // -> numero de amostras em _pcm
    float  rmsOf(const int16_t* s, size_t n) const;
    float  normalize(float rms) const;  // rms -> 0..1 para o display
    void   pushPreroll(const int16_t* s, size_t n);
    void   drainPreroll(AudioBuffer& out);

    bool     _ready       = false;
    int32_t* _rawBlock    = nullptr;    // amostras cruas de 32 bits
    int16_t* _pcm         = nullptr;    // amostras convertidas
    float    _noiseFloor  = VAD_MIN_RMS;
    float    _lastRms     = 0.0f;
    bool     _voiceActive = false;

    // Ring buffer de pre-roll
    int16_t* _preroll     = nullptr;
    size_t   _prerollCap  = 0;          // em amostras
    size_t   _prerollHead = 0;
    size_t   _prerollLen  = 0;
};
