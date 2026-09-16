#include "LLMClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "HttpUtils.h"

// =============================================================================
//  Historico
// =============================================================================

void LLMClient::resetHistory() {
    _history.clear();
    Serial.println(F("[LLM] Historico da conversa reiniciado."));
}

void LLMClient::pushTurn(bool isUser, const String& text) {
    _history.push_back({isUser, text});
    // 1 turno = pergunta + resposta -> 2 entradas.
    while (_history.size() > (size_t)(LLM_HISTORY_TURNS * 2)) {
        _history.erase(_history.begin());
    }
}

// =============================================================================
//  Montagem do corpo JSON
// =============================================================================

String LLMClient::buildRequestBody(const String& question) const {
    JsonDocument doc;

    // System prompt (instrucao de sistema separada do historico)
    doc["system_instruction"]["parts"][0]["text"] = _systemPrompt;

    JsonArray contents = doc["contents"].to<JsonArray>();

    for (const Turn& t : _history) {
        JsonObject turn = contents.add<JsonObject>();
        turn["role"] = t.isUser ? "user" : "model";
        turn["parts"][0]["text"] = t.text;
    }

    JsonObject current = contents.add<JsonObject>();
    current["role"] = "user";
    current["parts"][0]["text"] = question;

    JsonObject gen = doc["generationConfig"].to<JsonObject>();
    gen["temperature"]      = LLM_TEMPERATURE;
    gen["maxOutputTokens"]  = LLM_MAX_OUTPUT_TOKENS;
    gen["topP"]             = 0.95;
    gen["candidateCount"]   = 1;

    String out;
    serializeJson(doc, out);
    return out;
}

// =============================================================================
//  Chamada
// =============================================================================

bool LLMClient::ask(const String& question, String& outAnswer, String& outError) {
    outAnswer = "";
    outError  = "";

    if (WiFi.status() != WL_CONNECTED) {
        outError = F("sem Wi-Fi");
        return false;
    }
    if (question.isEmpty()) {
        outError = F("pergunta vazia");
        return false;
    }

    const uint32_t t0 = millis();

    String url = String(GEMINI_API_BASE) + GEMINI_MODEL + ":generateContent?key=" + GEMINI_API_KEY;
    const String body = buildRequestBody(question);

    Serial.printf("[LLM] %s | %u bytes de prompt | %u turnos no historico\n",
                  GEMINI_MODEL, (unsigned)body.length(), (unsigned)_history.size());

    WiFiClientSecure client;
    httpx::configureTls(client, LLM_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(LLM_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, url)) {
        outError = F("nao foi possivel abrir a conexao");
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    const int code = http.POST((uint8_t*)body.c_str(), body.length());
    _lastLatencyMs = millis() - t0;

    if (code != HTTP_CODE_OK) {
        const String detail = (code > 0) ? httpx::readBody(http, 512) : String();
        http.end();
        Serial.printf("[LLM] FALHA (%d) em %u ms: %s\n", code,
                      (unsigned)_lastLatencyMs, detail.c_str());
        if (code == 400)      outError = F("LLM: requisicao rejeitada (chave?)");
        else if (code == 403) outError = F("LLM: chave Gemini sem permissao");
        else if (code == 429) outError = F("LLM: limite de uso atingido");
        else                  outError = String(F("LLM: ")) + httpx::errorToString(code);
        return false;
    }

    const String payload = httpx::readBody(http, 16384);
    http.end();

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        outError = F("LLM: resposta invalida");
        Serial.printf("[LLM] JSON invalido: %s\n", err.c_str());
        return false;
    }

    if (doc["error"].is<JsonObject>()) {
        const char* msg = doc["error"]["message"];
        outError = String(F("LLM: ")) + (msg ? msg : "erro da API");
        return false;
    }

    // Conteudo bloqueado pelos filtros de seguranca
    if (doc["promptFeedback"]["blockReason"].is<const char*>()) {
        outError = F("LLM: pergunta bloqueada pelo filtro");
        return false;
    }

    const char* text = doc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) {
        const char* finish = doc["candidates"][0]["finishReason"];
        Serial.printf("[LLM] Sem texto na resposta (finishReason=%s)\n",
                      finish ? finish : "?");
        outError = F("LLM: resposta vazia");
        return false;
    }

    outAnswer = sanitizeForSpeech(String(text));

    pushTurn(true,  question);
    pushTurn(false, outAnswer);

    Serial.printf("[LLM] %u ms -> \"%s\"\n", (unsigned)_lastLatencyMs, outAnswer.c_str());
    return true;
}

// =============================================================================
//  Limpeza para sintese de voz
// =============================================================================

String LLMClient::sanitizeForSpeech(const String& raw) {
    String s = raw;

    s.replace("\r", " ");
    s.replace("\n", " ");
    s.replace("**", "");
    s.replace("__", "");
    s.replace("```", "");
    s.replace("`", "");
    s.replace("#", "");
    s.replace("*", "");
    s.replace("_", " ");
    s.replace(">", "");

    // Remove caracteres nao imprimiveis e emojis (mantem acentos UTF-8).
    String out;
    out.reserve(s.length());
    bool lastWasSpace = false;

    for (unsigned int i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];

        // Emojis vivem em sequencias UTF-8 de 4 bytes (U+1F300 e acima).
        if (c >= 0xF0) { i += 3; continue; }

        if (c < 0x20) continue;                 // controle

        const bool isSpace = (c == ' ');
        if (isSpace && lastWasSpace) continue;  // colapsa espacos
        lastWasSpace = isSpace;

        out += (char)c;
    }

    out.trim();
    return out;
}
