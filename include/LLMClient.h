// =============================================================================
//  LLMClient.h - Google Gemini (generateContent)
//
//  Mantem um historico curto da conversa para dar contexto, e limpa a resposta
//  de qualquer formatacao antes de mandar para o TTS.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <vector>

#include "config.h"

class LLMClient {
public:
    bool ask(const String& question, String& outAnswer, String& outError);

    void setSystemPrompt(const String& p) { _systemPrompt = p; }
    void resetHistory();
    size_t historySize() const { return _history.size(); }

    uint32_t lastLatencyMs() const { return _lastLatencyMs; }

    // Remove markdown, emojis e quebras de linha: o texto vai virar audio.
    static String sanitizeForSpeech(const String& raw);

private:
    struct Turn {
        bool   isUser;
        String text;
    };

    String buildRequestBody(const String& question) const;
    void   pushTurn(bool isUser, const String& text);

    String            _systemPrompt  = LLM_SYSTEM_PROMPT;
    std::vector<Turn> _history;
    uint32_t          _lastLatencyMs = 0;
};
