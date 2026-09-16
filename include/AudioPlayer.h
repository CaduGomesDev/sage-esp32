// =============================================================================
//  AudioPlayer.h - Reproducao PCM 16 bits no MAX98357A via I2S
//
//  Aceita tanto PCM cru (ElevenLabs pcm_16000) quanto WAV completo
//  (Google Cloud TTS LINEAR16) - o cabecalho e detectado automaticamente.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <functional>

#include "AudioBuffer.h"
#include "config.h"

class AudioPlayer {
public:
    // progress: 0.0..1.0 | level: amplitude media do trecho (0..1)
    using ProgressCallback = std::function<void(float progress, float level)>;

    bool begin(uint32_t sampleRate = TTS_SAMPLE_RATE);
    void end();
    bool isReady() const { return _ready; }

    bool  setSampleRate(uint32_t hz);
    void  setVolume(float v);              // 0.0 .. 1.0
    float volume() const { return _volume; }

    // Detecta WAV (e sua taxa de amostragem) ou trata como PCM cru.
    bool play(const AudioBuffer& buffer, ProgressCallback onProgress = nullptr);

    bool playPcm16(const int16_t* samples, size_t count,
                   ProgressCallback onProgress = nullptr);

    // Bipe senoidal de confirmacao (gerado em tempo real, sem buffer extra).
    void beep(uint16_t freqHz = BEEP_FREQ_HZ, uint16_t durationMs = BEEP_DURATION_MS);

    // Zera os buffers DMA para nao deixar residuo audivel no amplificador.
    void silence();

private:
    bool     _ready      = false;
    uint32_t _sampleRate = TTS_SAMPLE_RATE;
    float    _volume     = SPK_DEFAULT_VOLUME;
    int16_t* _chunk      = nullptr;     // buffer de trabalho (RAM interna)
    size_t   _chunkSize  = 0;           // em amostras
};
