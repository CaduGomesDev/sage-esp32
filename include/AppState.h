// =============================================================================
//  AppState.h - Estados da maquina de estados principal
// =============================================================================
#pragma once

#include <Arduino.h>

enum AppState : uint8_t {
    STATE_BOOT = 0,     // inicializando perifericos
    STATE_WIFI,         // conectando / reconectando ao Wi-Fi
    STATE_IDLE,         // aguardando gatilho de voz ou botao
    STATE_LISTENING,    // gravando audio do usuario
    STATE_PROCESSING,   // STT -> LLM -> TTS
    STATE_SPEAKING,     // reproduzindo a resposta
    STATE_ERROR         // falha temporaria, volta para IDLE
};

// Sub-etapa exibida durante STATE_PROCESSING.
enum ProcessingStep : uint8_t {
    STEP_NONE = 0,
    STEP_STT,
    STEP_LLM,
    STEP_TTS
};

inline const char* appStateName(AppState s) {
    switch (s) {
        case STATE_BOOT:       return "BOOT";
        case STATE_WIFI:       return "WIFI";
        case STATE_IDLE:       return "IDLE";
        case STATE_LISTENING:  return "LISTENING";
        case STATE_PROCESSING: return "PROCESSING";
        case STATE_SPEAKING:   return "SPEAKING";
        case STATE_ERROR:      return "ERROR";
        default:               return "?";
    }
}

inline const char* processingStepName(ProcessingStep s) {
    switch (s) {
        case STEP_STT: return "Transcrevendo";
        case STEP_LLM: return "Pensando";
        case STEP_TTS: return "Gerando voz";
        default:       return "Processando";
    }
}
