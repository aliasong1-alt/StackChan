/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "neon_light.h"
#include <hal/hal.h>

using namespace stackchan::addon;

static uint8_t hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void NeonLight::init()
{
    _r = 0;
    _g = 0;
    _b = 0;
    _dirty = true;
}

void NeonLight::update()
{
    if (!_dirty) {
        return;
    }
    _dirty = false;
    for (int i = 0; i < _led_count; i++) {
        set_rgb_color_impl(i, _r, _g, _b);
    }
    refresh_rgb_impl();
}

void NeonLight::setColor(uint8_t r, uint8_t g, uint8_t b)
{
    _r = r;
    _g = g;
    _b = b;
    _dirty = true;
}

void NeonLight::setColor(uint32_t hex)
{
    _r = (hex >> 16) & 0xFF;
    _g = (hex >> 8) & 0xFF;
    _b = hex & 0xFF;
    _dirty = true;
}

void NeonLight::setColor(std::string_view hex)
{
    size_t offset = 0;
    if (!hex.empty() && hex[0] == '#') offset = 1;
    if (hex.size() - offset >= 6) {
        _r = (hex_char_to_val(hex[offset]) << 4) | hex_char_to_val(hex[offset + 1]);
        _g = (hex_char_to_val(hex[offset + 2]) << 4) | hex_char_to_val(hex[offset + 3]);
        _b = (hex_char_to_val(hex[offset + 4]) << 4) | hex_char_to_val(hex[offset + 5]);
        _dirty = true;
    }
}

void NeonLight::setDuration(float durationSec)
{
    // No-op: animation removed, colors apply immediately
    (void)durationSec;
}

void LeftNeonLight::set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    GetHAL().setRgbColor(index, r, g, b);
}

void LeftNeonLight::refresh_rgb_impl()
{
    GetHAL().refreshRgb();
}

void RightNeonLight::set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    GetHAL().setRgbColor(index + 6, r, g, b);
}

void RightNeonLight::refresh_rgb_impl()
{
    GetHAL().refreshRgb();
}
