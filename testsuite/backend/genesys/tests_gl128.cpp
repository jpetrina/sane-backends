/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 vermiculous <vermiculous@disroot.org>

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define DEBUG_DECLARE_ONLY

#include "tests.h"
#include "tests_printers.h"
#include "minigtest.h"

#include "../../../backend/genesys/gl128.h"
#include "../../../backend/genesys/low.h"

#include <cmath>

namespace genesys {

namespace {

const Genesys_Model& opticfilm_8200i_se_model()
{
    for (const auto& usb_dev : *s_usb_devices) {
        if (usb_dev.vendor_id() == 0x07b3 && usb_dev.product_id() == 0x1825) {
            return usb_dev.model();
        }
    }
    throw SaneException("OpticFilm 8200i SE not in the USB device table");
}

// Scan settings as genesys derives them from the SANE options (mm).
Genesys_Settings make_settings(unsigned dpi, float tl_x, float tl_y, float width_mm,
                               float height_mm, ScanColorMode mode = ScanColorMode::COLOR_SINGLE_PASS)
{
    Genesys_Settings settings;
    settings.scan_method = ScanMethod::TRANSPARENCY;
    settings.scan_mode = mode;
    settings.xres = dpi;
    settings.yres = dpi;
    settings.tl_x = tl_x;
    settings.tl_y = tl_y;
    settings.pixels = static_cast<unsigned>(width_mm * dpi / MM_PER_INCH);
    settings.requested_pixels = settings.pixels;
    settings.lines = static_cast<unsigned>(height_mm * dpi / MM_PER_INCH);
    settings.depth = 16;
    settings.color_filter = mode == ScanColorMode::GRAY ? ColorFilter::GREEN : ColorFilter::NONE;
    return settings;
}

struct Gl128Fixture
{
    Genesys_Device dev;
    gl128::CommandSetGl128 cmd_set;

    Gl128Fixture()
    {
        dev.model = &opticfilm_8200i_se_model();
        for (const auto& motor : *s_motors) {
            if (motor.id == dev.model->motor_id) {
                dev.motor = motor;
            }
        }
    }

    ScanSession session(const Genesys_Settings& settings)
    {
        dev.settings = settings;
        const auto& sensor = sanei_genesys_find_sensor(&dev, settings.xres, 3,
                                                       settings.scan_method);
        return cmd_set.calculate_scan_session(&dev, sensor, settings);
    }
};

unsigned lcm(unsigned a, unsigned b)
{
    unsigned x = a, y = b;
    while (y) {
        unsigned t = x % y;
        x = y;
        y = t;
    }
    return a / x * b;
}

unsigned column_step(unsigned dpi)
{
    unsigned asic_dpi = dpi * gl128::gl128_host_downsample(dpi);
    return 7200 / asic_dpi;
}

} // namespace

// The model offers exactly SilverFast's resolutions, transparency only.
void test_gl128_model()
{
    const auto& model = opticfilm_8200i_se_model();
    ASSERT_TRUE(model.asic_type == AsicType::GL128);
    ASSERT_EQ(model.resolutions.size(), 1u);
    ASSERT_EQ(model.resolutions[0].methods.size(), 1u);
    ASSERT_TRUE(model.resolutions[0].methods[0] == ScanMethod::TRANSPARENCY);
    std::vector<unsigned> expected = { 7200, 3600, 2400, 1800, 1440, 1200, 900, 720, 600, 300, 150 };
    ASSERT_EQ(model.resolutions[0].resolutions_x, expected);
    ASSERT_EQ(model.bpp_color_values, std::vector<unsigned>{ 16 });
}

// Width: the native span is a multiple of lcm(4, 2 * step) and at least the
// requested width, so the output can be cropped to exactly the request.
void test_gl128_session_width()
{
    Gl128Fixture f;
    for (unsigned dpi : { 150u, 300u, 600u, 720u, 900u, 1200u, 1440u, 1800u, 2400u, 3600u, 7200u }) {
        for (float width : { 5.0f, 9.99f, 10.0f, 20.0f, 36.58f }) {
            auto settings = make_settings(dpi, 0, 0, width, 2);
            auto s = f.session(settings);
            unsigned step = column_step(dpi);
            unsigned align = lcm(4, 2 * step);
            unsigned span = s.pixel_endx - s.pixel_startx;
            ASSERT_EQ(span % align, 0u);
            ASSERT_TRUE(span * dpi / 7200 >= settings.requested_pixels);
            ASSERT_TRUE(span * dpi / 7200 < settings.requested_pixels + align);
            ASSERT_EQ(s.params.requested_pixels, settings.requested_pixels);
        }
    }
}

// CCD stagger: only odd column steps (7200, 2400, 1440) alternate between the
// two CCD rows. The shift is in 2x-oversampled rows, round(8 * dpi / 7200),
// and the window starts on an even native column that is a multiple of the
// step.
void test_gl128_session_stagger()
{
    Gl128Fixture f;
    struct Case { unsigned dpi; unsigned rows; };
    for (auto c : { Case{7200, 8}, Case{3600, 0}, Case{2400, 3}, Case{1800, 0},
                    Case{1440, 2}, Case{1200, 0}, Case{900, 0}, Case{720, 0},
                    Case{600, 0}, Case{300, 0}, Case{150, 0} })
    {
        for (float tl_x : { 0.0f, 3.3f, 10.0f }) {
            auto s = f.session(make_settings(c.dpi, tl_x, 0, 5, 2));
            ASSERT_EQ(s.num_staggered_lines, c.rows);
            if (c.rows > 0) {
                ASSERT_EQ(s.stagger_y.shifts(), (std::vector<std::size_t>{ c.rows, 0 }));
                ASSERT_EQ(s.pixel_startx % (2 * column_step(c.dpi)), 0u);
            } else {
                ASSERT_TRUE(s.stagger_y.empty());
            }
        }
    }
}

// X is mirrored: moving the crop right on the film moves the native window
// start left by the same distance.
void test_gl128_session_mirror()
{
    Gl128Fixture f;
    auto s0 = f.session(make_settings(3600, 0, 0, 10, 10));
    auto s10 = f.session(make_settings(3600, 10, 0, 10, 10));
    long shift = static_cast<long>(s0.pixel_startx) - static_cast<long>(s10.pixel_startx);
    long expected = lround(10 * 7200 / MM_PER_INCH);
    ASSERT_TRUE(std::abs(shift - expected) <= 4);
}

// Lines: LINCNT counts 4 per output line and the scanner returns 2 rows per
// line; below 600 dpi the ASIC scans at 600 and the host averages.
void test_gl128_session_lines()
{
    Gl128Fixture f;
    for (unsigned dpi : { 150u, 300u, 1800u, 7200u }) {
        auto settings = make_settings(dpi, 0, 0, 10, 5);
        auto s = f.session(settings);
        unsigned down = gl128::gl128_host_downsample(dpi);
        ASSERT_EQ(down, dpi < 600 ? 600 / dpi : 1u);
        ASSERT_EQ(s.params.lines, settings.lines * down);
        ASSERT_EQ(s.optical_line_count, s.params.lines * 2);
        ASSERT_EQ(s.output_line_bytes, s.params.pixels * 3 * 2);
        ASSERT_TRUE(s.buffer_size_read > 0);
    }
}

// Gray scans colour and reduces to one channel on the host.
void test_gl128_session_gray()
{
    Gl128Fixture f;
    auto s = f.session(make_settings(1440, 0, 0, 10, 5, ScanColorMode::GRAY));
    ASSERT_TRUE(s.use_host_side_gray);
    ASSERT_EQ(s.params.channels, 3u);
    ASSERT_TRUE(s.params.scan_mode == ScanColorMode::COLOR_SINGLE_PASS);
    ASSERT_TRUE(s.params.color_filter == ColorFilter::GREEN);

    auto c = f.session(make_settings(1440, 0, 0, 10, 5));
    ASSERT_FALSE(c.use_host_side_gray);
}

void test_gl128()
{
    genesys_init_usb_device_tables();
    genesys_init_sensor_tables();
    genesys_init_motor_tables();

    test_gl128_model();
    test_gl128_session_width();
    test_gl128_session_stagger();
    test_gl128_session_mirror();
    test_gl128_session_lines();
    test_gl128_session_gray();
}

} // namespace genesys
