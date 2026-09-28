/* sane - Scanner Access Now Easy.

   Copyright (C) 2019 Povilas Kanapickas <povilas@radix.lt>

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

#include "test_scanner_interface.h"
#include "device.h"
#include <cstring>

namespace genesys {

TestScannerInterface::TestScannerInterface(Genesys_Device* dev, std::uint16_t vendor_id,
                                           std::uint16_t product_id, std::uint16_t bcd_device) :
    dev_{dev},
    usb_dev_{vendor_id, product_id, bcd_device}
{
    // initialize status registers
    if (dev_->model->asic_type == AsicType::GL124) {
        write_register(0x101, 0x00);
    } else if (dev_->model->asic_type == AsicType::GL126) {
        // GL126 (OpticFilm 135i): its status address is 0x101 like GL124's,
        // and its init()/offset_calibration read reg 0x01 directly (the
        // idle-homed check). Seed both so the backend can be driven in
        // test mode; 0x01 = 0x00 reads as "cold, never homed", which
        // makes offset_calibration stop early (before the lamp-warmup
        // loop), enough to observe that calibration was entered.
        write_register(0x01, 0x00);
        write_register(0x101, 0x00);
        // Regs 0x3b/0x3c: the magazine flow reads these unconditionally,
        // the Load film / Check status hooks as preconditions/
        // diagnostics, "eject" as a refusal check, and
        // RegisterCache::get() throws for an address that was never
        // written, unlike real hardware, which always answers a
        // register read with something. Seeded to 0x00/0x00 (the state
        // a device-open table leaves) so test-mode scenarios that only
        // care about reg 0x01/0x101 do not have to seed these too just
        // to avoid that mock-only throw.
        write_register(0x3B, 0x00);
        write_register(0x3C, 0x00);
    } else {
        write_register(0x41, 0x00);
    }
    if (dev_->model->asic_type == AsicType::GL841 ||
        dev_->model->asic_type == AsicType::GL842 ||
        dev_->model->asic_type == AsicType::GL843 ||
        dev_->model->asic_type == AsicType::GL845 ||
        dev_->model->asic_type == AsicType::GL846 ||
        dev_->model->asic_type == AsicType::GL847)
    {
        write_register(0x40, 0x00);
    }

    // initialize other registers that we read on init
    if (dev_->model->asic_type == AsicType::GL124) {
        write_register(0x33, 0x00);
        write_register(0xbd, 0x00);
        write_register(0xbe, 0x00);
        write_register(0x100, 0x00);
    }

    if (dev_->model->asic_type == AsicType::GL845 ||
        dev_->model->asic_type == AsicType::GL846 ||
        dev_->model->asic_type == AsicType::GL847)
    {
        write_register(0xbd, 0x00);
        write_register(0xbe, 0x00);

        write_register(0xd0, 0x00);
        write_register(0xd1, 0x01);
        write_register(0xd2, 0x02);
        write_register(0xd3, 0x03);
        write_register(0xd4, 0x04);
        write_register(0xd5, 0x05);
        write_register(0xd6, 0x06);
        write_register(0xd7, 0x07);
        write_register(0xd8, 0x08);
        write_register(0xd9, 0x09);
    }
}

TestScannerInterface::~TestScannerInterface() = default;

bool TestScannerInterface::is_mock() const
{
    return true;
}

std::uint8_t TestScannerInterface::read_register(std::uint16_t address)
{
    return cached_regs_.get(address);
}

void TestScannerInterface::write_register(std::uint16_t address, std::uint8_t value)
{
    cached_regs_.update(address, value);
    ++cache_write_count_;
}

void TestScannerInterface::write_registers(const Genesys_Register_Set& regs)
{
    cached_regs_.update(regs);
    ++cache_write_count_;
}


void TestScannerInterface::write_0x8c(std::uint8_t index, std::uint8_t value)
{
    (void) index;
    (void) value;
    ++cache_write_count_;
}

void TestScannerInterface::bulk_read_data(std::uint8_t addr, std::uint8_t* data, std::size_t size)
{
    (void) addr;
    std::memset(data, 0, size);
}

void TestScannerInterface::bulk_write_data(std::uint8_t addr, std::uint8_t* data, std::size_t size)
{
    (void) addr;
    (void) data;
    (void) size;
    ++cache_write_count_;
}

void TestScannerInterface::write_buffer(std::uint8_t type, std::uint32_t addr, std::uint8_t* data,
                                        std::size_t size)
{
    (void) type;
    (void) addr;
    (void) data;
    (void) size;
    ++cache_write_count_;
}

void TestScannerInterface::write_gamma(std::uint8_t type, std::uint32_t addr, std::uint8_t* data,
                                       std::size_t size)
{
    (void) type;
    (void) addr;
    (void) data;
    (void) size;
    ++cache_write_count_;
}

void TestScannerInterface::write_ahb(std::uint32_t addr, std::uint32_t size, std::uint8_t* data)
{
    (void) addr;
    (void) size;
    (void) data;
    ++cache_write_count_;
}

std::uint16_t TestScannerInterface::read_fe_register(std::uint8_t address)
{
    return cached_fe_regs_.get(address);
}

void TestScannerInterface::write_fe_register(std::uint8_t address, std::uint16_t value)
{
    cached_fe_regs_.update(address, value);
    ++cache_write_count_;
}

IUsbDevice& TestScannerInterface::get_usb_device()
{
    return usb_dev_;
}

void TestScannerInterface::sleep_us(unsigned microseconds)
{
    (void) microseconds;
}

void TestScannerInterface::record_slope_table(unsigned table_nr,
                                              const std::vector<std::uint16_t>& steps)
{
    slope_tables_[table_nr] = steps;
}

std::map<unsigned, std::vector<std::uint16_t>>& TestScannerInterface::recorded_slope_tables()
{
    return slope_tables_;
}

void TestScannerInterface::record_progress_message(const char* msg)
{
    last_progress_message_ = msg;
}

const std::string& TestScannerInterface::last_progress_message() const
{
    return last_progress_message_;
}

void TestScannerInterface::record_key_value(const std::string& key, const std::string& value)
{
    key_values_[key] = value;
}

std::map<std::string, std::string>& TestScannerInterface::recorded_key_values()
{
    return key_values_;
}

void TestScannerInterface::test_checkpoint(const std::string& name)
{
    if (checkpoint_callback_) {
        checkpoint_callback_(*dev_, *this, name);
    }
}

void TestScannerInterface::set_checkpoint_callback(TestCheckpointCallback callback)
{
    checkpoint_callback_ = callback;
}

} // namespace genesys
