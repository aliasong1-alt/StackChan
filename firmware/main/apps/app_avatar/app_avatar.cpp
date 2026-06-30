/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_avatar.h"
#include "view/ws_call.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <stackchan/modifiers/modifiers.h>
#include <string_view>
#include <cstdint>
#include <memory>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

#include <string>
#include <sstream>
#include <unordered_set>
#include <board.h>
#include <audio/audio_codec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

static uint32_t _last_touch_report  = 0;
static uint32_t _last_status_report = 0;

struct VoicePlaybackArg {
    int16_t* data;
    size_t count;
};

static void _voice_playback_task(void* param)
{
    auto* arg = static_cast<VoicePlaybackArg*>(param);
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    if (codec && arg->count > 0) {
        codec->EnableOutput(true);

        const size_t chunk_size = 512;
        std::vector<int16_t> chunk;
        for (size_t i = 0; i < arg->count; i += chunk_size) {
            size_t n = std::min(chunk_size, arg->count - i);
            chunk.assign(arg->data + i, arg->data + i + n);
            codec->OutputData(chunk);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
        codec->EnableOutput(false);
    }

    heap_caps_free(arg->data);
    delete arg;
    vTaskDelete(nullptr);
}

static bool contains_word(const std::string& text, const std::unordered_set<std::string>& words)
{
    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };

    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        token = to_lower(token);
        if (words.find(token) != words.end()) {
            return true;
        }
    }
    return false;
}

AppAvatar::AppAvatar()
{
    // 配置 App 名
    setAppInfo().name = "AVATAR";
    // 配置 App 图标
    static auto icon  = assets::get_image("icon_sentinel.bin");
    setAppInfo().icon = (void*)&icon;
    // 配置 App 主题颜色
    static uint32_t theme_color = 0xFF6699;
    setAppInfo().userData       = (void*)&theme_color;
}

// App 被安装时会被调用
void AppAvatar::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppAvatar::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Create loading page
    std::unique_ptr<view::LoadingPage> loading_page;
    {
        LvglLockGuard lock;
        loading_page = std::make_unique<view::LoadingPage>(0xFF6699, 0x431525);
    }

    // Start avatar service
    GetHAL().startWebSocketAvatarService([&](std::string_view msg) {
        LvglLockGuard lock;
        loading_page->setMessage(msg);
    });
    // GetHAL().startBleServer();

    LvglLockGuard lock;

    // Destroy loading page
    loading_page.reset();

    // Create default avatar
    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(lv_screen_active());
    avatar->getPanel()->onClick().connect([&]() { _screen_clicked_flag = true; });
    GetStackChan().attachAvatar(std::move(avatar));

    // Load all modifiers for lifelike behavior
    GetStackChan().addModifier(std::make_unique<BlinkModifier>());
    GetStackChan().addModifier(std::make_unique<IdleExpressionModifier>(4000, 10000));
    GetStackChan().addModifier(std::make_unique<IdleMotionModifier>(15000, 30000));
    GetStackChan().addModifier(std::make_unique<HeadPetModifier>());
    GetStackChan().addModifier(std::make_unique<BreathModifier>());
    GetStackChan().addModifier(std::make_unique<ImuEventModifier>());

    /* ---------------------- Sensor reporting to VPS ---------------------- */
    // Head touch gestures
    GetHAL().onHeadPetGesture.connect([&](HeadPetGesture gesture) {
        const char* name = "unknown";
        switch (gesture) {
            case HeadPetGesture::Press: name = "press"; break;
            case HeadPetGesture::Release: name = "release"; break;
            case HeadPetGesture::SwipeForward: name = "swipe_forward"; break;
            case HeadPetGesture::SwipeBackward: name = "swipe_backward"; break;
            default: return;
        }
        GetHAL().sendWsText(fmt::format("{{\"type\":\"touch\",\"source\":\"head\",\"gesture\":\"{}\"}}", name));
    });

    // Wake word detection
    GetHAL().startWakeWordService();
    GetHAL().onWakeWordDetected.connect([&](const std::string& word) {
        GetHAL().sendWsText(fmt::format("{{\"type\":\"wake\",\"word\":\"{}\"}}", word));
        GetHAL().showRgbColor(0, 30, 30);
        {
            LvglLockGuard lock;
            GetStackChan().addModifier(std::make_unique<SpeakingModifier>(1500));
        }
        // LED off after 1.5 seconds (handled by timed modifier destroying itself)
    });

    // Sound level from wake word service → forward to VPS
    GetHAL().onSoundLevel.connect([&](int peak, int avg) {
        GetHAL().sendWsText(fmt::format("{{\"type\":\"sound\",\"peak\":{},\"avg\":{}}}", peak, avg));
    });

    // Mic monitoring toggle from VPS
    GetHAL().onMicMonitorToggle.connect([&](bool enabled) {
        _mic_monitoring = enabled;
    });

    // Voice audio playback from VPS
    GetHAL().onVoiceAudioReceived.connect([&](std::vector<int16_t> audio) {
        {
            LvglLockGuard lock;
            GetStackChan().addModifier(std::make_unique<SpeakingModifier>(audio.size() * 1000 / 24000));
        }
        GetHAL().showRgbColor(0, 30, 0); // green = speaking

        auto* arg = new VoicePlaybackArg();
        arg->count = audio.size();
        arg->data = static_cast<int16_t*>(
            heap_caps_malloc(audio.size() * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (arg->data) {
            memcpy(arg->data, audio.data(), audio.size() * sizeof(int16_t));
            xTaskCreatePinnedToCoreWithCaps(
                _voice_playback_task, "vplay", 4096, arg, 2, nullptr, 1, MALLOC_CAP_SPIRAM);
        } else {
            delete arg;
        }
    });

    // IMU events (shake, pick up)
    GetHAL().onImuMotionEvent.connect([&](ImuMotionEvent event) {
        const char* name = "unknown";
        switch (event) {
            case ImuMotionEvent::Shake: name = "shake"; break;
            case ImuMotionEvent::PickUp: name = "pickup"; break;
            default: return;
        }
        GetHAL().sendWsText(fmt::format("{{\"type\":\"imu\",\"event\":\"{}\"}}", name));
    });

    /* ------------------------------- BLE events ------------------------------- */
    GetHAL().onBleAvatarData.connect([&](const char* data) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_avatar_data.update_flag) {
            return;
        }
        _ble_avatar_data.update_flag = true;
        _ble_avatar_data.data_ptr    = (char*)data;
    });

    GetHAL().onBleMotionData.connect([&](const char* data) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_motion_data.update_flag) {
            return;
        }
        _ble_motion_data.update_flag = true;
        _ble_motion_data.data_ptr    = (char*)data;
    });

    /* ---------------------------- Websocket events ---------------------------- */
    // Avatar control
    GetHAL().onWsAvatarData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        GetStackChan().updateAvatarFromJson(data.data());
    });

    // Motion control
    GetHAL().onWsMotionData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(data.data());
    });

    // Phone call handling
    GetHAL().onWsCallRequest.connect([&](std::string caller) {
        if (_ws_call_view_id >= 0) {
            mclog::tagWarn(getAppInfo().name, "ws call view already exists");
            return;
        }

        LvglLockGuard lvgl_lock;

        auto& avatar = GetStackChan().avatar();
        avatar.setSpeech("");
        avatar.leftEye().setVisible(false);
        avatar.rightEye().setVisible(false);
        avatar.mouth().setVisible(false);

        auto view      = std::make_unique<view::WsCallView>(lv_screen_active(), caller);
        view->onAccept = []() {
            auto& avatar = GetStackChan().avatar();
            avatar.setSpeech("");
            avatar.leftEye().setVisible(true);
            avatar.rightEye().setVisible(true);
            avatar.mouth().setVisible(true);

            GetHAL().onWsCallResponse.emit(true);
        };
        view->onDecline = []() {
            auto& avatar = GetStackChan().avatar();
            avatar.setSpeech("");
            avatar.leftEye().setVisible(true);
            avatar.rightEye().setVisible(true);
            avatar.mouth().setVisible(true);

            GetHAL().onWsCallResponse.emit(false);
        };
        view->onEnd     = []() { GetHAL().onWsCallEnd.emit(WsSignalSource::Local); };
        view->onDestory = [&]() { _ws_call_view_id = -1; };

        _ws_call_view_id = avatar.addDecorator(std::move(view));
    });

    GetHAL().onWsCallEnd.connect([&](WsSignalSource source) {
        if (source != WsSignalSource::Remote) {
            return;
        }

        LvglLockGuard lvgl_lock;

        if (_ws_call_view_id < 0) {
            mclog::tagWarn(getAppInfo().name, "ws call view does not exist");
            return;
        }

        auto& avatar = GetStackChan().avatar();
        avatar.setSpeech("");
        avatar.leftEye().setVisible(true);
        avatar.rightEye().setVisible(true);
        avatar.mouth().setVisible(true);

        avatar.removeDecorator(_ws_call_view_id);
        _ws_call_view_id = -1;
    });

    // Text message handling
    GetHAL().onWsTextMessage.connect([&](const WsTextMessage_t& message) {
        LvglLockGuard lvgl_lock;

        auto& stackchan = GetStackChan();

        stackchan.addModifier(
            std::make_unique<TimedSpeechModifier>(fmt::format("{} says: {}", message.name, message.content), 6000));
        stackchan.addModifier(std::make_unique<SpeakingModifier>(2000));

        // Special handling
        if (contains_word(message.content, {"hello", "hi"})) {
            stackchan.addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, 2000));
        } else if (contains_word(message.content, {"love"})) {
            stackchan.addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, 2000));
        }
    });

    GetHAL().onWsDanceData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        auto sequence = stackchan::animation::parse_sequence_from_json(data.data());
        if (!sequence.empty()) {
            if (_dance_modifier_id >= 0) {
                GetStackChan().removeModifier(_dance_modifier_id);
            }
            _dance_modifier_id = GetStackChan().addModifier(std::make_unique<DanceModifier>(sequence));
        }
    });

    GetHAL().onWsLog.connect([&](CommonLogLevel level, std::string_view msg) {
        auto type         = static_cast<view::ToastType>(level);
        uint32_t duration = type == view::ToastType::Error ? 12000 : 1600;
        view::pop_a_toast(msg, type, duration);
    });

    /* ------------------------------ Video window ------------------------------ */
    _video_window = std::make_unique<view::VideoWindow>(lv_screen_active());

    /* ----------------------------- Common widgets ----------------------------- */
    view::create_home_indicator([&]() { close(); }, 0xFF9ABC, 0x431525);
    view::create_status_bar(0xFF9ABC, 0x431525);
}

void AppAvatar::onRunning()
{
    std::lock_guard<std::mutex> lock(_mutex);

    LvglLockGuard lvgl_lock;

    if (_ble_avatar_data.update_flag) {
        GetStackChan().updateAvatarFromJson(_ble_avatar_data.data_ptr);
        _ble_avatar_data.update_flag = false;
        _ble_avatar_data.data_ptr    = nullptr;
    }

    if (_ble_motion_data.update_flag) {
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(_ble_motion_data.data_ptr);
        _ble_motion_data.update_flag = false;
        _ble_motion_data.data_ptr    = nullptr;
    }

    if (_screen_clicked_flag) {
        _screen_clicked_flag = false;
        GetHAL().sendWsText("{\"type\":\"touch\",\"source\":\"screen\"}");
        if (_dance_modifier_id >= 0) {
            GetStackChan().removeModifier(_dance_modifier_id);
            _dance_modifier_id = -1;
        }
    }

    // Periodic status report (every 30 seconds)
    {
        uint32_t now = GetHAL().millis();
        if (now - _last_status_report > 30000) {
            _last_status_report = now;
            auto msg = fmt::format(
                "{{\"type\":\"status\",\"battery\":{},\"charging\":{},\"uptime\":{}}}",
                GetHAL().getBatteryLevel(),
                GetHAL().isBatteryCharging() ? "true" : "false",
                now / 1000);
            GetHAL().sendWsText(msg);
        }
    }

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppAvatar::update_mic_monitor()
{
    if (!_mic_monitoring) return;

    uint32_t now = GetHAL().millis();
    if (now - _last_sound_report < 500) return;
    _last_sound_report = now;

    std::vector<int16_t> waveform;
    GetHAL().getMicWaveformFrame(waveform);
    if (waveform.empty()) return;

    int16_t peak = 0;
    int64_t sum = 0;
    for (auto s : waveform) {
        int16_t abs_s = s < 0 ? -s : s;
        if (abs_s > peak) peak = abs_s;
        sum += (int64_t)abs_s;
    }
    int16_t avg = (int16_t)(sum / waveform.size());

    if (peak > 500) {
        GetHAL().sendWsText(fmt::format("{{\"type\":\"sound\",\"peak\":{},\"avg\":{}}}", peak, avg));
    }
}

void AppAvatar::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    {
        LvglLockGuard lock;

        GetStackChan().resetAvatar();
        _video_window.reset();

        GetHAL().onBleAvatarData.clear();
        GetHAL().onBleMotionData.clear();
        GetHAL().onHeadPetGesture.clear();
        GetHAL().onImuMotionEvent.clear();
        GetHAL().onMicMonitorToggle.clear();
        GetHAL().onWakeWordDetected.clear();
        GetHAL().onSoundLevel.clear();
        GetHAL().onVoiceAudioReceived.clear();

        GetHAL().onWsAvatarData.clear();
        GetHAL().onWsMotionData.clear();
        GetHAL().onWsCallRequest.clear();
        GetHAL().onWsCallEnd.clear();
        GetHAL().onWsTextMessage.clear();
        GetHAL().onWsDanceData.clear();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(1);
}

void AppAvatar::check_auto_angle_sync_mode()
{
    auto& motion = GetStackChan().motion();

    // If far from last command, enable auto angle sync
    if (GetHAL().millis() - _last_motion_cmd_tick > 2000) {
        motion.setAutoAngleSyncEnabled(true);
    } else {
        motion.setAutoAngleSyncEnabled(false);
    }

    _last_motion_cmd_tick = GetHAL().millis();
}
