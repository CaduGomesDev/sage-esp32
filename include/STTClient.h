// =============================================================================
//  STTClient.h - Speech to Text via Groq (whisper-large-v3)
//
//  Envia o WAV da PSRAM como multipart/form-data e devolve a transcricao.
// =============================================================================
#pragma once

#include <Arduino.h>

#include "AudioBuffer.h"
#include "config.h"

class STTClient {
public:
    // Retorna true e preenche `outText` em caso de sucesso.
    // Em caso de falha, `outError` traz uma mensagem curta em PT-BR.
    bool transcribe(const AudioBuffer& wavAudio, String& outText, String& outError);

    void setLanguage(const char* lang) { _language = lang; }

    // Dica de contexto opcional (nomes proprios, jargao) para o Whisper.
    void setPrompt(const String& p) { _prompt = p; }

    uint32_t lastLatencyMs() const { return _lastLatencyMs; }

private:
    const char* _language      = GROQ_STT_LANGUAGE;
    String      _prompt;
    uint32_t    _lastLatencyMs = 0;
};
