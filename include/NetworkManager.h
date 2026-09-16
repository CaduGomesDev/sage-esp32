// =============================================================================
//  NetworkManager.h - Wi-Fi com reconexao automatica e sincronismo NTP
//
//  O nome nao e "WiFiManager" de proposito, para nao colidir com a biblioteca
//  homonima (tzapu/WiFiManager) caso voce a adicione depois.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>

class NetworkManager {
public:
    // Configura o radio e inicia a primeira tentativa de conexao.
    void begin(const char* ssid, const char* password);

    // Bloqueia ate conectar ou estourar o timeout. Use apenas no setup().
    bool connectBlocking(uint32_t timeoutMs);

    // Chamar a cada iteracao do loop(): cuida da reconexao com backoff.
    void loop();

    bool    isConnected() const;
    int     rssi() const;
    uint8_t signalBars() const;      // 0..4, para o icone do display
    String  ip() const;

    // Sincroniza o relogio via NTP (necessario se voce ativar validacao de
    // certificado TLS; tambem deixa os logs com hora real).
    bool syncTime(uint32_t timeoutMs = 8000);

private:
    const char* _ssid          = nullptr;
    const char* _password      = nullptr;
    uint32_t    _lastAttemptMs = 0;
    uint8_t     _retryCount    = 0;
    bool        _wasConnected  = false;
};
