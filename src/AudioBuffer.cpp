#include "AudioBuffer.h"

#include <esp_heap_caps.h>
#include <string.h>

// =============================================================================
//  AudioBuffer
// =============================================================================

AudioBuffer::~AudioBuffer() {
    release();
}

bool AudioBuffer::allocate(size_t bytes) {
    if (_data && _capacity >= bytes) {   // ja temos espaco suficiente
        _length = 0;
        return true;
    }
    release();

    // 1a tentativa: PSRAM (o caminho normal neste projeto)
    _data = static_cast<uint8_t*>(ps_malloc(bytes));
    _inPsram = (_data != nullptr);

    // 2a tentativa: RAM interna (util em bench sem PSRAM)
    if (!_data) {
        Serial.println(F("[AudioBuffer] PSRAM indisponivel, tentando RAM interna..."));
        _data = static_cast<uint8_t*>(malloc(bytes));
        _inPsram = false;
    }

    if (!_data) {
        Serial.printf("[AudioBuffer] ERRO: falha ao alocar %u bytes\n", (unsigned)bytes);
        _capacity = 0;
        _length   = 0;
        return false;
    }

    _capacity = bytes;
    _length   = 0;
    Serial.printf("[AudioBuffer] %u bytes alocados em %s\n",
                  (unsigned)bytes, _inPsram ? "PSRAM" : "RAM interna");
    return true;
}

void AudioBuffer::release() {
    if (_data) {
        free(_data);
        _data = nullptr;
    }
    _capacity = 0;
    _length   = 0;
    _inPsram  = false;
}

bool AudioBuffer::append(const void* src, size_t n) {
    if (!_data || n == 0)        return (n == 0);
    if (_length + n > _capacity) return false;   // buffer cheio
    memcpy(_data + _length, src, n);
    _length += n;
    return true;
}

bool AudioBuffer::reserveHeader(size_t n) {
    if (!_data || n > _capacity) return false;
    memset(_data, 0, n);
    _length = n;
    return true;
}

// =============================================================================
//  WAV
// =============================================================================
namespace {

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
inline void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}
inline uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

void wav::writeHeader(uint8_t* dst, uint32_t pcmBytes, uint32_t sampleRate,
                      uint16_t channels, uint16_t bitsPerSample) {
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t byteRate   = sampleRate * blockAlign;

    memcpy(dst + 0,  "RIFF", 4);
    put32 (dst + 4,  36 + pcmBytes);      // tamanho do arquivo - 8
    memcpy(dst + 8,  "WAVE", 4);

    memcpy(dst + 12, "fmt ", 4);
    put32 (dst + 16, 16);                 // tamanho do bloco fmt (PCM)
    put16 (dst + 20, 1);                  // formato = PCM
    put16 (dst + 22, channels);
    put32 (dst + 24, sampleRate);
    put32 (dst + 28, byteRate);
    put16 (dst + 32, blockAlign);
    put16 (dst + 34, bitsPerSample);

    memcpy(dst + 36, "data", 4);
    put32 (dst + 40, pcmBytes);
}

bool wav::parseHeader(const uint8_t* data, size_t size, size_t& dataOffset,
                      size_t& dataSize, uint32_t& sampleRate) {
    if (!data || size < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;

    size_t pos = 12;
    sampleRate = 16000;

    // Percorre os chunks ate achar "data" (Google TTS insere chunks extras).
    while (pos + 8 <= size) {
        const uint8_t* id  = data + pos;
        const uint32_t len = get32(data + pos + 4);

        if (memcmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= size) {
            sampleRate = get32(data + pos + 8 + 4);
        } else if (memcmp(id, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize   = (len == 0 || dataOffset + len > size) ? (size - dataOffset) : len;
            return true;
        }
        pos += 8 + len + (len & 1);   // chunks tem padding para tamanho par
    }
    return false;
}
