#include "AudioRecorder.h"

#include <math.h>
#include <string.h>

// =============================================================================
//  Inicializacao do driver I2S (RX)
// =============================================================================

bool AudioRecorder::begin() {
    if (_ready) return true;

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE_HZ;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;   // INMP441 = 24 bits em slot de 32
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;   // pino L/R do mic em GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = MIC_DMA_BUF_COUNT;
    cfg.dma_buf_len          = MIC_DMA_BUF_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[Rec] ERRO i2s_driver_install: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_MIC_SCK;
    pins.ws_io_num    = PIN_MIC_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_MIC_SD;

    err = i2s_set_pin(MIC_I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[Rec] ERRO i2s_set_pin: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(MIC_I2S_PORT);
        return false;
    }

    // Buffers de trabalho: pequenos, ficam na RAM interna (acesso mais rapido).
    _rawBlock = (int32_t*)malloc(MIC_BLOCK_SAMPLES * sizeof(int32_t));
    _pcm      = (int16_t*)malloc(MIC_BLOCK_SAMPLES * sizeof(int16_t));

    // Pre-roll: ~320 ms na PSRAM.
    _prerollCap = (size_t)SAMPLE_RATE_HZ * VAD_PREROLL_MS / 1000;
    _preroll    = (int16_t*)ps_malloc(_prerollCap * sizeof(int16_t));
    if (!_preroll) _preroll = (int16_t*)malloc(_prerollCap * sizeof(int16_t));

    if (!_rawBlock || !_pcm || !_preroll) {
        Serial.println(F("[Rec] ERRO: sem memoria para os buffers de captura."));
        end();
        return false;
    }

    _prerollHead = 0;
    _prerollLen  = 0;

    i2s_zero_dma_buffer(MIC_I2S_PORT);
    _ready = true;

    Serial.printf("[Rec] Microfone I2S pronto (%d Hz, 16 bits, mono) "
                  "SCK=%d WS=%d SD=%d\n",
                  SAMPLE_RATE_HZ, PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);
    return true;
}

void AudioRecorder::end() {
    if (_ready) i2s_driver_uninstall(MIC_I2S_PORT);
    _ready = false;

    free(_rawBlock); _rawBlock = nullptr;
    free(_pcm);      _pcm      = nullptr;
    free(_preroll);  _preroll  = nullptr;
    _prerollCap = _prerollHead = _prerollLen = 0;
}

// =============================================================================
//  Leitura e conversao
// =============================================================================

size_t AudioRecorder::readBlock() {
    if (!_ready) return 0;

    size_t bytesRead = 0;
    const esp_err_t err = i2s_read(MIC_I2S_PORT, _rawBlock,
                                   MIC_BLOCK_SAMPLES * sizeof(int32_t),
                                   &bytesRead, pdMS_TO_TICKS(200));
    if (err != ESP_OK || bytesRead == 0) return 0;

    const size_t n = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < n; i++) {
        // 24 bits alinhados a esquerda -> desloca para a faixa de int16
        int32_t v = _rawBlock[i] >> MIC_SHIFT_BITS;
        v = (int32_t)(v * MIC_DIGITAL_GAIN);
        if (v >  32767) v =  32767;      // saturacao (evita wrap-around audivel)
        if (v < -32768) v = -32768;
        _pcm[i] = (int16_t)v;
    }
    return n;
}

float AudioRecorder::rmsOf(const int16_t* s, size_t n) const {
    if (n == 0) return 0.0f;
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = s[i];
        acc += (uint64_t)(v * v);
    }
    return sqrtf((float)acc / (float)n);
}

float AudioRecorder::normalize(float rms) const {
    // Escala logaritmica: a fala humana ocupa uma faixa dinamica grande e
    // uma escala linear deixaria o VU quase sempre no chao.
    const float floorRms = max(_noiseFloor, 50.0f);
    if (rms <= floorRms) return 0.0f;
    const float db = 20.0f * log10f(rms / floorRms);   // 0 dB .. ~35 dB
    return constrain(db / 30.0f, 0.0f, 1.0f);
}

float AudioRecorder::poll() {
    const size_t n = readBlock();
    if (n == 0) return 0.0f;

    _lastRms = rmsOf(_pcm, n);
    pushPreroll(_pcm, n);

    const float trigger = max(_noiseFloor * VAD_TRIGGER_FACTOR, VAD_MIN_RMS);
    _voiceActive = (_lastRms > trigger);

    return normalize(_lastRms);
}

void AudioRecorder::flush() {
    if (!_ready) return;
    i2s_zero_dma_buffer(MIC_I2S_PORT);
    _prerollLen  = 0;
    _prerollHead = 0;
    _voiceActive = false;

    // Consome o que ainda estiver no caminho para nao gravar o eco do TTS.
    for (int i = 0; i < MIC_DMA_BUF_COUNT; i++) readBlock();
}

// =============================================================================
//  Pre-roll (ring buffer)
// =============================================================================

void AudioRecorder::pushPreroll(const int16_t* s, size_t n) {
    if (!_preroll || _prerollCap == 0) return;

    if (n >= _prerollCap) {                   // bloco maior que o ring
        memcpy(_preroll, s + (n - _prerollCap), _prerollCap * sizeof(int16_t));
        _prerollHead = 0;
        _prerollLen  = _prerollCap;
        return;
    }

    const size_t firstPart = min(n, _prerollCap - _prerollHead);
    memcpy(_preroll + _prerollHead, s, firstPart * sizeof(int16_t));
    if (n > firstPart) {
        memcpy(_preroll, s + firstPart, (n - firstPart) * sizeof(int16_t));
    }
    _prerollHead = (_prerollHead + n) % _prerollCap;
    _prerollLen  = min(_prerollLen + n, _prerollCap);
}

void AudioRecorder::drainPreroll(AudioBuffer& out) {
    if (!_preroll || _prerollLen == 0) return;

    const size_t start = (_prerollHead + _prerollCap - _prerollLen) % _prerollCap;
    const size_t first = min(_prerollLen, _prerollCap - start);

    out.append(_preroll + start, first * sizeof(int16_t));
    if (_prerollLen > first) {
        out.append(_preroll, (_prerollLen - first) * sizeof(int16_t));
    }

    _prerollLen  = 0;
    _prerollHead = 0;
}

// =============================================================================
//  Gravacao
// =============================================================================

RecordResult AudioRecorder::record(AudioBuffer& out, const RecordOptions& opt,
                                   LevelCallback onLevel, AbortCallback abortIf) {
    if (!_ready)        return REC_NOT_READY;
    if (!out.isValid()) return REC_NOT_READY;

    out.clear();
    if (opt.writeWavHeader && !out.reserveHeader(WAV_HEADER_SIZE)) return REC_NOT_READY;

    const size_t audioStart = out.size();
    drainPreroll(out);                    // nao perde a primeira silaba

    const float release = max(_noiseFloor * VAD_RELEASE_FACTOR, VAD_MIN_RMS * 0.6f);

    const uint32_t startMs = millis();
    uint32_t lastVoiceMs   = startMs;
    uint32_t speechMs      = 0;
    bool     bufferFull    = false;

    Serial.printf("[Rec] Gravando (max %u ms, piso de ruido %.0f)...\n",
                  (unsigned)opt.maxMs, _noiseFloor);

    while (true) {
        const uint32_t now     = millis();
        const uint32_t elapsed = now - startMs;

        if (elapsed >= opt.maxMs) {
            Serial.println(F("[Rec] Tempo maximo atingido."));
            break;
        }
        if (abortIf && abortIf()) {
            Serial.println(F("[Rec] Encerrado pelo chamador."));
            break;
        }

        const size_t n = readBlock();
        if (n == 0) continue;

        if (!out.append(_pcm, n * sizeof(int16_t))) {
            bufferFull = true;
            Serial.println(F("[Rec] Buffer cheio."));
            break;
        }

        const float rms = rmsOf(_pcm, n);
        _lastRms = rms;

        if (onLevel) onLevel(normalize(rms), elapsed);

        if (rms > release) {
            lastVoiceMs = now;
            speechMs   += (size_t)(n * 1000UL / SAMPLE_RATE_HZ);
        } else if (opt.useVad && speechMs >= opt.minSpeechMs &&
                   (now - lastVoiceMs) >= opt.silenceMs) {
            Serial.println(F("[Rec] Silencio detectado, encerrando."));
            break;
        }
    }

    const size_t pcmBytes = out.size() - audioStart;

    if (opt.writeWavHeader) {
        wav::writeHeader(out.data(), (uint32_t)pcmBytes, SAMPLE_RATE_HZ, 1, 16);
    }

    const uint32_t durationMs = (uint32_t)(pcmBytes / 2UL * 1000UL / SAMPLE_RATE_HZ);
    Serial.printf("[Rec] %u bytes (%.2f s de audio, fala util %u ms)\n",
                  (unsigned)out.size(), durationMs / 1000.0f, (unsigned)speechMs);

    if (bufferFull)                     return REC_BUFFER_FULL;
    if (speechMs < opt.minSpeechMs)     return REC_TOO_SHORT;
    return REC_OK;
}

// =============================================================================
//  Calibracao do piso de ruido
// =============================================================================

void AudioRecorder::calibrateNoiseFloor(uint32_t durationMs) {
    if (!_ready) return;

    Serial.printf("[Rec] Calibrando ruido ambiente por %u ms (fique em silencio)...\n",
                  (unsigned)durationMs);

    i2s_zero_dma_buffer(MIC_I2S_PORT);
    for (int i = 0; i < 4; i++) readBlock();   // descarta o transitorio inicial

    const uint32_t start = millis();
    double sum   = 0.0;
    uint32_t cnt = 0;
    float peak   = 0.0f;

    while (millis() - start < durationMs) {
        const size_t n = readBlock();
        if (n == 0) continue;
        const float rms = rmsOf(_pcm, n);
        sum += rms;
        peak = max(peak, rms);
        cnt++;
    }

    if (cnt == 0) {
        Serial.println(F("[Rec] AVISO: calibracao sem amostras. Verifique a fiacao do mic."));
        _noiseFloor = VAD_MIN_RMS;
        return;
    }

    // Media + margem para o pico, com um piso minimo de seguranca.
    const float mean = (float)(sum / cnt);
    _noiseFloor = max(mean * 1.2f + (peak - mean) * 0.3f, 40.0f);

    Serial.printf("[Rec] Piso de ruido = %.0f (medio %.0f | pico %.0f) | "
                  "disparo em %.0f\n",
                  _noiseFloor, mean, peak,
                  max(_noiseFloor * VAD_TRIGGER_FACTOR, VAD_MIN_RMS));
}

const char* AudioRecorder::resultName(RecordResult r) {
    switch (r) {
        case REC_OK:          return "OK";
        case REC_TOO_SHORT:   return "audio muito curto";
        case REC_BUFFER_FULL: return "buffer cheio";
        case REC_NOT_READY:   return "gravador nao inicializado";
        case REC_I2S_ERROR:   return "erro de I2S";
        default:              return "desconhecido";
    }
}
