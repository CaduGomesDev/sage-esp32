// =============================================================================
//  AudioBuffer.h - Buffer de audio alocado preferencialmente na PSRAM
//
//  Toda alocacao grande do projeto (gravacao e resposta de voz) passa por aqui.
//  ps_malloc() mantem a RAM interna livre para a pilha TCP/TLS, que e o
//  recurso mais escasso do ESP32-S3 durante as chamadas HTTPS.
// =============================================================================
#pragma once

#include <Arduino.h>

class AudioBuffer {
public:
    AudioBuffer() = default;
    ~AudioBuffer();

    AudioBuffer(const AudioBuffer&)            = delete;   // buffers sao grandes:
    AudioBuffer& operator=(const AudioBuffer&) = delete;   // proibimos copia.

    // Aloca `bytes` na PSRAM. Se a PSRAM nao existir ou estiver cheia,
    // tenta a RAM interna (e avisa no Serial).
    bool   allocate(size_t bytes);
    void   release();

    void   clear()                 { _length = 0; }
    bool   append(const void* src, size_t n);
    bool   reserveHeader(size_t n);          // avanca o cursor deixando espaco

    uint8_t*       data()          { return _data; }
    const uint8_t* data() const    { return _data; }
    size_t         size() const    { return _length; }
    size_t         capacity() const{ return _capacity; }
    size_t         freeSpace() const { return _capacity - _length; }
    bool           isValid() const { return _data != nullptr; }
    bool           inPsram() const { return _inPsram; }

    // Amostras PCM 16-bit a partir de um offset em bytes.
    const int16_t* samples(size_t byteOffset = 0) const {
        return reinterpret_cast<const int16_t*>(_data + byteOffset);
    }
    size_t sampleCount(size_t byteOffset = 0) const {
        return (_length > byteOffset) ? (_length - byteOffset) / 2 : 0;
    }

private:
    uint8_t* _data     = nullptr;
    size_t   _capacity = 0;
    size_t   _length   = 0;
    bool     _inPsram  = false;
};

// -----------------------------------------------------------------------------
//  Utilitarios WAV (PCM 16-bit little-endian)
// -----------------------------------------------------------------------------
namespace wav {

// Escreve um cabecalho RIFF/WAVE de 44 bytes em `dst`.
// `pcmBytes` = quantidade de bytes de audio que vem DEPOIS do cabecalho.
void writeHeader(uint8_t* dst, uint32_t pcmBytes,
                 uint32_t sampleRate = 16000,
                 uint16_t channels   = 1,
                 uint16_t bitsPerSample = 16);

// Detecta um WAV e devolve o offset do bloco "data" e a taxa de amostragem.
// Retorna false se o buffer nao for um WAV (nesse caso trate como PCM cru).
bool parseHeader(const uint8_t* data, size_t size,
                 size_t& dataOffset, size_t& dataSize, uint32_t& sampleRate);

} // namespace wav
