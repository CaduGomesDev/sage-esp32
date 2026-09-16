// =============================================================================
//  config.h - Configuracao central do projeto (pinos, audio, APIs, timings)
//  Sage - Assistente de Voz | ESP32-S3 DevKitC-1 N16R8
// =============================================================================
#pragma once

#include <Arduino.h>
#include <driver/i2s.h>

#include "secrets.h"   // <<<< SUAS CHAVES DE API FICAM AQUI

// -----------------------------------------------------------------------------
//  IDENTIDADE
// -----------------------------------------------------------------------------
#define DEVICE_NAME              "Sage"
#define FIRMWARE_VERSION         "1.0.0"

// =============================================================================
//  1. PINAGEM
// =============================================================================

// --- Microfone I2S INMP441 (entrada / RX) ------------------------------------
// O pino L/R do INMP441 deve ir para GND -> dados no canal ESQUERDO.
#define MIC_I2S_PORT             I2S_NUM_0
#define PIN_MIC_SCK              42        // BCLK
#define PIN_MIC_WS               41        // LRCL / WS
#define PIN_MIC_SD               2         // DOUT do microfone

// --- Amplificador I2S MAX98357A (saida / TX) ---------------------------------
// O pino SD do MAX98357A precisa estar em nivel ALTO (ou com o pull-up da
// placa) para o amplificador sair do modo shutdown.
#define SPK_I2S_PORT             I2S_NUM_1
#define PIN_SPK_BCLK             15        // BCLK
#define PIN_SPK_LRC              16        // LRC / WS
#define PIN_SPK_DIN              7         // DIN

// --- Display OLED 0.96 pol I2C -----------------------------------------------
#define PIN_OLED_SDA             8
#define PIN_OLED_SCL             9
#define OLED_I2C_ADDRESS         0x3C      // alguns modulos usam 0x3D
#define OLED_WIDTH               128
#define OLED_HEIGHT              64
#define OLED_I2C_FREQ            400000UL

// Driver do display: 0 = SSD1306 (padrao), 1 = SH1106
#define OLED_DRIVER_SSD1306      0
#define OLED_DRIVER_SH1106       1
#define OLED_DRIVER              OLED_DRIVER_SSD1306

// --- Botao push-to-talk (BOOT da DevKitC-1, ativo em LOW) --------------------
#define PIN_BUTTON               0
#define BUTTON_ACTIVE_LEVEL      LOW

// =============================================================================
//  2. AUDIO
// =============================================================================

// --- Captura -----------------------------------------------------------------
#define SAMPLE_RATE_HZ           16000     // Whisper trabalha nativamente a 16k
#define REC_MAX_SECONDS          12
#define REC_MAX_SAMPLES          ((size_t)SAMPLE_RATE_HZ * REC_MAX_SECONDS)
#define WAV_HEADER_SIZE          44
#define REC_BUFFER_BYTES         (REC_MAX_SAMPLES * 2 + WAV_HEADER_SIZE)

#define MIC_DMA_BUF_COUNT        8
#define MIC_DMA_BUF_LEN          256       // frames por buffer DMA
#define MIC_BLOCK_SAMPLES        512       // amostras processadas por iteracao

// O INMP441 entrega 24 bits alinhados a esquerda dentro de um slot de 32 bits.
// Deslocamos para caber em int16 e aplicamos um ganho digital leve.
#define MIC_SHIFT_BITS           14
#define MIC_DIGITAL_GAIN         3.0f

// --- Deteccao de voz (VAD por energia RMS) -----------------------------------
#define VAD_CALIBRATION_MS       1200      // mede o ruido ambiente no boot
#define VAD_TRIGGER_FACTOR       4.0f      // x piso de ruido para iniciar
#define VAD_RELEASE_FACTOR       2.0f      // x piso de ruido para considerar silencio
#define VAD_MIN_RMS              300.0f    // limiar absoluto minimo
#define VAD_SILENCE_MS           900       // silencio que encerra a gravacao
#define VAD_MIN_SPEECH_MS        400       // fala minima para valer um pedido
#define VAD_PREROLL_MS           320       // audio guardado ANTES do gatilho

// --- Reproducao --------------------------------------------------------------
// Precisa bater com o formato devolvido pelo TTS_PROVIDER escolhido abaixo.
// Gemini TTS entrega 24 kHz; ElevenLabs pcm_16000 e Google Cloud LINEAR16, 16 kHz.
#define TTS_SAMPLE_RATE          24000
#define TTS_MAX_SECONDS          40
#define TTS_BUFFER_BYTES         ((size_t)TTS_SAMPLE_RATE * 2 * TTS_MAX_SECONDS)

#define SPK_DMA_BUF_COUNT        8
#define SPK_DMA_BUF_LEN          256
#define SPK_DEFAULT_VOLUME       0.75f     // 0.0 .. 1.0

// =============================================================================
//  3. WI-FI
// =============================================================================
#define WIFI_CONNECT_TIMEOUT_MS  20000
#define WIFI_RETRY_INTERVAL_MS   5000
#define WIFI_HOSTNAME            "sage-esp32"
#define NTP_SERVER_1             "pool.ntp.org"
#define NTP_SERVER_2             "time.google.com"
#define TZ_INFO                  "<-03>3"  // America/Sao_Paulo

// =============================================================================
//  4. APIs EXTERNAS
// =============================================================================

// --- STT: Groq / Whisper large v3 -------------------------------------------
#define GROQ_STT_URL             "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_STT_MODEL           "whisper-large-v3"
#define GROQ_STT_LANGUAGE        "pt"
#define STT_TIMEOUT_MS           25000

// --- LLM: Google Gemini ------------------------------------------------------
#define GEMINI_MODEL             "gemini-1.5-flash"
#define GEMINI_API_BASE          "https://generativelanguage.googleapis.com/v1beta/models/"
#define LLM_TIMEOUT_MS           25000
#define LLM_MAX_OUTPUT_TOKENS    160
#define LLM_TEMPERATURE          0.7f
#define LLM_HISTORY_TURNS        6        // pares pergunta/resposta mantidos

// System prompt: curto, em PT-BR, otimizado para saida falada.
#define LLM_SYSTEM_PROMPT \
  "Voce e a Sage, uma assistente de voz que roda dentro de um pequeno " \
  "dispositivo ESP32. Responda SEMPRE em portugues do Brasil, de forma " \
  "natural e conversada, em no maximo 2 frases curtas (ate 45 palavras). " \
  "Sua resposta sera lida em voz alta: nunca use markdown, listas, emojis, " \
  "asteriscos ou qualquer formatacao. Escreva numeros por extenso quando " \
  "forem curtos. Se nao souber algo, admita em uma frase."

// --- TTS ---------------------------------------------------------------------
//  GEMINI     : usa a MESMA chave do LLM, funciona no plano gratuito. Padrao.
//  ELEVENLABS : melhor voz e latencia < 1 s, mas o plano gratuito NAO libera as
//               vozes da biblioteca via API (HTTP 402). Exige plano pago.
//  GOOGLE     : Google Cloud Text-to-Speech, exige billing habilitado no GCP.
#define TTS_PROVIDER_ELEVENLABS  0
#define TTS_PROVIDER_GOOGLE      1
#define TTS_PROVIDER_GEMINI      2
#define TTS_PROVIDER             TTS_PROVIDER_GEMINI

#define TTS_TIMEOUT_MS           30000

// Gemini TTS (modelos em preview; confira a lista com GET /v1beta/models).
// Medido nesta conta: 3.1-flash ~3,3 s | 2.5-flash ~6,1 s para uma frase curta.
#define GEMINI_TTS_MODEL         "gemini-3.1-flash-tts-preview"
#define GEMINI_TTS_VOICE         "Kore"   // Puck, Charon, Aoede, Leda, Zephyr...

// Prefixo de estilo: o modelo trata como direcao de atuacao e NAO le em voz
// alta (verificado comparando a duracao do audio com e sem o prefixo).
#define GEMINI_TTS_STYLE \
  "Leia em portugues do Brasil, em tom natural e amigavel: "

// ElevenLabs: pcm_16000 devolve PCM 16-bit mono cru (sem precisar decodificar MP3).
#define ELEVENLABS_URL_BASE      "https://api.elevenlabs.io/v1/text-to-speech/"
#define ELEVENLABS_MODEL         "eleven_flash_v2_5"   // baixa latencia, multilingue
#define ELEVENLABS_OUTPUT_FORMAT "pcm_16000"
#define ELEVENLABS_STABILITY     0.5f
#define ELEVENLABS_SIMILARITY    0.75f

// Google Cloud TTS: LINEAR16 devolve um WAV em base64 dentro do JSON.
#define GOOGLE_TTS_URL           "https://texttospeech.googleapis.com/v1/text:synthesize"
#define GOOGLE_TTS_LANGUAGE      "pt-BR"
#define GOOGLE_TTS_VOICE         "pt-BR-Standard-B"

// =============================================================================
//  5. COMPORTAMENTO / UI
// =============================================================================
#define DISPLAY_FPS              25
#define DISPLAY_TASK_STACK       4096
#define DISPLAY_TASK_CORE        0
#define ERROR_DISPLAY_MS         3500

// Som de confirmacao ao comecar a ouvir
#define ENABLE_START_BEEP        1
#define BEEP_FREQ_HZ             880
#define BEEP_DURATION_MS         90

// =============================================================================
//  6. VALIDACOES DE COMPILACAO
// =============================================================================
#if !defined(BOARD_HAS_PSRAM)
#  error "BOARD_HAS_PSRAM nao definido. Verifique build_flags no platformio.ini."
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#  warning "Projeto escrito para o Arduino core 2.x (driver/i2s.h legado)."
#endif
