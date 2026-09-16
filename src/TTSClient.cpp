#include "TTSClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "HttpUtils.h"

namespace {

constexpr size_t NET_CHUNK = 1024;   // bloco de leitura do socket TLS

// ---------------------------------------------------------------------------
//  Decodificador base64 incremental
// ---------------------------------------------------------------------------
class Base64Decoder {
public:
    // Alimenta um caractere base64. Devolve quantos bytes escreveu em out.
    size_t push(char c, uint8_t* out) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') return 0;

        const int8_t v = valueOf(c);
        if (v < 0) return 0;                       // caractere invalido: ignora

        _acc = (_acc << 6) | (uint32_t)v;
        _bits += 6;

        if (_bits >= 8) {
            _bits -= 8;
            out[0] = (uint8_t)((_acc >> _bits) & 0xFF);
            return 1;
        }
        return 0;
    }

private:
    static int8_t valueOf(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    uint32_t _acc  = 0;
    uint8_t  _bits = 0;
};

// ---------------------------------------------------------------------------
//  Varre o corpo JSON procurando as chaves de `keys` NA ORDEM dada e decodifica
//  em base64 a string que vier depois da ultima delas.
//
//  Motivo de existir: a resposta do Gemini e do Google TTS e um JSON unico com
//  o audio inteiro em base64. Carregar esse JSON custaria ~1,33x o tamanho do
//  audio em RAM. Aqui os bytes sao decodificados conforme chegam do socket.
//
//  As chaves devem incluir as aspas, ex.: "\"inlineData\"".
// ---------------------------------------------------------------------------
bool streamBase64Value(HTTPClient& http, const char* const* keys, size_t keyCount,
                       AudioBuffer& out,
                       const TTSClient::ProgressCallback& onProgress) {
    WiFiClient* stream = http.getStreamPtr();

    size_t   keyIdx     = 0;
    size_t   matched    = 0;
    bool     seenColon  = false;
    bool     inValue    = false;
    bool     done       = false;
    bool     truncated  = false;
    uint32_t lastDataMs = millis();

    Base64Decoder decoder;
    uint8_t pending[NET_CHUNK];
    size_t  pendingLen = 0;
    uint8_t decoded;

    while (http.connected() && !done) {
        const int avail = stream->available();
        if (avail <= 0) {
            if (millis() - lastDataMs > TTS_TIMEOUT_MS) {
                Serial.println(F("[TTS] Timeout aguardando dados."));
                break;
            }
            delay(1);
            continue;
        }
        lastDataMs = millis();

        uint8_t netBuf[NET_CHUNK];
        const int n = stream->readBytes(netBuf, min((size_t)avail, sizeof(netBuf)));

        for (int i = 0; i < n; i++) {
            const char c = (char)netBuf[i];

            if (!inValue) {
                const char*  key  = keys[keyIdx];
                const size_t klen = strlen(key);

                if (matched < klen) {
                    // Casamento simples: no reinicio, so o 1o caractere conta.
                    // Basta para as chaves usadas aqui (sem prefixos repetidos).
                    matched = (c == key[matched]) ? matched + 1
                                                  : (c == key[0] ? 1 : 0);
                    if (matched == klen && keyIdx + 1 < keyCount) {
                        keyIdx++;          // achou uma chave intermediaria
                        matched = 0;
                    }
                } else if (!seenColon) {
                    if (c == ':') seenColon = true;
                } else if (c == '"') {
                    inValue = true;        // comecou o base64
                }
                continue;
            }

            if (c == '"') { done = true; break; }   // fim do base64

            if (decoder.push(c, &decoded)) {
                pending[pendingLen++] = decoded;
                if (pendingLen == sizeof(pending)) {
                    if (!out.append(pending, pendingLen)) {
                        truncated = true;
                        done = true;
                        break;
                    }
                    pendingLen = 0;
                    if (onProgress) onProgress(out.size());
                }
            }
        }
    }

    if (pendingLen) out.append(pending, pendingLen);

    if (truncated) {
        Serial.printf("[TTS] AVISO: buffer cheio, audio truncado. "
                      "Aumente TTS_MAX_SECONDS (atual: %d s)\n", TTS_MAX_SECONDS);
    }
    return out.size() > 0;
}

} // namespace

// =============================================================================
//  Entrada publica
// =============================================================================

bool TTSClient::synthesize(const String& text, AudioBuffer& out, String& outError,
                           ProgressCallback onProgress) {
    outError = "";

    if (WiFi.status() != WL_CONNECTED) {
        outError = F("sem Wi-Fi");
        return false;
    }
    if (text.isEmpty()) {
        outError = F("texto vazio");
        return false;
    }
    if (!out.isValid()) {
        outError = F("buffer de audio nao alocado");
        return false;
    }

    out.clear();
    const uint32_t t0 = millis();

#if TTS_PROVIDER == TTS_PROVIDER_GEMINI
    const bool ok = synthesizeGemini(text, out, outError, onProgress);
#elif TTS_PROVIDER == TTS_PROVIDER_GOOGLE
    const bool ok = synthesizeGoogle(text, out, outError, onProgress);
#else
    const bool ok = synthesizeElevenLabs(text, out, outError, onProgress);
#endif

    _lastLatencyMs = millis() - t0;

    if (ok) {
        const float seconds = (float)out.size() / 2.0f / (float)TTS_SAMPLE_RATE;
        Serial.printf("[TTS] %u bytes (%.1f s de audio) em %u ms\n",
                      (unsigned)out.size(), seconds, (unsigned)_lastLatencyMs);
    }
    return ok;
}

// =============================================================================
//  Gemini TTS  (mesma chave do LLM, plano gratuito)
// =============================================================================

bool TTSClient::synthesizeGemini(const String& text, AudioBuffer& out,
                                 String& outError, ProgressCallback onProgress) {
    JsonDocument doc;
    doc["contents"][0]["parts"][0]["text"] = String(F(GEMINI_TTS_STYLE)) + text;

    JsonObject gen = doc["generationConfig"].to<JsonObject>();
    gen["responseModalities"].to<JsonArray>().add("AUDIO");
    gen["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] =
        GEMINI_TTS_VOICE;

    String body;
    serializeJson(doc, body);

    const String url = String(GEMINI_API_BASE) + GEMINI_TTS_MODEL +
                       ":generateContent?key=" + GEMINI_API_KEY;

    Serial.printf("[TTS] Gemini (%s, voz %s), %u caracteres...\n",
                  GEMINI_TTS_MODEL, GEMINI_TTS_VOICE, (unsigned)text.length());

    WiFiClientSecure client;
    httpx::configureTls(client, TTS_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(TTS_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, url)) {
        outError = F("nao foi possivel abrir a conexao");
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    const int code = http.POST((uint8_t*)body.c_str(), body.length());

    if (code != HTTP_CODE_OK) {
        const String detail = (code > 0) ? httpx::readBody(http, 512) : String();
        http.end();
        Serial.printf("[TTS] FALHA (%d): %s\n", code, detail.c_str());
        if (code == 429)      outError = F("TTS: limite de uso atingido");
        else if (code == 403) outError = F("TTS: chave Gemini sem permissao");
        else if (code == 404) outError = F("TTS: modelo indisponivel");
        else                  outError = String(F("TTS: ")) + httpx::errorToString(code);
        return false;
    }

    // candidates[0].content.parts[0].inlineData.data
    static const char* const KEYS[] = {"\"inlineData\"", "\"data\""};
    const bool got = streamBase64Value(http, KEYS, 2, out, onProgress);
    http.end();

    if (!got) {
        outError = F("TTS: nenhum audio recebido");
        return false;
    }
    return true;
}

// =============================================================================
//  ElevenLabs - PCM cru
//  Obs.: o plano gratuito recusa as vozes da biblioteca via API (HTTP 402).
// =============================================================================

bool TTSClient::synthesizeElevenLabs(const String& text, AudioBuffer& out,
                                     String& outError, ProgressCallback onProgress) {
    JsonDocument doc;
    doc["text"]     = text;
    doc["model_id"] = ELEVENLABS_MODEL;
    doc["voice_settings"]["stability"]        = ELEVENLABS_STABILITY;
    doc["voice_settings"]["similarity_boost"] = ELEVENLABS_SIMILARITY;

    String body;
    serializeJson(doc, body);

    const String url = String(ELEVENLABS_URL_BASE) + _voiceId +
                       "?output_format=" + ELEVENLABS_OUTPUT_FORMAT;

    Serial.printf("[TTS] ElevenLabs (%s, voz %s), %u caracteres...\n",
                  ELEVENLABS_MODEL, _voiceId.c_str(), (unsigned)text.length());

    WiFiClientSecure client;
    httpx::configureTls(client, TTS_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(TTS_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, url)) {
        outError = F("nao foi possivel abrir a conexao");
        return false;
    }

    http.addHeader("xi-api-key", ELEVENLABS_API_KEY);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "audio/pcm");

    const int code = http.POST((uint8_t*)body.c_str(), body.length());

    if (code != HTTP_CODE_OK) {
        const String detail = (code > 0) ? httpx::readBody(http, 512) : String();
        http.end();
        Serial.printf("[TTS] FALHA (%d): %s\n", code, detail.c_str());
        if (code == 401)      outError = F("TTS: chave ElevenLabs invalida");
        else if (code == 402) outError = F("TTS: voz exige plano pago");
        else if (code == 422) outError = F("TTS: voz ou formato invalido");
        else if (code == 429) outError = F("TTS: limite de uso atingido");
        else                  outError = String(F("TTS: ")) + httpx::errorToString(code);
        return false;
    }

    // Resposta e PCM cru: basta copiar o stream para a PSRAM.
    WiFiClient* stream = http.getStreamPtr();
    const int   total  = http.getSize();          // -1 quando vem em chunks

    uint8_t  buf[NET_CHUNK];
    uint32_t lastDataMs = millis();

    while (http.connected() && (total < 0 || out.size() < (size_t)total)) {
        const int avail = stream->available();
        if (avail <= 0) {
            if (millis() - lastDataMs > TTS_TIMEOUT_MS) {
                Serial.println(F("[TTS] Timeout aguardando dados."));
                break;
            }
            delay(1);
            continue;
        }

        const int n = stream->readBytes(buf, min((size_t)avail, sizeof(buf)));
        if (n <= 0) continue;
        lastDataMs = millis();

        if (!out.append(buf, n)) {
            Serial.printf("[TTS] AVISO: buffer cheio, audio truncado. "
                          "Aumente TTS_MAX_SECONDS (atual: %d s)\n", TTS_MAX_SECONDS);
            break;
        }
        if (onProgress) onProgress(out.size());
    }

    http.end();

    if (out.size() == 0) {
        outError = F("TTS: nenhum audio recebido");
        return false;
    }
    return true;
}

// =============================================================================
//  Google Cloud TTS - WAV base64 dentro do JSON (exige billing no GCP)
// =============================================================================

bool TTSClient::synthesizeGoogle(const String& text, AudioBuffer& out,
                                 String& outError, ProgressCallback onProgress) {
    JsonDocument doc;
    doc["input"]["text"]                  = text;
    doc["voice"]["languageCode"]          = GOOGLE_TTS_LANGUAGE;
    doc["voice"]["name"]                  = GOOGLE_TTS_VOICE;
    doc["audioConfig"]["audioEncoding"]   = "LINEAR16";
    doc["audioConfig"]["sampleRateHertz"] = TTS_SAMPLE_RATE;

    String body;
    serializeJson(doc, body);

    const String url = String(GOOGLE_TTS_URL) + "?key=" + GOOGLE_TTS_API_KEY;

    Serial.printf("[TTS] Google TTS (%s), %u caracteres...\n",
                  GOOGLE_TTS_VOICE, (unsigned)text.length());

    WiFiClientSecure client;
    httpx::configureTls(client, TTS_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(TTS_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, url)) {
        outError = F("nao foi possivel abrir a conexao");
        return false;
    }
    http.addHeader("Content-Type", "application/json; charset=utf-8");

    const int code = http.POST((uint8_t*)body.c_str(), body.length());

    if (code != HTTP_CODE_OK) {
        const String detail = (code > 0) ? httpx::readBody(http, 512) : String();
        http.end();
        Serial.printf("[TTS] FALHA (%d): %s\n", code, detail.c_str());
        outError = String(F("TTS: ")) + httpx::errorToString(code);
        return false;
    }

    static const char* const KEYS[] = {"\"audioContent\""};
    const bool got = streamBase64Value(http, KEYS, 1, out, onProgress);
    http.end();

    if (!got) {
        outError = F("TTS: nenhum audio recebido");
        return false;
    }
    return true;
}
