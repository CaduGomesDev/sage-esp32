#include "NetworkManager.h"
#include "config.h"

#include <time.h>

void NetworkManager::begin(const char* ssid, const char* password) {
    _ssid     = ssid;
    _password = password;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.setAutoReconnect(true);

    // Desliga o power save: com modem sleep ativo a latencia do TLS sobe
    // muito e o streaming de audio sofre com stalls de centenas de ms.
    WiFi.setSleep(false);

    Serial.printf("[WiFi] Conectando em \"%s\"...\n", _ssid);
    WiFi.begin(_ssid, _password);
    _lastAttemptMs = millis();
}

bool NetworkManager::connectBlocking(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        _wasConnected = true;
        _retryCount   = 0;
        Serial.printf("[WiFi] Conectado. IP: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }

    Serial.println(F("[WiFi] Falha na conexao inicial (status nao conectado)."));
    return false;
}

void NetworkManager::loop() {
    const bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected) {
        if (!_wasConnected) {
            _wasConnected = true;
            _retryCount   = 0;
            Serial.printf("[WiFi] Reconectado. IP: %s | RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        return;
    }

    if (_wasConnected) {
        _wasConnected = false;
        Serial.println(F("[WiFi] Conexao perdida. Tentando reconectar..."));
    }

    // Backoff simples: 5s, 10s, 15s ... limitado a 30s.
    const uint32_t interval =
        min<uint32_t>(WIFI_RETRY_INTERVAL_MS * (_retryCount + 1), 30000);

    if (millis() - _lastAttemptMs < interval) return;
    _lastAttemptMs = millis();

    if (_retryCount > 0 && (_retryCount % 4) == 0) {
        // A cada 4 falhas reinicia o stack Wi-Fi: resolve casos em que o
        // driver fica preso apos o roteador reiniciar.
        Serial.println(F("[WiFi] Reiniciando o stack Wi-Fi..."));
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
    }

    Serial.printf("[WiFi] Tentativa de reconexao #%u\n", (unsigned)(_retryCount + 1));
    WiFi.begin(_ssid, _password);
    if (_retryCount < 255) _retryCount++;
}

bool NetworkManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

int NetworkManager::rssi() const {
    return isConnected() ? WiFi.RSSI() : -127;
}

uint8_t NetworkManager::signalBars() const {
    if (!isConnected()) return 0;
    const int r = WiFi.RSSI();
    if (r >= -55) return 4;
    if (r >= -65) return 3;
    if (r >= -75) return 2;
    return 1;
}

String NetworkManager::ip() const {
    return isConnected() ? WiFi.localIP().toString() : String("0.0.0.0");
}

bool NetworkManager::syncTime(uint32_t timeoutMs) {
    if (!isConnected()) return false;

    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

    const uint32_t start = millis();
    struct tm tmInfo;
    while ((millis() - start) < timeoutMs) {
        if (getLocalTime(&tmInfo, 200) && tmInfo.tm_year > (2020 - 1900)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &tmInfo);
            Serial.printf("[NTP] Relogio sincronizado: %s\n", buf);
            return true;
        }
        delay(200);
    }
    Serial.println(F("[NTP] Nao foi possivel sincronizar o relogio (seguindo assim mesmo)."));
    return false;
}
