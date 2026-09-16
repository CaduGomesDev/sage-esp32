// =============================================================================
//  HttpUtils.h - Infraestrutura comum dos clientes HTTPS
//
//  MultipartStream: envia "prologo + binario grande (PSRAM) + epilogo" sem
//  montar uma copia concatenada na memoria. O HTTPClient consome esse Stream
//  em blocos de 1460 bytes, entao o WAV de 400 KB nunca e duplicado.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClientSecure.h>

namespace httpx {

// Configura o cliente TLS. Por padrao usa setInsecure() (sem validar o
// certificado do servidor) para manter o projeto simples; veja o README para
// fixar as CAs raiz em producao.
void configureTls(WiFiClientSecure& client, uint32_t timeoutMs);

// Mensagem legivel para os codigos negativos do HTTPClient.
String errorToString(int code);

// Le todo o corpo da resposta como String, com limite de tamanho.
String readBody(HTTPClient& http, size_t maxBytes = 8192);

} // namespace httpx

// -----------------------------------------------------------------------------
//  Stream de corpo multipart/form-data
// -----------------------------------------------------------------------------
class MultipartStream : public Stream {
public:
    MultipartStream(const String& prologue, const uint8_t* body, size_t bodyLen,
                    const String& epilogue);

    size_t totalSize() const { return _total; }

    // --- Stream ---------------------------------------------------------------
    int    available() override;
    int    read() override;
    int    peek() override;
    size_t readBytes(char* buffer, size_t length) override;
    size_t readBytes(uint8_t* buffer, size_t length) override;

    // --- Print (nao usamos escrita) -------------------------------------------
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
    void   flush() override {}

private:
    uint8_t byteAt(size_t index) const;

    String         _prologue;
    const uint8_t* _body;
    size_t         _bodyLen;
    String         _epilogue;
    size_t         _pos   = 0;
    size_t         _total = 0;
};
