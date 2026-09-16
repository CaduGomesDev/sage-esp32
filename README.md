# Sage — Assistente de Voz Nativo para ESP32-S3

Assistente de voz autônomo rodando inteiramente no ESP32-S3 DevKitC-1 (N16R8).
Captura a voz pelo microfone I2S, transcreve, consulta um LLM e responde em
áudio pelo amplificador I2S — com feedback visual num OLED.

```
    voz  ──►  INMP441 ──► [STT: Groq Whisper] ──► [LLM: Gemini] ──► [TTS: Gemini]
                                                                          │
    áudio ◄── MAX98357A ◄──────────────────────────────────────────────────┘
```

---

## 1. Onde colocar as chaves de API

As chaves ficam **exclusivamente** em `include/secrets.h`, que já está no
`.gitignore` e nunca deve ser commitado. O arquivo já foi criado a partir do
modelo — basta abrir e preencher:

São só **duas** chaves na configuração padrão — a do Gemini serve para o LLM e
para a voz.

| Constante             | Obrigatória? | Onde obter                                            |
| --------------------- | ------------ | ----------------------------------------------------- |
| `WIFI_SSID` / `WIFI_PASSWORD` | sim  | Sua rede **2,4 GHz** (o ESP32-S3 não suporta 5 GHz)   |
| `GROQ_API_KEY`        | sim          | <https://console.groq.com/keys> — `gsk_...`            |
| `GEMINI_API_KEY`      | sim          | <https://aistudio.google.com/app/apikey> — `AIza...` ou `AQ....` |
| `ELEVENLABS_API_KEY`  | não          | Só com `TTS_PROVIDER = ELEVENLABS` (exige plano pago)  |
| `ELEVENLABS_VOICE_ID` | não          | idem — padrão já preenchido com a voz Rachel           |
| `GOOGLE_TTS_API_KEY`  | não          | Só com `TTS_PROVIDER = GOOGLE` (exige billing no GCP)  |

Se precisar recriar o arquivo:

```powershell
Copy-Item include\secrets.h.example include\secrets.h
```

Tudo o que **não** é segredo (modelos, endpoints, pinos, limiares) fica em
`include/config.h`.

---

## 2. Ligações

### Microfone INMP441 (I2S0 — entrada)

| INMP441 | ESP32-S3 |
| ------- | -------- |
| VDD     | 3V3      |
| GND     | GND      |
| L/R     | **GND** (canal esquerdo — o firmware lê `ONLY_LEFT`) |
| SCK     | GPIO 42  |
| WS      | GPIO 41  |
| SD      | GPIO 2   |

### Amplificador MAX98357A (I2S1 — saída)

| MAX98357A | ESP32-S3 |
| --------- | -------- |
| VIN       | 5V (USB) — 3V3 funciona, mas com menos volume |
| GND       | GND      |
| BCLK      | GPIO 15  |
| LRC       | GPIO 16  |
| DIN       | GPIO 7   |
| SD        | Deixe no pull-up da placa. Se o som não sair, ligue em 3V3 |
| GAIN      | Livre = 9 dB. Para GND = 12 dB |

### OLED 0.96" I2C

| OLED | ESP32-S3 |
| ---- | -------- |
| VCC  | 3V3      |
| GND  | GND      |
| SDA  | GPIO 8   |
| SCL  | GPIO 9   |

Endereço padrão `0x3C`. Se o seu módulo usar `0x3D`, ajuste `OLED_I2C_ADDRESS`.
Para painéis **SH1106**, mude `OLED_DRIVER` para `OLED_DRIVER_SH1106`.

O botão **BOOT** (GPIO 0) funciona como push-to-talk: segure para gravar.

---

## 3. Compilar e gravar

```powershell
pio run              # compila
pio run -t upload    # grava
pio device monitor   # serial em 115200
```

> Se o monitor serial não mostrar nada, você provavelmente está no conector
> **USB** (nativo) em vez do **UART**. Troque de porta ou mude
> `-DARDUINO_USB_CDC_ON_BOOT=0` para `=1` no `platformio.ini`.

Configuração de memória do N16R8 (já no `platformio.ini`) — sem
`board_build.arduino.memory_type = qio_opi` a PSRAM OPI **não é detectada** e
`ps_malloc()` falha:

```ini
board_build.arduino.memory_type = qio_opi
board_build.flash_mode          = qio
board_build.partitions          = default_16MB.csv
build_flags                     = -DBOARD_HAS_PSRAM
```

---

## 4. Arquitetura

```
include/
  config.h           Pinos, parâmetros de áudio, endpoints, prompts, timings
  secrets.h          Chaves de API (gitignored)
  secrets.h.example  Modelo versionado
  AppState.h         enum da máquina de estados
  AudioBuffer.h      Buffer em PSRAM + leitura/escrita de cabeçalho WAV
  NetworkManager.h   Wi-Fi com reconexão automática + NTP
  DisplayManager.h   Rosto animado no OLED (task dedicada)
  AudioRecorder.h    Captura I2S + VAD + pré-roll
  AudioPlayer.h      Reprodução I2S + volume + bipes
  HttpUtils.h        TLS, erros e o Stream multipart
  STTClient.h        Groq / whisper-large-v3
  LLMClient.h        Google Gemini + histórico + limpeza para fala
  TTSClient.h        ElevenLabs ou Google Cloud TTS
src/
  main.cpp           Máquina de estados, setup, loop, console serial
  *.cpp              Implementações dos módulos acima
```

### Máquina de estados

```
  BOOT ─► WIFI ─► IDLE ──(voz ou botão)──► LISTENING
                   ▲                           │
                   │                           ▼
               SPEAKING ◄── PROCESSING ◄───────┘
                   ▲             │
                   └─── ERROR ◄──┘   (volta para IDLE após 3,5 s)
```

### Decisões que valem conhecer

**Tudo que é grande mora na PSRAM.** `AudioBuffer` usa `ps_malloc()` e só cai
para a RAM interna como último recurso. São ~384 KB para a gravação (12 s) e
~1,3 MB para a resposta (40 s). A RAM interna fica livre para o mbedTLS, que é
o recurso realmente escasso durante as chamadas HTTPS.

**O WAV nunca é duplicado na memória.** `MultipartStream` (em `HttpUtils.h`) é
um `Stream` que costura "prólogo + binário da PSRAM + epílogo" sob demanda. O
`HTTPClient` o consome em blocos de 1460 bytes, então enviar 400 KB de áudio
não exige 400 KB extras.

**O display roda numa task própria** (core 0, 25 fps). As chamadas de STT, LLM
e TTS bloqueiam o `loop()` por vários segundos; sem uma task separada a
animação de "pensando" simplesmente congelaria. Só a task de render toca no
barramento I2C; o resto do firmware usa setters protegidos por mutex.

**O quadro só vai para o painel quando muda.** Enviar 1 KB de framebuffer a
400 kHz custa ~23 ms de I2C. O `DisplayManager` guarda um shadow buffer e
compara com `memcmp` antes de cada flush, então um rosto parado entre piscadas
não gera tráfego nenhum no barramento.

---

## 4.1 As expressões

O OLED não mostra texto de status: o estado vira um rosto.

| Estado | Rosto |
| ------ | ----- |
| `STATE_BOOT` | Olhos sonolentos (duas linhas horizontais) |
| `STATE_IDLE` | Olhos arredondados, piscada aleatória a cada 3–5 s (às vezes dupla) e pequenas sacadas do olhar |
| `STATE_LISTENING` | Olhos arregalados + onda sonora no rodapé, com amplitude no nível do microfone |
| `STATE_PROCESSING` | Olhos viram barras que sobem e descem em contrafase; três pontos indicam a etapa (STT → LLM → TTS) |
| `STATE_SPEAKING` | Olhos felizes `^ ^` + boca que abre conforme a amplitude do áudio |
| `STATE_ERROR` ou sem Wi-Fi | Olhos `X X`, boca virada, triângulo de alerta e ícone de Wi-Fi cortado nos cantos |

A geometria dos olhos (largura, altura, raio, espaçamento, linha) é
interpolada quadro a quadro, então a troca de estado é uma transição suave em
vez de um corte seco.

`setMessage()` e `setSubtitle()` continuam na API, mas por padrão não são
desenhados — o rosto é a interface. Para voltar a ver a transcrição e a
resposta como legenda de uma linha no rodapé, compile com
`-DFACE_SHOW_CAPTION=1`. Mensagens de erro aparecem no rodapé mesmo com a
legenda desligada: um `X X` sem dizer o que falhou não ajuda a depurar.

**O INMP441 é um microfone de 24 bits.** O driver lê slots de 32 bits e o
firmware desloca 14 bits (`MIC_SHIFT_BITS`) antes de saturar em `int16`.

**Pré-roll de 320 ms.** Um ring buffer guarda o áudio anterior ao disparo do
VAD, senão a primeira sílaba da frase — justamente a que acionou o gatilho —
seria perdida.

**O TTS usa o Gemini, com a mesma chave do LLM.** A escolha foi forçada por um
teste: o plano gratuito do ElevenLabs recusa as vozes da biblioteca via API
(HTTP 402 `paid_plan_required`), e o Google Cloud TTS exige billing habilitado.
O `gemini-3.1-flash-tts-preview` devolve PCM L16 a 24 kHz, em português, sem
custo e sem conta nova.

O contra é a latência: medidos ~3,3 s para uma frase curta, contra menos de 1 s
do ElevenLabs. Os três provedores continuam implementados — se você assinar o
plano Starter, basta trocar `TTS_PROVIDER` em `config.h` para
`TTS_PROVIDER_ELEVENLABS` e ajustar `TTS_SAMPLE_RATE` para `16000`.

**O base64 é decodificado em streaming.** Gemini e Google TTS devolvem o áudio
inteiro codificado dentro de um JSON — 262 KB de JSON para 196 KB de áudio.
`streamBase64Value()` varre o socket procurando a chave e decodifica conforme
os bytes chegam, então a string base64 nunca existe na memória.

---

## 5. Console serial (bring-up)

Com o monitor aberto em 115200, digite `help`:

| Comando        | Efeito                                              |
| -------------- | --------------------------------------------------- |
| `ask <texto>`  | Pula o STT e manda o texto direto para o LLM         |
| `say <texto>`  | Testa só o TTS + alto-falante                        |
| `listen`       | Força uma gravação                                   |
| `cal`          | Recalibra o piso de ruído                            |
| `reset`        | Limpa o histórico da conversa                        |
| `status`       | Memória, Wi-Fi, estado, piso de ruído                |
| `vol <0-100>`  | Ajusta o volume                                      |

---

## 6. Ajuste fino

Tudo em `include/config.h`:

| Sintoma                                 | O que mexer                                    |
| --------------------------------------- | ---------------------------------------------- |
| Dispara sozinho com ruído ambiente      | Aumente `VAD_TRIGGER_FACTOR` ou `VAD_MIN_RMS`   |
| Não dispara quando você fala            | Diminua `VAD_TRIGGER_FACTOR`; aumente `MIC_DIGITAL_GAIN` |
| Corta o fim da frase                    | Aumente `VAD_SILENCE_MS`                       |
| Corta o começo da frase                 | Aumente `VAD_PREROLL_MS`                       |
| Áudio distorcido/saturado               | Diminua `MIC_DIGITAL_GAIN` ou `SPK_DEFAULT_VOLUME` |
| Respostas longas demais                 | Diminua `LLM_MAX_OUTPUT_TOKENS`                |
| Resposta de voz truncada                | Aumente `TTS_MAX_SECONDS`                      |

---

## 7. Limitações conhecidas

- **TLS sem validação de certificado.** `httpx::configureTls()` usa
  `setInsecure()` para não ter que embarcar e manter três cadeias de CA. Para
  produção, troque por `client.setCACert(...)` — o relógio já é sincronizado
  por NTP no boot, que é o pré-requisito para a validação funcionar.
- **TTS em buffer, não em streaming.** A resposta é baixada por inteiro antes
  de tocar, o que adiciona a latência do download ao tempo de resposta. Um
  ring buffer consumido por uma task de playback resolveria; ficou de fora
  para manter o fluxo de controle simples. Com o Gemini TTS isso pesa mais:
  são ~3,3 s só para a voz, somados ao STT e ao LLM.
- **Os modelos de TTS do Gemini estão em preview** e podem ser renomeados ou
  aposentados. Se o firmware passar a responder `TTS: modelo indisponivel`
  (HTTP 404), liste os modelos disponíveis com
  `GET https://generativelanguage.googleapis.com/v1beta/models?key=SUA_CHAVE`
  e atualize `GEMINI_TTS_MODEL` em `config.h`.
- **Sem wake word.** O gatilho é energia de voz (VAD), então qualquer som alto
  o suficiente inicia uma gravação. Para wake word real, o caminho é
  ESP-SR/WakeNet, que exige o framework ESP-IDF.
- **`gemini-1.5-flash`** é o modelo configurado, conforme especificado. Os
  modelos mais recentes da família Flash são mais rápidos e baratos — trocar é
  só mudar `GEMINI_MODEL` em `config.h`.
