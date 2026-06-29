/*
 * Wake word detection service for VPS-connected StackChan.
 * Runs a background task that reads mic audio, feeds to ESP-SR,
 * and emits signals on wake word detection + sound level.
 */
#include "hal.h"
#include <mooncake_log.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <vector>
#include <atomic>

#if defined(CONFIG_USE_CUSTOM_WAKE_WORD)
#include <audio/wake_words/custom_wake_word.h>
#define WAKE_WORD_CLASS CustomWakeWord
#elif defined(CONFIG_USE_AFE_WAKE_WORD) || defined(CONFIG_USE_ESP_WAKE_WORD)
#include <audio/wake_words/esp_wake_word.h>
#define WAKE_WORD_CLASS EspWakeWord
#endif

static const std::string_view _tag = "HAL-WakeWord";

#ifdef WAKE_WORD_CLASS

static std::atomic<bool> _wake_running{false};

static void _wake_word_task(void* param)
{
    mclog::tagInfo(_tag, "wake word task started");

    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    if (!codec) {
        mclog::tagError(_tag, "audio codec not available");
        vTaskDelete(nullptr);
        return;
    }

    auto wake = std::make_unique<WAKE_WORD_CLASS>();
    if (!wake->Initialize(codec, nullptr)) {
        mclog::tagError(_tag, "wake word init failed");
        vTaskDelete(nullptr);
        return;
    }

    wake->OnWakeWordDetected([](const std::string& word) {
        mclog::tagInfo(_tag, "Wake word detected: {}", word);
        GetHAL().onWakeWordDetected.emit(word);
    });

    codec->EnableInput(true);
    wake->Start();

    std::vector<int16_t> audio_data;
    uint32_t sound_report_tick = 0;

    _wake_running = true;
    while (_wake_running) {
        if (!codec->InputData(audio_data) || audio_data.empty()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Feed to wake word engine
        wake->Feed(audio_data);

        // Re-start detection after triggered (it auto-stops)
        if (!wake->GetLastDetectedWakeWord().empty()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            wake->Start();
        }

        // Sound level reporting (~500ms intervals)
        uint32_t now = GetHAL().millis();
        if (now - sound_report_tick > 500) {
            sound_report_tick = now;
            int16_t peak = 0;
            int64_t sum = 0;
            for (auto s : audio_data) {
                int16_t a = s < 0 ? -s : s;
                if (a > peak) peak = a;
                sum += a;
            }
            int16_t avg = audio_data.size() > 0 ? (int16_t)(sum / audio_data.size()) : 0;
            if (peak > 300) {
                GetHAL().onSoundLevel.emit(peak, avg);
            }
        }
    }

    codec->EnableInput(false);
    mclog::tagInfo(_tag, "wake word task stopped");
    vTaskDelete(nullptr);
}

void Hal::startWakeWordService()
{
    mclog::tagInfo(_tag, "starting wake word service");
    xTaskCreatePinnedToCoreWithCaps(
        _wake_word_task, "wakeword", 1024 * 16, nullptr, 3, nullptr, 1, MALLOC_CAP_SPIRAM);
}

#else

void Hal::startWakeWordService()
{
    mclog::tagWarn(_tag, "wake word not configured, skipping");
}

#endif
