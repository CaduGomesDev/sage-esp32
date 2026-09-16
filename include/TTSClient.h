// =============================================================================
//  TTSClient.h - Text to Speech (ElevenLabs ou Google Cloud TTS)
//
//  O provedor e escolhido em tempo de compilacao por TTS_PROVIDER (config.h).
//
//  Gemini TTS  -> PCM L16 24 kHz em base64 dentro do JSON do generateContent.
//                 Usa a mesma chave do LLM e funciona no plano gratuito.
//  ElevenLabs  -> output_format=pcm_16000, resposta e PCM 16 bits cru.
//  Google TTS  -> LINEAR16 em base64 dentro de um JSON.
//
//  Nos dois casos com base64, a decodificacao acontece em streaming: a string
//  base64 (1,33x o tamanho do audio) nunca e materializada na memoria.
//  Em todos os casos o audio cai direto no AudioBuffer da PSRAM.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <functional>

#include "AudioBuffer.h"
#include "config.h"

class TTSClient {
public:
    // progress: bytes recebidos ate agora (para animar o display)
    using ProgressCallback = std::function<void(size_t bytesReceived)>;

    bool synthesize(const String& text, AudioBuffer& out, String& outError,
                    ProgressCallback onProgress = nullptr);

    void setVoiceId(const String& id) { _voiceId = id; }
    uint32_t lastLatencyMs() const { return _lastLatencyMs; }

    // Taxa de amostragem do audio produzido pelo provedor configurado.
    static uint32_t outputSampleRate() { return TTS_SAMPLE_RATE; }

private:
    bool synthesizeElevenLabs(const String& text, AudioBuffer& out,
                              String& outError, ProgressCallback onProgress);
    bool synthesizeGoogle(const String& text, AudioBuffer& out,
                          String& outError, ProgressCallback onProgress);
    bool synthesizeGemini(const String& text, AudioBuffer& out,
                          String& outError, ProgressCallback onProgress);

    String   _voiceId       = ELEVENLABS_VOICE_ID;
    uint32_t _lastLatencyMs = 0;
};
