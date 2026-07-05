/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include "board/hal_bridge.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <board.h>
#include <web_socket.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <jpg/image_to_jpeg.h>
#include <wifi_station.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <mutex>
#include <queue>
#include <vector>
#include <esp_heap_caps.h>
#include <display.h>
#include <lvgl_image.h>
#include <wifi_manager.h>
#include "utils/jpeg_to_image/jpeg_decoder.h"
#include "utils/secret_logic/secret_logic.h"

static std::string _tag = "WS-Avatar";

static const std::string _setting_ns              = "stackchan";
static const std::string _setting_device_name_key = "device_name";

class WebSocketAvatar {
public:
    enum class DataType : uint8_t {
        Opus              = 0x01,
        Jpeg              = 0x02,
        ControlAvatar     = 0x03,
        ControlMotion     = 0x04,
        StartCameraStream = 0x05,
        StopCameraStream  = 0x06,
        TextMessage       = 0x07,
        RequestCall       = 0x09,
        DeclineCall       = 0x0A,
        AcceptCall        = 0x0B,
        EndCall           = 0x0C,
        SetDeviceName     = 0x0D,
        GetDeviceName     = 0x0E,
        HeartbeatPing     = 0x10,
        HeartbeatPong     = 0x11,
        VideoModeOn       = 0x12,
        VideoModeOff      = 0x13,
        DanceSequence     = 0x14,
        StartAudioStream  = 0x18,
        StopAudioStream   = 0x19,
        VoiceUpload       = 0x20,
        VoicePlayback     = 0x21,
    };

    struct ReceivedMessage {
        bool binary;
        std::vector<uint8_t> data;
    };

    void init()
    {
        _url = secret_logic::get_server_url();

        connect();

        GetHAL().onWsSendText.connect([this](const std::string& text) {
            if (isConnected()) {
                _websocket->Send(text.c_str());
            }
        });

        GetHAL().onVoiceRecordingComplete.connect([this](const std::vector<int16_t>& audio) {
            if (isConnected()) {
                sendPacket(DataType::VoiceUpload,
                           reinterpret_cast<const uint8_t*>(audio.data()),
                           audio.size() * sizeof(int16_t));
                ESP_LOGI(_tag.c_str(), "Sent voice upload: %d samples", (int)audio.size());
            }
        });

        GetHAL().onWsCallResponse.connect([this](bool accepted) {
            if (!isConnected()) {
                return;
            }

            if (accepted) {
                ESP_LOGI(_tag.c_str(), "Sending AcceptCall");
                sendPacket(DataType::AcceptCall, nullptr, 0);
            } else {
                ESP_LOGI(_tag.c_str(), "Sending DeclineCall");
                sendPacket(DataType::DeclineCall, nullptr, 0);
            }
        });

        GetHAL().onWsCallEnd.connect([this](WsSignalSource source) {
            if (!isConnected()) {
                return;
            }

            if (source != WsSignalSource::Local) {
                return;
            }

            ESP_LOGI(_tag.c_str(), "Sending EndCall");
            sendPacket(DataType::EndCall, nullptr, 0);
        });
    }

    void connect()
    {
        auto token = secret_logic::generate_auth_token();

        // 销毁旧实例，确保状态复位
        _websocket.reset();

        auto& board  = Board::GetInstance();
        auto network = board.GetNetwork();

        // 创建 WebSocket 实例
        _websocket = network->CreateWebSocket(1);

        if (!_websocket) {
            ESP_LOGE(_tag.c_str(), "Failed to create websocket");
            return;
        }

        // 设置认证头
        _websocket->SetHeader("Authorization", token.c_str());

        // 设置回调
        _websocket->OnConnected([this]() {
            ESP_LOGI(_tag.c_str(), "Connected to server!");
            // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Server connected");
            _last_heartbeat_time = GetHAL().millis();
            _websocket->Send("{\"type\":\"hello\",\"device\":\"stackchan\",\"firmware\":\"dao-official-v1\",\"camera\":true}");
        });

        _websocket->OnDisconnected([this]() {
            ESP_LOGI(_tag.c_str(), "Disconnected!");
            // GetHAL().onWsLog.emit(CommonLogLevel::Error, "Server disconnected");
        });

        _websocket->OnData([this](const char* data, size_t len, bool binary) {
            std::lock_guard<std::mutex> lock(_mutex);
            _msg_queue.push({binary, std::vector<uint8_t>(data, data + len)});
        });

        // ESP_LOGI(_tag.c_str(), "Connecting to %s...", _url.c_str());
        // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Connecting to server...");
        if (!_websocket->Connect(_url.c_str())) {
            ESP_LOGE(_tag.c_str(), "Failed to connect");
            GetHAL().onWsLog.emit(CommonLogLevel::Error, "Connect to server Failed");
        }
        _last_reconnect_attempt = GetHAL().millis();
    }

    void update()
    {
        if (!_websocket) {
            return;
        }

        if (!_websocket->IsConnected()) {
            if (GetHAL().millis() - _last_reconnect_attempt > 5000) {
                connect();
            }
        } else {
            processMessages();

            // Check heartbeat timeout (5 min for self-hosted VPS)
            if (GetHAL().millis() - _last_heartbeat_time > 300000) {
                ESP_LOGE(_tag.c_str(), "Heartbeat timeout!");
                GetHAL().onWsLog.emit(CommonLogLevel::Error, "Heartbeat Timeout");
                _last_heartbeat_time = GetHAL().millis();
                return;
            }
        }

        if (_is_streaming) {
            if (GetHAL().millis() - _last_capture_time >= (_is_video_mode ? 700 : 350)) {
                captureAndSendFrame();
                _last_capture_time = GetHAL().millis();
            }
        }
    }

    void processMessages()
    {
        std::vector<ReceivedMessage> messages;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            while (!_msg_queue.empty()) {
                messages.push_back(std::move(_msg_queue.front()));
                _msg_queue.pop();
            }
        }

        for (const auto& msg : messages) {
            handleMessage(msg);
        }
    }

    void handleMessage(const ReceivedMessage& msg)
    {
        if (msg.binary) {
            if (msg.data.size() < 1) return;
            DataType type = static_cast<DataType>(msg.data[0]);
            ESP_LOGI(_tag.c_str(), "Received binary type: %d, len: %d", (int)type, (int)msg.data.size());

            switch (type) {
                // Opus handled in OnData Fast Path
                // case DataType::Opus: {
                //     if (msg.data.size() > 5) {
                //         auto packet = std::make_unique<AudioStreamPacket>();
                //         packet->payload.assign(msg.data.begin() + 5, msg.data.end());
                //         _audio_service.PushPacketToDecodeQueue(std::move(packet));
                //     }
                //     break;
                // }
                case DataType::StartCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Start Camera Stream");
                    setStreamingEnabled(true);
                    _websocket->Send("camera stream started");
                    break;
                }
                case DataType::StopCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Stop Camera Stream");
                    setStreamingEnabled(false);
                    _websocket->Send("camera stream stopped");
                    break;
                }
                case DataType::ControlAvatar: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Avatar Payload: %s", payload.c_str());
                        GetHAL().onWsAvatarData.emit(payload);
                    }
                    break;
                }
                case DataType::ControlMotion: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Motion Payload: %s", payload.c_str());
                        GetHAL().onWsMotionData.emit(payload);
                    }
                    break;
                }
                case DataType::RequestCall: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "RequestCall Payload: %s", payload.c_str());
                        GetHAL().onWsCallRequest.emit(payload);
                    }
                    break;
                }
                case DataType::EndCall: {
                    ESP_LOGI(_tag.c_str(), "EndCall");
                    GetHAL().onWsCallEnd.emit(WsSignalSource::Remote);
                    break;
                }
                case DataType::SetDeviceName: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "SetDeviceName Payload: %s", payload.c_str());

                        Settings settings(_setting_ns, true);
                        settings.SetString(_setting_device_name_key, payload);
                    }
                    break;
                }
                case DataType::GetDeviceName: {
                    ESP_LOGI(_tag.c_str(), "GetDeviceName");

                    Settings settings(_setting_ns, false);
                    auto device_name = settings.GetString(_setting_device_name_key, "StackChan");

                    sendPacket(DataType::GetDeviceName, (const uint8_t*)device_name.c_str(), device_name.size());
                    break;
                }
                case DataType::HeartbeatPing: {
                    ESP_LOGI(_tag.c_str(), "HeartbeatPing");
                    _last_heartbeat_time = GetHAL().millis();
                    sendPacket(DataType::HeartbeatPong, nullptr, 0);
                    break;
                }
                case DataType::TextMessage: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "TextMessage Payload: %s", payload.c_str());

                        ArduinoJson::JsonDocument doc;
                        auto error = ArduinoJson::deserializeJson(doc, payload);
                        if (error) {
                            ESP_LOGE(_tag.c_str(), "DeserializeJson failed: %s", error.c_str());
                            return;
                        }

                        WsTextMessage_t text_msg;

                        if (doc["name"].is<std::string>()) {
                            text_msg.name = doc["name"].as<std::string>();
                        }
                        if (doc["content"].is<std::string>()) {
                            text_msg.content = doc["content"].as<std::string>();
                        }

                        GetHAL().onWsTextMessage.emit(text_msg);
                    }
                    break;
                }
                case DataType::VideoModeOn: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOn");
                    GetHAL().onWsVideoModeChange.emit(true);
                    _is_video_mode = true;
                    break;
                }
                case DataType::VideoModeOff: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOff");
                    GetHAL().onWsVideoModeChange.emit(false);
                    _is_video_mode = false;
                    break;
                }
                case DataType::Jpeg: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        ESP_LOGI(_tag.c_str(), "Jpeg Frame Received, size: %d", (int)(msg.data.size() - 5));

                        static int64_t _time_count = 0;
                        static int64_t _interval   = 0;
                        _time_count                = esp_timer_get_time();

                        size_t jpeg_len    = msg.data.size() - 5;
                        uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(jpeg_len, MALLOC_CAP_8BIT);
                        if (jpeg_data) {
                            memcpy(jpeg_data, msg.data.data() + 5, jpeg_len);

                            auto image = jpeg_dec::decode_to_lvgl(jpeg_data, jpeg_len);
                            if (image) {
                                // ESP_LOGI(_tag.c_str(), "Done");

                                _interval = esp_timer_get_time() - _time_count;
                                mclog::info("jpeg decode time: {} ms", _interval / 1000);

                                GetHAL().onWsVideoFrame.emit(image);
                            } else {
                                ESP_LOGE(_tag.c_str(), "Failed to decode JPEG");
                            }
                            heap_caps_free(jpeg_data);
                        } else {
                            ESP_LOGE(_tag.c_str(), "Failed to allocate memory for JPEG");
                        }
                    }
                    break;
                }
                case DataType::DanceSequence: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Dance Payload:\n%s", payload.c_str());
                        ESP_LOGI(_tag.c_str(), "DanceSequence size: %d", (int)payload.size());
                        GetHAL().onWsDanceData.emit(payload);
                    }
                    break;
                }
                case DataType::VoicePlayback: {
                    if (msg.data.size() > 5) {
                        size_t pcm_bytes = msg.data.size() - 5;
                        size_t sample_count = pcm_bytes / sizeof(int16_t);
                        std::vector<int16_t> audio(sample_count);
                        memcpy(audio.data(), msg.data.data() + 5, pcm_bytes);
                        ESP_LOGI(_tag.c_str(), "Voice playback received: %d samples", (int)sample_count);
                        GetHAL().onVoiceAudioReceived.emit(std::move(audio));
                    }
                    break;
                }
                case DataType::StartAudioStream: {
                    break;
                }
                case DataType::StopAudioStream: {
                    break;
                }
                default:
                    break;
            }
        } else {
            // JSON text commands from VPS server
            std::string text(msg.data.begin(), msg.data.end());
            ESP_LOGI(_tag.c_str(), "Received text: %s", text.c_str());

            ArduinoJson::JsonDocument doc;
            auto error = ArduinoJson::deserializeJson(doc, text);
            if (error) return;

            const char* msgType = doc["type"];
            if (!msgType) return;

            if (strcmp(msgType, "avatar") == 0) {
                // Direct avatar control: {"type":"avatar", "leftEye":{...}, "mouth":{...}}
                ArduinoJson::JsonDocument payload;
                if (doc["leftEye"].is<ArduinoJson::JsonObject>()) payload["leftEye"] = doc["leftEye"];
                if (doc["rightEye"].is<ArduinoJson::JsonObject>()) payload["rightEye"] = doc["rightEye"];
                if (doc["mouth"].is<ArduinoJson::JsonObject>()) payload["mouth"] = doc["mouth"];
                std::string out;
                ArduinoJson::serializeJson(payload, out);
                GetHAL().onWsAvatarData.emit(out);
            }
            else if (strcmp(msgType, "motion") == 0) {
                // Direct motion control: {"type":"motion", "yawServo":{...}, "pitchServo":{...}}
                ArduinoJson::JsonDocument payload;
                if (doc["yawServo"].is<ArduinoJson::JsonObject>()) payload["yawServo"] = doc["yawServo"];
                if (doc["pitchServo"].is<ArduinoJson::JsonObject>()) payload["pitchServo"] = doc["pitchServo"];
                std::string out;
                ArduinoJson::serializeJson(payload, out);
                GetHAL().onWsMotionData.emit(out);
            }
            else if (strcmp(msgType, "text") == 0) {
                // Display text: {"type":"text", "value":"hello", "name":"到"}
                WsTextMessage_t text_msg;
                text_msg.name = doc["name"] | "到";
                text_msg.content = doc["value"] | "";
                GetHAL().onWsTextMessage.emit(text_msg);
            }
            else if (strcmp(msgType, "dance") == 0) {
                // Dance sequence: {"type":"dance", "sequence":[...]}
                if (doc["sequence"].is<ArduinoJson::JsonArray>()) {
                    std::string out;
                    ArduinoJson::serializeJson(doc["sequence"], out);
                    GetHAL().onWsDanceData.emit(out);
                }
            }
            else if (strcmp(msgType, "camera_start") == 0) {
                setStreamingEnabled(true);
            }
            else if (strcmp(msgType, "camera_stop") == 0) {
                setStreamingEnabled(false);
            }
            else if (strcmp(msgType, "led") == 0) {
                // LED control: {"type":"led","r":0,"g":20,"b":0} or {"type":"led","index":3,"r":255,"g":0,"b":0}
                uint8_t r = doc["r"] | 0;
                uint8_t g = doc["g"] | 0;
                uint8_t b = doc["b"] | 0;
                if (doc["index"].is<int>()) {
                    int idx = doc["index"];
                    if (idx >= 0 && idx < 12) {
                        GetHAL().setRgbColor(idx, r, g, b);
                        GetHAL().refreshRgb();
                    }
                } else {
                    GetStackChan().leftNeonLight().setColor(r, g, b);
                    GetStackChan().rightNeonLight().setColor(r, g, b);
                }
            }
            else if (strcmp(msgType, "led_off") == 0) {
                GetStackChan().leftNeonLight().setColor(0, 0, 0);
                GetStackChan().rightNeonLight().setColor(0, 0, 0);
            }
            else if (strcmp(msgType, "neonlight") == 0) {
                std::string json;
                ArduinoJson::serializeJson(doc, json);
                GetStackChan().updateNeonLightFromJson(json.c_str());
            }
            else if (strcmp(msgType, "mic_on") == 0) {
                GetHAL().onMicMonitorToggle.emit(true);
                GetHAL().sendWsText("{\"type\":\"mic_ack\",\"status\":\"on\"}");
            }
            else if (strcmp(msgType, "mic_off") == 0) {
                GetHAL().onMicMonitorToggle.emit(false);
                GetHAL().sendWsText("{\"type\":\"mic_ack\",\"status\":\"off\"}");
            }
            else if (strcmp(msgType, "brightness") == 0) {
                // Screen brightness: {"type":"brightness","value":128}
                int val = doc["value"] | 128;
                GetHAL().setBackLightBrightness(val);
            }
            else if (strcmp(msgType, "ping") == 0) {
                _last_heartbeat_time = GetHAL().millis();
                _websocket->Send("{\"type\":\"pong\"}");
            }
            else if (strcmp(msgType, "status") == 0) {
                ArduinoJson::JsonDocument resp;
                resp["type"] = "status";
                resp["battery"] = GetHAL().getBatteryLevel();
                resp["charging"] = GetHAL().isBatteryCharging();
                resp["uptime"] = GetHAL().millis() / 1000;
                resp["camera"] = true;
                std::string out;
                ArduinoJson::serializeJson(resp, out);
                _websocket->Send(out.c_str());
            }
        }
    }

    bool isConnected()
    {
        return _websocket && _websocket->IsConnected();
    }

    void captureAndSendFrame()
    {
        if (!isConnected()) {
            return;
        }

        static int64_t _time_count = 0;
        static int64_t _interval   = 0;

        auto camera = hal_bridge::board_get_camera();
        if (!camera) {
            return;
        }

        _time_count = esp_timer_get_time();
        if (camera->StreamCaptures()) {
            _interval = esp_timer_get_time() - _time_count;
            mclog::info("camera capture time: {} ms", _interval / 1000);

            const uint8_t* frameData = camera->GetFrameData();
            size_t frameSize         = camera->GetFrameSize();
            int width                = camera->GetFrameWidth();
            int height               = camera->GetFrameHeight();
            int format               = camera->GetFrameFormat();

            uint8_t* jpeg_data = nullptr;
            size_t jpeg_len    = 0;

            // 压缩为 JPEG
            _time_count = esp_timer_get_time();
            if (image_to_jpeg((uint8_t*)frameData, frameSize, width, height, (v4l2_pix_fmt_t)format, 20, &jpeg_data,
                              &jpeg_len)) {
                _interval = esp_timer_get_time() - _time_count;
                // mclog::info("jpeg encode time: {} ms, size: {}", _interval / 1000, jpeg_len);
                mclog::info("jpeg encode time: {} ms", _interval / 1000);

                if (jpeg_data) {
                    sendPacket(DataType::Jpeg, jpeg_data, jpeg_len);  // Type 2 for JPEG
                    free(jpeg_data);
                }
            }
        }
    }

    void setStreamingEnabled(bool enabled)
    {
        _is_streaming = enabled;
    }

private:
    std::unique_ptr<WebSocket> _websocket;
    std::string _url;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_capture_time      = 0;
    uint32_t _last_heartbeat_time    = 0;
    bool _is_streaming               = false;
    bool _is_video_mode              = false;
    std::mutex _mutex;
    std::queue<ReceivedMessage> _msg_queue;

    std::mutex _send_mutex;

    void sendPacket(DataType type, const uint8_t* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(_send_mutex);
        if (!_websocket || !_websocket->IsConnected()) {
            return;
        }

        // mclog::info("sending packet type: {}, len: {}", (int)type, (int)len);

        // static int64_t _time_count = 0;
        // static int64_t _interval   = 0;
        // _time_count                = esp_timer_get_time();

        std::vector<uint8_t> packet;
        packet.reserve(1 + 4 + len);

        // [1 byte type]
        packet.push_back(static_cast<uint8_t>(type));

        // [4 bytes length] (Big Endian)
        uint32_t net_len       = htonl((uint32_t)len);
        const uint8_t* len_ptr = (const uint8_t*)&net_len;
        packet.push_back(len_ptr[0]);
        packet.push_back(len_ptr[1]);
        packet.push_back(len_ptr[2]);
        packet.push_back(len_ptr[3]);

        // [payload]
        if (len > 0) {
            packet.insert(packet.end(), data, data + len);
        }

        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("pack time: {} ms, size: {}", _interval / 1000, packet.size());

        // _time_count = esp_timer_get_time();
        _websocket->Send(packet.data(), packet.size(), true);
        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("send time: {} ms, size: {}", _interval / 1000, packet.size());
    }
};

class WebsocketAvatarWorker : public mooncake::BasicAbility {
public:
    WebsocketAvatarWorker()
    {
        _service = std::make_unique<WebSocketAvatar>();
        _service->init();
    }

    void onCreate() override
    {
    }

    void onRunning() override
    {
        if (GetHAL().millis() - _last_tick < 20) {
            return;
        }
        _last_tick = GetHAL().millis();
        _service->update();
    }

    void onDestroy() override
    {
        _service.reset();
    }

private:
    std::unique_ptr<WebSocketAvatar> _service;
    uint32_t _last_tick = 0;
};

void Hal::startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog)
{
    mclog::tagInfo(_tag, "start websocket avatar service");

    startNetwork(onStartLog);

    onStartLog("Connecting to\nserver...");
    mooncake::GetMooncake().extensionManager()->createAbility(std::make_unique<WebsocketAvatarWorker>());
}
