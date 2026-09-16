#include "HttpUtils.h"

#include <string.h>

// =============================================================================
//  httpx
// =============================================================================

void httpx::configureTls(WiFiClientSecure& client, uint32_t timeoutMs) {
    // Sem validacao de certificado: evita ter que embarcar e manter atualizadas
    // as CAs raiz de tres provedores diferentes. Para producao, troque por
    // client.setCACert(<root_ca_pem>) e sincronize o relogio via NTP.
    client.setInsecure();

    client.setTimeout(timeoutMs / 1000);        // WiFiClient conta em segundos

    // Reduz o buffer de RX do mbedTLS: o padrao (16 KB) pesa demais na RAM
    // interna do ESP32-S3, que tambem precisa dela para o driver Wi-Fi.
    client.setHandshakeTimeout(timeoutMs / 1000);
}

String httpx::errorToString(int code) {
    if (code > 0) return String(F("HTTP ")) + code;
    switch (code) {
        case HTTPC_ERROR_CONNECTION_REFUSED: return F("conexao recusada");
        case HTTPC_ERROR_SEND_HEADER_FAILED: return F("falha ao enviar cabecalho");
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED:return F("falha ao enviar o corpo");
        case HTTPC_ERROR_NOT_CONNECTED:      return F("nao conectado");
        case HTTPC_ERROR_CONNECTION_LOST:    return F("conexao perdida");
        case HTTPC_ERROR_NO_STREAM:          return F("stream invalido");
        case HTTPC_ERROR_NO_HTTP_SERVER:     return F("servidor nao respondeu HTTP");
        case HTTPC_ERROR_TOO_LESS_RAM:       return F("memoria insuficiente");
        case HTTPC_ERROR_ENCODING:           return F("encoding nao suportado");
        case HTTPC_ERROR_STREAM_WRITE:       return F("erro de escrita no stream");
        case HTTPC_ERROR_READ_TIMEOUT:       return F("timeout de leitura");
        default:                             return String(F("erro ")) + code;
    }
}

String httpx::readBody(HTTPClient& http, size_t maxBytes) {
    const int len = http.getSize();
    if (len > 0 && (size_t)len <= maxBytes) {
        return http.getString();
    }
    if (len < 0) {                 // chunked: tamanho desconhecido
        String body = http.getString();
        if (body.length() > maxBytes) body = body.substring(0, maxBytes);
        return body;
    }
    // Resposta maior que o limite: le so o inicio (o suficiente para o erro).
    WiFiClient* stream = http.getStreamPtr();
    String out;
    out.reserve(maxBytes);
    while (stream->available() && out.length() < maxBytes) {
        out += (char)stream->read();
    }
    return out;
}

// =============================================================================
//  MultipartStream
// =============================================================================

MultipartStream::MultipartStream(const String& prologue, const uint8_t* body,
                                 size_t bodyLen, const String& epilogue)
    : _prologue(prologue), _body(body), _bodyLen(bodyLen), _epilogue(epilogue) {
    _total = _prologue.length() + _bodyLen + _epilogue.length();
}

uint8_t MultipartStream::byteAt(size_t index) const {
    const size_t pLen = _prologue.length();
    if (index < pLen)            return (uint8_t)_prologue[index];
    if (index < pLen + _bodyLen) return _body[index - pLen];
    return (uint8_t)_epilogue[index - pLen - _bodyLen];
}

int MultipartStream::available() {
    return (int)(_total - _pos);
}

int MultipartStream::read() {
    if (_pos >= _total) return -1;
    return byteAt(_pos++);
}

int MultipartStream::peek() {
    if (_pos >= _total) return -1;
    return byteAt(_pos);
}

size_t MultipartStream::readBytes(uint8_t* buffer, size_t length) {
    return readBytes(reinterpret_cast<char*>(buffer), length);
}

size_t MultipartStream::readBytes(char* buffer, size_t length) {
    if (_pos >= _total || length == 0) return 0;

    size_t remaining = min(length, _total - _pos);
    size_t written   = 0;

    const size_t pLen = _prologue.length();
    const size_t bEnd = pLen + _bodyLen;

    // 1) trecho do prologo
    if (_pos < pLen && remaining > 0) {
        const size_t n = min(remaining, pLen - _pos);
        memcpy(buffer + written, _prologue.c_str() + _pos, n);
        _pos += n; written += n; remaining -= n;
    }
    // 2) trecho binario (memcpy direto da PSRAM)
    if (_pos >= pLen && _pos < bEnd && remaining > 0) {
        const size_t n = min(remaining, bEnd - _pos);
        memcpy(buffer + written, _body + (_pos - pLen), n);
        _pos += n; written += n; remaining -= n;
    }
    // 3) trecho do epilogo
    if (_pos >= bEnd && remaining > 0) {
        const size_t n = min(remaining, _total - _pos);
        memcpy(buffer + written, _epilogue.c_str() + (_pos - bEnd), n);
        _pos += n; written += n;
    }

    return written;
}
