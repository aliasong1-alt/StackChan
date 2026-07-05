/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <hal/hal.h>
#include <string_view>

namespace stackchan::addon {

class NeonLight {
public:
    NeonLight(int ledCount) : _led_count(ledCount)
    {
    }

    void init();
    void update();

    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setColor(uint32_t hex);
    void setColor(std::string_view hex);
    void setDuration(float durationSec);
    int getLedCount() const
    {
        return _led_count;
    }

protected:
    virtual void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void refresh_rgb_impl()                                                 = 0;

private:
    int _led_count;
    bool _dirty = false;
    uint8_t _r   = 0;
    uint8_t _g   = 0;
    uint8_t _b   = 0;
};

class LeftNeonLight : public NeonLight {
public:
    LeftNeonLight() : NeonLight(6)
    {
    }

private:
    void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void refresh_rgb_impl() override;
};

class RightNeonLight : public NeonLight {
public:
    RightNeonLight() : NeonLight(6)
    {
    }

private:
    void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void refresh_rgb_impl() override;
};

}  // namespace stackchan::addon
