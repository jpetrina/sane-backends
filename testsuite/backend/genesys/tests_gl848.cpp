/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE project contributors

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
#include "minigtest.h"

#include "../../../backend/genesys/gl848.h"
#include "../../../backend/genesys/low.h"

namespace genesys {

void ensure_gl848_tables()
{
    static bool done = false;
    if (done) {
        return;
    }
    genesys_init_usb_device_tables();
    genesys_init_sensor_tables();
    genesys_init_motor_tables();
    done = true;
}

const UsbDeviceEntry* find_scanjet_200()
{
    ensure_gl848_tables();
    for (const auto& usb_dev : *s_usb_devices) {
        if (usb_dev.vendor_id() == 0x03f0 && usb_dev.product_id() == 0x1c05) {
            return &usb_dev;
        }
    }
    return nullptr;
}

const UsbDeviceEntry* find_scanjet_300()
{
    ensure_gl848_tables();
    for (const auto& usb_dev : *s_usb_devices) {
        if (usb_dev.vendor_id() == 0x03f0 && usb_dev.product_id() == 0x1d05) {
            return &usb_dev;
        }
    }
    return nullptr;
}

void test_gl848_scanjet_300_device()
{
    const UsbDeviceEntry* found = find_scanjet_300();
    ASSERT_TRUE(found != nullptr);
    if (found == nullptr) {
        return;
    }

    ASSERT_EQ(std::string(found->model().name),
              std::string("hewlett-packard-scanjet-300"));
    ASSERT_TRUE(found->model().asic_type == AsicType::GL848);
    ASSERT_TRUE(found->model().model_id == ModelId::HP_SCANJET_300);
    ASSERT_TRUE(found->model().sensor_id == SensorId::CIS_HP_SCANJET_300);
    ASSERT_TRUE(found->model().motor_id == MotorId::HP_SCANJET_300);
    ASSERT_TRUE(found->model().is_cis);
    ASSERT_FALSE(found->model().is_sheetfed);

    // Params cloned from ScanJet 200 without vendor traces -- but
    // confirmed working for basic scanning (see gl848.cpp model-specific
    // shading floor). Buttons and native 4800dpi remain unimplemented.
    ASSERT_FALSE(has_flag(found->model().flags, ModelFlag::UNTESTED));
}

void test_gl848_host_shading_path()
{
    ASSERT_EQ(gl848::host_shading_path("/home/u/.sane/hewlett-packard-scanjet-200.cal", 300),
              std::string("/home/u/.sane/hewlett-packard-scanjet-200-shading-300.bin"));
    ASSERT_EQ(gl848::host_shading_path("/tmp/custom.cal", 2400),
              std::string("/tmp/custom-shading-2400.bin"));
    ASSERT_EQ(gl848::host_shading_path("/tmp/no-suffix", 600),
              std::string("/tmp/no-suffix-shading-600.bin"));
}

void test_gl848_usb_device_and_command_set()
{
    const UsbDeviceEntry* found = find_scanjet_200();
    ASSERT_TRUE(found != nullptr);
    if (found == nullptr) {
        return;
    }

    ASSERT_EQ(std::string(found->model().name),
              std::string("hewlett-packard-scanjet-200"));
    ASSERT_TRUE(found->model().asic_type == AsicType::GL848);
    ASSERT_TRUE(found->model().model_id == ModelId::HP_SCANJET_200);
    ASSERT_TRUE(found->model().sensor_id == SensorId::CIS_HP_SCANJET_200);
    ASSERT_TRUE(found->model().motor_id == MotorId::HP_SCANJET_200);
    ASSERT_TRUE(found->model().is_cis);
    ASSERT_FALSE(found->model().is_sheetfed);
    ASSERT_TRUE(found->model().x_offset > 3.5);
    ASSERT_TRUE(found->model().x_offset < 4.5);
    ASSERT_TRUE(found->model().x_size > 209.0);
    ASSERT_TRUE(found->model().x_size < 211.0);

    ASSERT_TRUE(has_flag(found->model().flags, ModelFlag::DISABLE_SHADING_CALIBRATION));
    ASSERT_TRUE(has_flag(found->model().flags, ModelFlag::DISABLE_ADC_CALIBRATION));
    ASSERT_TRUE(has_flag(found->model().flags, ModelFlag::DISABLE_EXPOSURE_CALIBRATION));
    ASSERT_TRUE(has_flag(found->model().flags, ModelFlag::SWAP_16BIT_DATA));
    ASSERT_FALSE(has_flag(found->model().flags, ModelFlag::UNTESTED));

    auto cmd = create_cmd_set(AsicType::GL848);
    ASSERT_TRUE(cmd.get() != nullptr);
}

void test_gl848_optical_pass_selection()
{
    struct Case {
        unsigned user_dpi;
        unsigned optical_dpi;
        unsigned scale;
        unsigned samples;
        unsigned feed;
    };
    const Case cases[] = {
        {   75,  300, 4,  2580, 543 },
        {  100,  300, 3,  2580, 543 },
        {  150,  300, 2,  2580, 543 },
        {  200,  600, 3,  5160, 489 },
        {  300,  300, 1,  2580, 543 },
        {  600,  600, 1,  5160, 489 },
        { 1200, 1200, 1, 10320, 552 },
        { 2400, 2400, 1, 20640, 552 },
    };
    for (const auto& c : cases) {
        auto pass = gl848::choose_optical_pass(c.user_dpi);
        ASSERT_EQ(pass.dpi, c.optical_dpi);
        ASSERT_EQ(pass.scale, c.scale);
        ASSERT_EQ(pass.samples_per_line, c.samples);
        ASSERT_EQ(pass.feed_lines, c.feed);
        ASSERT_TRUE(pass.page_lines > 0);
        ASSERT_EQ(pass.page_lines % 3, 0u);
    }
}

void test_gl848_pass_geometry()
{
    // A4 210×80 mm, centred on the 218 mm CIS (x = 4 … 214 mm).
    const float x0 = 4.0f;
    const float x1 = 214.0f;
    const float y0 = 0.0f;
    const float y1 = 80.0f;

    struct Case {
        unsigned dpi;
        unsigned optical;
        unsigned scale;
        unsigned out_w;
        unsigned out_h;
        unsigned feed;
    };
    const Case cases[] = {
        {   75,  300, 4,   621,  236, 543 },
        {  100,  300, 3,   828,  315, 543 },
        {  150,  300, 2,  1243,  472, 543 },
        {  200,  600, 3,  1656,  630, 489 },
        {  300,  300, 1,  2486,  945, 543 },
        {  600,  600, 1,  4970, 1890, 489 },
        { 1200, 1200, 1,  9942, 3780, 552 },
        { 2400, 2400, 1, 19882, 7557, 552 },
    };
    for (const auto& c : cases) {
        auto g = gl848::compute_pass_geometry(c.dpi, c.dpi, x0, y0, x1, y1);
        ASSERT_EQ(g.pass.dpi, c.optical);
        ASSERT_EQ(g.scale_x, c.scale);
        ASSERT_EQ(g.output_width, c.out_w);
        ASSERT_EQ(g.output_height, c.out_h);
        ASSERT_EQ(g.optical_xw, c.out_w * c.scale);
        ASSERT_EQ(g.skip_y, 0u);
        ASSERT_EQ(g.color_rows, c.out_h * c.scale);
        ASSERT_EQ(g.cis_lines, (g.skip_y + g.color_rows) * 3);
        ASSERT_EQ(g.pass.feed_lines, c.feed);
        ASSERT_TRUE(g.optical_x0 + g.optical_xw <= g.pass.samples_per_line);
    }

    // Y offset is a host skip: FEEDL stays the park-to-page constant.
    auto gy = gl848::compute_pass_geometry(150, 150, x0, 10.0f, x1, 90.0f);
    ASSERT_TRUE(gy.skip_y > 0);
    ASSERT_EQ(gy.pass.feed_lines, 543u);
    ASSERT_EQ(gy.cis_lines, (gy.skip_y + gy.color_rows) * 3);

    // Crop is clamped to the full-width CIS.
    auto gx = gl848::compute_pass_geometry(300, 300, 200.0f, 0.0f, 250.0f, 10.0f);
    ASSERT_TRUE(gx.optical_x0 < gx.pass.samples_per_line);
    ASSERT_TRUE(gx.optical_xw >= 1u);
    ASSERT_TRUE(gx.optical_x0 + gx.optical_xw <= gx.pass.samples_per_line);
    ASSERT_EQ(gx.pass.feed_lines, 543u);

    // Default scanimage area is the full A4 (210×297 mm).
    auto ga4 = gl848::compute_pass_geometry(600, 600, x0, 0.0f, x1, 297.0f);
    ASSERT_EQ(ga4.output_width, 4970u);
    ASSERT_EQ(ga4.output_height, 7016u);
    ASSERT_EQ(ga4.skip_y, 0u);
    ASSERT_EQ(ga4.pass.feed_lines, 489u);
}

void test_gl848_sensor_and_motor_tables()
{
    ensure_gl848_tables();
    bool tables_ok = true;
    try {
        verify_usb_device_tables();
        verify_sensor_tables();
    } catch (...) {
        tables_ok = false;
    }
    ASSERT_TRUE(tables_ok);

    unsigned sensor_count = 0;
    struct Expected {
        unsigned user_dpi;
        unsigned optical;
        unsigned factor;
    };
    const Expected expected[] = {
        {   75,  300, 4 },
        {  100,  300, 3 },
        {  150,  300, 2 },
        {  200,  600, 3 },
        {  300,  300, 1 },
        {  600,  600, 1 },
        { 1200, 1200, 1 },
        { 2400, 2400, 1 },
    };
    for (const auto& e : expected) {
        bool found = false;
        for (const auto& sensor : *s_sensors) {
            if (sensor.sensor_id != SensorId::CIS_HP_SCANJET_200) {
                continue;
            }
            if (sensor.register_dpiset != e.user_dpi) {
                continue;
            }
            found = true;
            sensor_count++;
            ASSERT_EQ(sensor.optical_resolution, e.optical);
            ASSERT_EQ(sensor.shading_resolution, e.optical);
            ASSERT_EQ(sensor.register_dpihw, e.optical);
            ASSERT_EQ(sensor.shading_factor, e.factor);
            ASSERT_EQ(sensor.full_resolution, 2400u);
            break;
        }
        ASSERT_TRUE(found);
    }
    ASSERT_EQ(sensor_count, 8u);

    bool motor_found = false;
    for (const auto& motor : *s_motors) {
        if (motor.id != MotorId::HP_SCANJET_200) {
            continue;
        }
        motor_found = true;
        ASSERT_EQ(motor.base_ydpi, 2400);
        break;
    }
    ASSERT_TRUE(motor_found);
}

void test_gl848()
{
    test_gl848_host_shading_path();
    test_gl848_usb_device_and_command_set();
    test_gl848_scanjet_300_device();
    test_gl848_optical_pass_selection();
    test_gl848_pass_geometry();
    test_gl848_sensor_and_motor_tables();
}

} // namespace genesys
