#include "AudioPlayer.h"

#include <math.h>
#include <string.h>

namespace {
constexpr size_t CHUNK_SAMPLES = 512;     // 1 KB por escrita no I2S
} // namespace

// =============================================================================
//  Inicializacao do driver I2S (TX)
// =============================================================================

bool AudioPlayer::begin(uint32_t sampleRate) {
    if (_ready) return true;

    _sampleRate = sampleRate;

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = sampleRate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;   // fonte mono
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = SPK_DMA_BUF_COUNT;
    cfg.dma_buf_len          = SPK_DMA_BUF_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;    // silencio em vez de ruido no underrun
    cfg.fixed_mclk           = 0;

    esp_err_t err = i2s_driver_install(SPK_I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[Play] ERRO i2s_driver_install: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_SPK_BCLK;
    pins.ws_io_num    = PIN_SPK_LRC;
    pins.data_out_num = PIN_SPK_DIN;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(SPK_I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[Play] ERRO i2s_set_pin: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(SPK_I2S_PORT);
        return false;
    }

    _chunk     = (int16_t*)malloc(CHUNK_SAMPLES * sizeof(int16_t));
    _chunkSize = CHUNK_SAMPLES;
    if (!_chunk) {
        Serial.println(F("[Play] ERRO: sem memoria para o buffer de saida."));
        i2s_driver_uninstall(SPK_I2S_PORT);
        return false;
    }

    i2s_zero_dma_buffer(SPK_I2S_PORT);
    _ready = true;

    Serial.printf("[Play] Alto-falante I2S pronto (%u Hz) BCLK=%d LRC=%d DIN=%d\n",
                  (unsigned)sampleRate, PIN_SPK_BCLK, PIN_SPK_LRC, PIN_SPK_DIN);
    return true;
}

void AudioPlayer::end() {
    if (_ready) {
        i2s_zero_dma_buffer(SPK_I2S_PORT);
        i2s_driver_uninstall(SPK_I2S_PORT);
    }
    _ready = false;
    free(_chunk);
    _chunk     = nullptr;
    _chunkSize = 0;
}

bool AudioPlayer::setSampleRate(uint32_t hz) {
    if (!_ready || hz == _sampleRate) return true;

    const esp_err_t err = i2s_set_clk(SPK_I2S_PORT, hz,
                                      I2S_BITS_PER_SAMPLE_16BIT,
                                      I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[Play] ERRO i2s_set_clk(%u): %s\n", (unsigned)hz,
                      esp_err_to_name(err));
        return false;
    }
    _sampleRate = hz;
    Serial.printf("[Play] Taxa de amostragem ajustada para %u Hz\n", (unsigned)hz);
    return true;
}

void AudioPlayer::setVolume(float v) {
    _volume = constrain(v, 0.0f, 1.0f);
}

void AudioPlayer::silence() {
    if (_ready) i2s_zero_dma_buffer(SPK_I2S_PORT);
}

// =============================================================================
//  Reproducao
// =============================================================================

bool AudioPlayer::play(const AudioBuffer& buffer, ProgressCallback onProgress) {
    if (!_ready || !buffer.isValid() || buffer.size() == 0) return false;

    size_t   offset = 0;
    size_t   bytes  = buffer.size();
    uint32_t rate   = TTS_SAMPLE_RATE;

    size_t   wavOffset = 0, wavSize = 0;
    uint32_t wavRate   = 0;

    if (wav::parseHeader(buffer.data(), buffer.size(), wavOffset, wavSize, wavRate)) {
        offset = wavOffset;
        bytes  = wavSize;
        rate   = wavRate;
        Serial.printf("[Play] WAV detectado: %u Hz, %u bytes de PCM\n",
                      (unsigned)rate, (unsigned)bytes);
    } else {
        Serial.printf("[Play] PCM cru: %u bytes a %u Hz\n",
                      (unsigned)bytes, (unsigned)rate);
    }

    setSampleRate(rate);

    return playPcm16(reinterpret_cast<const int16_t*>(buffer.data() + offset),
                     bytes / 2, onProgress);
}

bool AudioPlayer::playPcm16(const int16_t* samples, size_t count,
                            ProgressCallback onProgress) {
    if (!_ready || !samples || count == 0) return false;

    const uint32_t t0 = millis();
    size_t played = 0;

    while (played < count) {
        const size_t n = min(_chunkSize, count - played);

        // Copia PSRAM -> RAM interna aplicando volume. O DMA do I2S le sempre
        // de RAM interna, entao a copia e obrigatoria de qualquer forma.
        uint32_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)(samples[played + i] * _volume);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            _chunk[i] = (int16_t)v;
            acc += (uint32_t)abs((int)v);
        }

        size_t bytesWritten = 0;
        const esp_err_t err = i2s_write(SPK_I2S_PORT, _chunk, n * sizeof(int16_t),
                                        &bytesWritten, portMAX_DELAY);
        if (err != ESP_OK) {
            Serial.printf("[Play] ERRO i2s_write: %s\n", esp_err_to_name(err));
            return false;
        }

        played += bytesWritten / sizeof(int16_t);

        if (onProgress) {
            const float level = (float)(acc / n) / 12000.0f;
            onProgress((float)played / (float)count, constrain(level, 0.0f, 1.0f));
        }
    }

    // Deixa o DMA esvaziar antes de zerar, senao o final da frase e cortado.
    i2s_zero_dma_buffer(SPK_I2S_PORT);

    Serial.printf("[Play] %u amostras reproduzidas em %u ms\n",
                  (unsigned)played, (unsigned)(millis() - t0));
    return true;
}

void AudioPlayer::beep(uint16_t freqHz, uint16_t durationMs) {
    if (!_ready) return;

    const size_t total = (size_t)_sampleRate * durationMs / 1000;
    const float  step  = 2.0f * PI * freqHz / (float)_sampleRate;
    const size_t fade  = min<size_t>(total / 4, _sampleRate / 100);  // ~10 ms

    float phase = 0.0f;
    size_t done = 0;

    while (done < total) {
        const size_t n = min(_chunkSize, total - done);
        for (size_t i = 0; i < n; i++) {
            const size_t idx = done + i;
            // Rampa de entrada/saida para nao estalar o alto-falante.
            float env = 1.0f;
            if (idx < fade)              env = (float)idx / fade;
            else if (idx > total - fade) env = (float)(total - idx) / fade;

            _chunk[i] = (int16_t)(sinf(phase) * 8000.0f * env * _volume);
            phase += step;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;
        }

        size_t written = 0;
        i2s_write(SPK_I2S_PORT, _chunk, n * sizeof(int16_t), &written, portMAX_DELAY);
        done += n;
    }

    i2s_zero_dma_buffer(SPK_I2S_PORT);
}
