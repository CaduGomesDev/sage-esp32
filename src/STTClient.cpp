#include "STTClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "HttpUtils.h"

namespace {

const char* BOUNDARY = "----SageBoundary7MA4YWxkTrZu0gW";

// Monta um campo de texto do multipart.
void addField(String& out, const char* name, const String& value) {
    out += "--";
    out += BOUNDARY;
    out += "\r\nContent-Disposition: form-data; name=\"";
    out += name;
    out += "\"\r\n\r\n";
    out += value;
    out += "\r\n";
}

} // namespace

bool STTClient::transcribe(const AudioBuffer& wavAudio, String& outText,
                           String& outError) {
    outText = "";
    outError = "";

    if (WiFi.status() != WL_CONNECTED) {
        outError = F("sem Wi-Fi");
        return false;
    }
    if (!wavAudio.isValid() || wavAudio.size() <= WAV_HEADER_SIZE) {
        outError = F("audio vazio");
        return false;
    }

    const uint32_t t0 = millis();

    // ---- Corpo multipart: prologo + WAV (PSRAM) + epilogo -------------------
    String prologue;
    prologue.reserve(512);
    addField(prologue, "model", F(GROQ_STT_MODEL));
    addField(prologue, "response_format", F("json"));
    addField(prologue, "temperature", F("0"));
    if (_language && strlen(_language)) addField(prologue, "language", String(_language));
    if (_prompt.length())               addField(prologue, "prompt", _prompt);

    prologue += "--";
    prologue += BOUNDARY;
    prologue += "\r\nContent-Disposition: form-data; name=\"file\"; "
                "filename=\"audio.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";

    String epilogue = "\r\n--";
    epilogue += BOUNDARY;
    epilogue += "--\r\n";

    MultipartStream body(prologue, wavAudio.data(), wavAudio.size(), epilogue);

    Serial.printf("[STT] Enviando %u bytes de audio para o Groq (%s)...\n",
                  (unsigned)wavAudio.size(), GROQ_STT_MODEL);

    // ---- Requisicao ---------------------------------------------------------
    WiFiClientSecure client;
    httpx::configureTls(client, STT_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(STT_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, GROQ_STT_URL)) {
        outError = F("nao foi possivel abrir a conexao");
        return false;
    }

    http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);
    http.addHeader("Accept", "application/json");

    const int code = http.sendRequest("POST", &body, body.totalSize());
    _lastLatencyMs = millis() - t0;

    if (code != HTTP_CODE_OK) {
        const String detail = (code > 0) ? httpx::readBody(http, 512) : String();
        http.end();
        outError = String(F("STT: ")) + httpx::errorToString(code);
        Serial.printf("[STT] FALHA (%d) em %u ms: %s\n", code,
                      (unsigned)_lastLatencyMs, detail.c_str());
        if (code == 401) outError = F("STT: chave Groq invalida");
        if (code == 429) outError = F("STT: limite de uso atingido");
        return false;
    }

    const String payload = httpx::readBody(http, 8192);
    http.end();

    // ---- Parse --------------------------------------------------------------
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        outError = F("STT: resposta invalida");
        Serial.printf("[STT] JSON invalido: %s\n", err.c_str());
        return false;
    }

    if (doc["error"].is<JsonObject>()) {
        outError = String(F("STT: ")) + (const char*)doc["error"]["message"];
        return false;
    }

    outText = doc["text"].as<const char*>() ? doc["text"].as<const char*>() : "";
    outText.trim();

    if (outText.isEmpty()) {
        outError = F("nao entendi nada");
        Serial.println(F("[STT] Transcricao vazia."));
        return false;
    }

    Serial.printf("[STT] %u ms -> \"%s\"\n", (unsigned)_lastLatencyMs, outText.c_str());
    return true;
}
