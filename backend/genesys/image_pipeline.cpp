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

#include "image_pipeline.h"
#include "image.h"
#include "low.h"
#include <cmath>
#include <cstdio>
#include <numeric>

namespace genesys {

ImagePipelineNode::~ImagePipelineNode() {}

bool ImagePipelineNodeCallableSource::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = producer_(get_row_bytes(), out_data);
    if (!got_data)
        eof_ = true;
    return got_data;
}

ImagePipelineNodeBufferedCallableSource::ImagePipelineNodeBufferedCallableSource(
        std::size_t width, std::size_t height, PixelFormat format, std::size_t input_batch_size,
        ProducerCallback producer) :
    width_{width},
    height_{height},
    format_{format},
    buffer_{input_batch_size, producer}
{
    buffer_.set_remaining_size(height_ * get_row_bytes());
}

bool ImagePipelineNodeBufferedCallableSource::get_next_row_data(std::uint8_t* out_data)
{
    if (curr_row_ >= get_height()) {
        DBG(DBG_warn, "%s: reading out of bounds. Row %zu, height: %zu\n", __func__,
            curr_row_, get_height());
        eof_ = true;
        return false;
    }

    bool got_data = true;

    got_data &= buffer_.get_data(get_row_bytes(), out_data);
    curr_row_++;
    if (!got_data) {
        eof_ = true;
    }
    return got_data;
}

ImagePipelineNodeArraySource::ImagePipelineNodeArraySource(std::size_t width, std::size_t height,
                                                           PixelFormat format,
                                                           std::vector<std::uint8_t> data) :
    width_{width},
    height_{height},
    format_{format},
    data_{std::move(data)},
    next_row_{0}
{
    auto size = get_row_bytes() * height_;
    if (data_.size() < size) {
        throw SaneException("The given array is too small (%zu bytes). Need at least %zu",
                            data_.size(), size);
    }
}

bool ImagePipelineNodeArraySource::get_next_row_data(std::uint8_t* out_data)
{
    if (next_row_ >= height_) {
        eof_ = true;
        return false;
    }

    auto row_bytes = get_row_bytes();
    std::memcpy(out_data, data_.data() + row_bytes * next_row_, row_bytes);
    next_row_++;

    return true;
}


ImagePipelineNodeImageSource::ImagePipelineNodeImageSource(const Image& source) :
    source_{source}
{}

bool ImagePipelineNodeImageSource::get_next_row_data(std::uint8_t* out_data)
{
    if (next_row_ >= get_height()) {
        return false;
    }
    std::memcpy(out_data, source_.get_row_ptr(next_row_), get_row_bytes());
    next_row_++;
    return true;
}

bool ImagePipelineNodeFormatConvert::get_next_row_data(std::uint8_t* out_data)
{
    auto src_format = source_.get_format();
    if (src_format == dst_format_) {
        return source_.get_next_row_data(out_data);
    }

    buffer_.clear();
    buffer_.resize(source_.get_row_bytes());
    bool got_data = source_.get_next_row_data(buffer_.data());

    convert_pixel_row_format(buffer_.data(), src_format, out_data, dst_format_, get_width());
    return got_data;
}

ImagePipelineNodeDesegment::ImagePipelineNodeDesegment(ImagePipelineNode& source,
                                                       std::size_t output_width,
                                                       const std::vector<unsigned>& segment_order,
                                                       std::size_t segment_pixels,
                                                       std::size_t interleaved_lines,
                                                       std::size_t pixels_per_chunk) :
    source_(source),
    output_width_{output_width},
    segment_order_{segment_order},
    segment_pixels_{segment_pixels},
    interleaved_lines_{interleaved_lines},
    pixels_per_chunk_{pixels_per_chunk},
    buffer_{source_.get_row_bytes()}
{
    DBG_HELPER_ARGS(dbg, "segment_count=%zu, segment_size=%zu, interleaved_lines=%zu, "
                         "pixels_per_shunk=%zu", segment_order.size(), segment_pixels,
                    interleaved_lines, pixels_per_chunk);

    if (source_.get_height() % interleaved_lines_ > 0) {
        throw SaneException("Height is not a multiple of the number of lines to interelave %zu/%zu",
                            source_.get_height(), interleaved_lines_);
    }
}

ImagePipelineNodeDesegment::ImagePipelineNodeDesegment(ImagePipelineNode& source,
                                                       std::size_t output_width,
                                                       std::size_t segment_count,
                                                       std::size_t segment_pixels,
                                                       std::size_t interleaved_lines,
                                                       std::size_t pixels_per_chunk) :
    source_(source),
    output_width_{output_width},
    segment_pixels_{segment_pixels},
    interleaved_lines_{interleaved_lines},
    pixels_per_chunk_{pixels_per_chunk},
    buffer_{source_.get_row_bytes()}
{
    DBG_HELPER_ARGS(dbg, "segment_count=%zu, segment_size=%zu, interleaved_lines=%zu, "
                    "pixels_per_shunk=%zu", segment_count, segment_pixels, interleaved_lines,
                    pixels_per_chunk);

    segment_order_.resize(segment_count);
    std::iota(segment_order_.begin(), segment_order_.end(), 0);
}

bool ImagePipelineNodeDesegment::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    buffer_.clear();
    for (std::size_t i = 0; i < interleaved_lines_; ++i) {
        buffer_.push_back();
        got_data &= source_.get_next_row_data(buffer_.get_row_ptr(i));
    }
    if (!buffer_.is_linear()) {
        throw SaneException("Buffer is not linear");
    }

    auto format = get_format();
    auto segment_count = segment_order_.size();

    const std::uint8_t* in_data = buffer_.get_row_ptr(0);

    std::size_t groups_count = output_width_ / (segment_order_.size() * pixels_per_chunk_);

    for (std::size_t igroup = 0; igroup < groups_count; ++igroup) {
        for (std::size_t isegment = 0; isegment < segment_count; ++isegment) {
            auto input_offset = igroup * pixels_per_chunk_;
            input_offset += segment_pixels_ * segment_order_[isegment];
            auto output_offset = (igroup * segment_count + isegment) * pixels_per_chunk_;

            for (std::size_t ipixel = 0; ipixel < pixels_per_chunk_; ++ipixel) {
                auto pixel = get_raw_pixel_from_row(in_data, input_offset + ipixel, format);
                set_raw_pixel_to_row(out_data, output_offset + ipixel, pixel, format);
            }
        }
    }
    return got_data;
}

ImagePipelineNodeDeinterleaveLines::ImagePipelineNodeDeinterleaveLines(
        ImagePipelineNode& source, std::size_t interleaved_lines, std::size_t pixels_per_chunk) :
    ImagePipelineNodeDesegment(source, source.get_width() * interleaved_lines,
                               interleaved_lines, source.get_width(),
                               interleaved_lines, pixels_per_chunk)
{}

ImagePipelineNodeSwap16BitEndian::ImagePipelineNodeSwap16BitEndian(ImagePipelineNode& source) :
    source_(source),
    needs_swapping_{false}
{
    if (get_pixel_format_depth(source_.get_format()) == 16) {
        needs_swapping_ = true;
    } else {
        DBG(DBG_info, "%s: this pipeline node does nothing for non 16-bit formats", __func__);
    }
}

bool ImagePipelineNodeSwap16BitEndian::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = source_.get_next_row_data(out_data);
    if (needs_swapping_) {
        std::size_t pixels = get_row_bytes() / 2;
        for (std::size_t i = 0; i < pixels; ++i) {
            std::swap(*out_data, *(out_data + 1));
            out_data += 2;
        }
    }
    return got_data;
}

ImagePipelineNodeMirrorRows::ImagePipelineNodeMirrorRows(ImagePipelineNode& source) :
    source_(source)
{
}

bool ImagePipelineNodeMirrorRows::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = source_.get_next_row_data(out_data);
    auto format = get_format();
    auto width = get_width();
    for (std::size_t x = 0; x < width / 2; ++x) {
        auto left = get_raw_pixel_from_row(out_data, x, format);
        auto right = get_raw_pixel_from_row(out_data, width - 1 - x, format);
        set_raw_pixel_to_row(out_data, x, right, format);
        set_raw_pixel_to_row(out_data, width - 1 - x, left, format);
    }
    return got_data;
}

ImagePipelineNodeInvert::ImagePipelineNodeInvert(ImagePipelineNode& source) :
    source_(source)
{
}

bool ImagePipelineNodeInvert::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = source_.get_next_row_data(out_data);
    auto num_values = get_width() * get_pixel_channels(source_.get_format());
    auto depth = get_pixel_format_depth(source_.get_format());

    switch (depth) {
        case 16: {
            auto* data = reinterpret_cast<std::uint16_t*>(out_data);
            for (std::size_t i = 0; i < num_values; ++i) {
                *data = 0xffff - *data;
                data++;
            }
            break;
        }
        case 8: {
            auto* data = out_data;
            for (std::size_t i = 0; i < num_values; ++i) {
                *data = 0xff - *data;
                data++;
            }
            break;
        }
        case 1: {
            auto* data = out_data;
            auto num_bytes = (num_values + 7) / 8;
            for (std::size_t i = 0; i < num_bytes; ++i) {
                *data = ~*data;
                data++;
            }
            break;
        }
        default:
            throw SaneException("Unsupported pixel depth");
    }

    return got_data;
}

ImagePipelineNodeMergeMonoLinesToColor::ImagePipelineNodeMergeMonoLinesToColor(
        ImagePipelineNode& source, ColorOrder color_order) :
    source_(source),
    buffer_(source_.get_row_bytes())
{
    DBG_HELPER_ARGS(dbg, "color_order %d", static_cast<unsigned>(color_order));

    output_format_ = get_output_format(source_.get_format(), color_order);
}

bool ImagePipelineNodeMergeMonoLinesToColor::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    buffer_.clear();
    for (unsigned i = 0; i < 3; ++i) {
        buffer_.push_back();
        got_data &= source_.get_next_row_data(buffer_.get_row_ptr(i));
    }

    const auto* row0 = buffer_.get_row_ptr(0);
    const auto* row1 = buffer_.get_row_ptr(1);
    const auto* row2 = buffer_.get_row_ptr(2);

    auto format = source_.get_format();

    for (std::size_t x = 0, width = get_width(); x < width; ++x) {
        std::uint16_t ch0 = get_raw_channel_from_row(row0, x, 0, format);
        std::uint16_t ch1 = get_raw_channel_from_row(row1, x, 0, format);
        std::uint16_t ch2 = get_raw_channel_from_row(row2, x, 0, format);
        set_raw_channel_to_row(out_data, x, 0, ch0, output_format_);
        set_raw_channel_to_row(out_data, x, 1, ch1, output_format_);
        set_raw_channel_to_row(out_data, x, 2, ch2, output_format_);
    }
    return got_data;
}

PixelFormat ImagePipelineNodeMergeMonoLinesToColor::get_output_format(PixelFormat input_format,
                                                                      ColorOrder order)
{
    switch (input_format) {
        case PixelFormat::I1: {
            if (order == ColorOrder::RGB) {
                return PixelFormat::RGB111;
            }
            break;
        }
        case PixelFormat::I8: {
            if (order == ColorOrder::RGB) {
                return PixelFormat::RGB888;
            }
            if (order == ColorOrder::BGR) {
                return PixelFormat::BGR888;
            }
            break;
        }
        case PixelFormat::I16: {
            if (order == ColorOrder::RGB) {
                return PixelFormat::RGB161616;
            }
            if (order == ColorOrder::BGR) {
                return PixelFormat::BGR161616;
            }
            break;
        }
        default: break;
    }
    throw SaneException("Unsupported format combidation %d %d",
                        static_cast<unsigned>(input_format),
                        static_cast<unsigned>(order));
}

ImagePipelineNodeSplitMonoLines::ImagePipelineNodeSplitMonoLines(ImagePipelineNode& source) :
    source_(source),
    next_channel_{0}
{
    output_format_ = get_output_format(source_.get_format());
}

bool ImagePipelineNodeSplitMonoLines::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    if (next_channel_ == 0) {
        buffer_.resize(source_.get_row_bytes());
        got_data &= source_.get_next_row_data(buffer_.data());
    }

    const auto* row = buffer_.data();
    auto format = source_.get_format();

    for (std::size_t x = 0, width = get_width(); x < width; ++x) {
        std::uint16_t ch = get_raw_channel_from_row(row, x, next_channel_, format);
        set_raw_channel_to_row(out_data, x, 0, ch, output_format_);
    }
    next_channel_ = (next_channel_ + 1) % 3;

    return got_data;
}

PixelFormat ImagePipelineNodeSplitMonoLines::get_output_format(PixelFormat input_format)
{
    switch (input_format) {
        case PixelFormat::RGB111: return PixelFormat::I1;
        case PixelFormat::RGB888:
        case PixelFormat::BGR888: return PixelFormat::I8;
        case PixelFormat::RGB161616:
        case PixelFormat::BGR161616: return PixelFormat::I16;
        default: break;
    }
    throw SaneException("Unsupported input format %d", static_cast<unsigned>(input_format));
}


namespace {

void assign_gray_channel_order(ColorOrder order, float red, float green, float blue,
                               float& ch0, float& ch1, float& ch2)
{
    switch (order) {
        case ColorOrder::RGB:
            ch0 = red; ch1 = green; ch2 = blue;
            break;
        case ColorOrder::BGR:
            ch0 = blue; ch1 = green; ch2 = red;
            break;
        case ColorOrder::GBR:
            ch0 = green; ch1 = blue; ch2 = red;
            break;
        default:
            throw SaneException("Unknown color order");
    }
}

void assign_gray_channel_order(ColorOrder order, unsigned red, unsigned green, unsigned blue,
                               unsigned& ch0, unsigned& ch1, unsigned& ch2)
{
    switch (order) {
        case ColorOrder::RGB:
            ch0 = red; ch1 = green; ch2 = blue;
            break;
        case ColorOrder::BGR:
            ch0 = blue; ch1 = green; ch2 = red;
            break;
        case ColorOrder::GBR:
            ch0 = green; ch1 = blue; ch2 = red;
            break;
        default:
            throw SaneException("Unknown color order");
    }
}

} // namespace

ImagePipelineNodeMergeColorToGray::ImagePipelineNodeMergeColorToGray(ImagePipelineNode& source) :
    source_(source)
{
    output_format_ = get_output_format(source_.get_format());
    assign_gray_channel_order(get_pixel_format_color_order(source_.get_format()),
                              0.2125f, 0.7154f, 0.0721f,
                              ch0_mult_, ch1_mult_, ch2_mult_);
    temp_buffer_.resize(source_.get_row_bytes());
}

ImagePipelineNodeMergeColorToGray::ImagePipelineNodeMergeColorToGray(
        ImagePipelineNode& source, unsigned wr, unsigned wg, unsigned wb, unsigned wdiv) :
    source_(source),
    wdiv_{wdiv == 0 ? 1u : wdiv},
    integer_weights_{true}
{
    output_format_ = get_output_format(source_.get_format());
    assign_gray_channel_order(get_pixel_format_color_order(source_.get_format()),
                              wr, wg, wb, w0_, w1_, w2_);
    temp_buffer_.resize(source_.get_row_bytes());
}

bool ImagePipelineNodeMergeColorToGray::get_next_row_data(std::uint8_t* out_data)
{
    auto* src_data = temp_buffer_.data();

    bool got_data = source_.get_next_row_data(src_data);

    auto src_format = source_.get_format();

    for (std::size_t x = 0, width = get_width(); x < width; ++x) {
        std::uint16_t ch0 = get_raw_channel_from_row(src_data, x, 0, src_format);
        std::uint16_t ch1 = get_raw_channel_from_row(src_data, x, 1, src_format);
        std::uint16_t ch2 = get_raw_channel_from_row(src_data, x, 2, src_format);
        std::uint16_t mono = 0;
        if (integer_weights_) {
            mono = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(ch0) * w0_ +
                 static_cast<std::uint32_t>(ch1) * w1_ +
                 static_cast<std::uint32_t>(ch2) * w2_) / wdiv_);
        } else {
            float value = ch0 * ch0_mult_ + ch1 * ch1_mult_ + ch2 * ch2_mult_;
            mono = static_cast<std::uint16_t>(value);
        }
        set_raw_channel_to_row(out_data, x, 0, mono, output_format_);
    }
    return got_data;
}

PixelFormat ImagePipelineNodeMergeColorToGray::get_output_format(PixelFormat input_format)
{
    switch (input_format) {
        case PixelFormat::RGB111:
            return PixelFormat::I1;
        case PixelFormat::RGB888:
        case PixelFormat::BGR888:
            return PixelFormat::I8;
        case PixelFormat::RGB161616:
        case PixelFormat::BGR161616:
            return PixelFormat::I16;
        default: break;
    }
    throw SaneException("Unsupported format %d", static_cast<unsigned>(input_format));
}

ImagePipelineNodeComponentShiftLines::ImagePipelineNodeComponentShiftLines(
        ImagePipelineNode& source, unsigned shift_r, unsigned shift_g, unsigned shift_b) :
    source_(source),
    buffer_{source.get_row_bytes()}
{
    DBG_HELPER_ARGS(dbg, "shifts={%d, %d, %d}", shift_r, shift_g, shift_b);

    switch (source.get_format()) {
        case PixelFormat::RGB111:
        case PixelFormat::RGB888:
        case PixelFormat::RGB161616: {
            channel_shifts_ = { shift_r, shift_g, shift_b };
            break;
        }
        case PixelFormat::BGR888:
        case PixelFormat::BGR161616: {
            channel_shifts_ = { shift_b, shift_g, shift_r };
            break;
        }
        default:
            throw SaneException("Unsupported input format %d",
                                static_cast<unsigned>(source.get_format()));
    }
    extra_height_ = *std::max_element(channel_shifts_.begin(), channel_shifts_.end());
    height_ = source_.get_height();
    if (extra_height_ > height_) {
        height_ = 0;
    } else {
        height_ -= extra_height_;
    }
}

bool ImagePipelineNodeComponentShiftLines::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    if (!buffer_.empty()) {
        buffer_.pop_front();
    }
    while (buffer_.height() < extra_height_ + 1) {
        buffer_.push_back();
        got_data &= source_.get_next_row_data(buffer_.get_back_row_ptr());
    }

    auto format = get_format();
    const auto* row0 = buffer_.get_row_ptr(channel_shifts_[0]);
    const auto* row1 = buffer_.get_row_ptr(channel_shifts_[1]);
    const auto* row2 = buffer_.get_row_ptr(channel_shifts_[2]);

    for (std::size_t x = 0, width = get_width(); x < width; ++x) {
        std::uint16_t ch0 = get_raw_channel_from_row(row0, x, 0, format);
        std::uint16_t ch1 = get_raw_channel_from_row(row1, x, 1, format);
        std::uint16_t ch2 = get_raw_channel_from_row(row2, x, 2, format);
        set_raw_channel_to_row(out_data, x, 0, ch0, format);
        set_raw_channel_to_row(out_data, x, 1, ch1, format);
        set_raw_channel_to_row(out_data, x, 2, ch2, format);
    }
    return got_data;
}

ImagePipelineNodePixelShiftLines::ImagePipelineNodePixelShiftLines(
        ImagePipelineNode& source, const std::vector<std::size_t>& shifts) :
    source_(source),
    pixel_shifts_{shifts},
    buffer_{get_row_bytes()}
{
    extra_height_ = *std::max_element(pixel_shifts_.begin(), pixel_shifts_.end());
    height_ = source_.get_height();
    if (extra_height_ > height_) {
        height_ = 0;
    } else {
        height_ -= extra_height_;
    }
}

bool ImagePipelineNodePixelShiftLines::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    if (!buffer_.empty()) {
        buffer_.pop_front();
    }
    while (buffer_.height() < extra_height_ + 1) {
        buffer_.push_back();
        got_data &= source_.get_next_row_data(buffer_.get_back_row_ptr());
    }

    auto format = get_format();
    auto shift_count = pixel_shifts_.size();

    std::vector<std::uint8_t*> rows;
    rows.resize(shift_count, nullptr);

    for (std::size_t irow = 0; irow < shift_count; ++irow) {
        rows[irow] = buffer_.get_row_ptr(pixel_shifts_[irow]);
    }

    for (std::size_t x = 0, width = get_width(); x < width;) {
        for (std::size_t irow = 0; irow < shift_count && x < width; irow++, x++) {
            RawPixel pixel = get_raw_pixel_from_row(rows[irow], x, format);
            set_raw_pixel_to_row(out_data, x, pixel, format);
        }
    }
    return got_data;
}

ImagePipelineNodePixelShiftColumns::ImagePipelineNodePixelShiftColumns(
        ImagePipelineNode& source, const std::vector<std::size_t>& shifts) :
    source_(source),
    pixel_shifts_{shifts}
{
    width_ = source_.get_width();
    extra_width_ = compute_pixel_shift_extra_width(width_, pixel_shifts_);
    if (extra_width_ > width_) {
        width_ = 0;
    } else {
        width_ -= extra_width_;
    }
    temp_buffer_.resize(source_.get_row_bytes());
}

bool ImagePipelineNodePixelShiftColumns::get_next_row_data(std::uint8_t* out_data)
{
    if (width_ == 0) {
        throw SaneException("Attempt to read zero-width line");
    }
    bool got_data = source_.get_next_row_data(temp_buffer_.data());

    auto format = get_format();
    auto shift_count = pixel_shifts_.size();

    for (std::size_t x = 0, width = get_width(); x < width; x += shift_count) {
        for (std::size_t ishift = 0; ishift < shift_count && x + ishift < width; ishift++) {
            RawPixel pixel = get_raw_pixel_from_row(temp_buffer_.data(), x + pixel_shifts_[ishift],
                                                    format);
            set_raw_pixel_to_row(out_data, x + ishift, pixel, format);
        }
    }
    return got_data;
}


std::size_t compute_pixel_shift_extra_width(std::size_t source_width,
                                            const std::vector<std::size_t>& shifts)
{
    // we iterate across pixel shifts and find the pixel that needs the maximum shift according to
    // source_width.
    int group_size = shifts.size();
    int non_filled_group = source_width % shifts.size();
    int extra_width = 0;

    for (int i = 0; i < group_size; ++i) {
        int shift_groups = shifts[i] / group_size;
        int shift_rem = shifts[i] % group_size;

        if (shift_rem < non_filled_group) {
            shift_groups--;
        }
        extra_width = std::max(extra_width, shift_groups * group_size + non_filled_group - i);
    }
    return extra_width;
}

ImagePipelineNodeExtract::ImagePipelineNodeExtract(ImagePipelineNode& source,
                                                   std::size_t offset_x, std::size_t offset_y,
                                                   std::size_t width, std::size_t height) :
    source_(source),
    offset_x_{offset_x},
    offset_y_{offset_y},
    width_{width},
    height_{height}
{
    cached_line_.resize(source_.get_row_bytes());
}

ImagePipelineNodeExtract::~ImagePipelineNodeExtract() {}

ImagePipelineNodeScaleRows::ImagePipelineNodeScaleRows(ImagePipelineNode& source,
                                                       std::size_t width) :
    source_(source),
    width_{width}
{
    cached_line_.resize(source_.get_row_bytes());
}

bool ImagePipelineNodeScaleRows::get_next_row_data(std::uint8_t* out_data)
{
    auto src_width = source_.get_width();
    auto dst_width = width_;

    bool got_data = source_.get_next_row_data(cached_line_.data());

    const auto* src_data = cached_line_.data();
    auto format = get_format();
    auto channels = get_pixel_channels(format);

    if (src_width > dst_width) {
        // average
        std::uint32_t counter = src_width / 2;
        unsigned src_x = 0;
        for (unsigned dst_x = 0; dst_x < dst_width; dst_x++) {
            unsigned avg[3] = {0, 0, 0};
            unsigned count = 0;
            while (counter < src_width && src_x < src_width) {
                counter += dst_width;

                for (unsigned c = 0; c < channels; c++) {
                    avg[c] += get_raw_channel_from_row(src_data, src_x, c, format);
                }

                src_x++;
                count++;
            }
            counter -= src_width;

            for (unsigned c = 0; c < channels; c++) {
                set_raw_channel_to_row(out_data, dst_x, c, avg[c] / count, format);
            }
        }
    } else {
        // interpolate and copy pixels
        std::uint32_t counter = dst_width / 2;
        unsigned dst_x = 0;

        for (unsigned src_x = 0; src_x < src_width; src_x++) {
            unsigned avg[3] = {0, 0, 0};
            for (unsigned c = 0; c < channels; c++) {
                avg[c] += get_raw_channel_from_row(src_data, src_x, c, format);
            }
            while ((counter < dst_width || src_x + 1 == src_width) && dst_x < dst_width) {
                counter += src_width;

                for (unsigned c = 0; c < channels; c++) {
                    set_raw_channel_to_row(out_data, dst_x, c, avg[c], format);
                }
                dst_x++;
            }
            counter -= dst_width;
        }
    }
    return got_data;
}

ImagePipelineNodeDecimateLines::ImagePipelineNodeDecimateLines(ImagePipelineNode& source,
                                                               unsigned factor) :
    source_(source),
    factor_{factor}
{
    if (factor_ == 0) {
        throw SaneException("Decimate factor must be greater than zero");
    }
    height_ = source_.get_height() / factor_;
    temp_.resize(source_.get_row_bytes());
    accum_.resize(get_width() * get_pixel_channels(get_format()));
}

bool ImagePipelineNodeDecimateLines::get_next_row_data(std::uint8_t* out_data)
{
    auto format = get_format();
    auto channels = get_pixel_channels(format);
    auto width = get_width();

    std::fill(accum_.begin(), accum_.end(), 0);

    bool got_data = true;
    for (unsigned n = 0; n < factor_; ++n) {
        got_data &= source_.get_next_row_data(temp_.data());
        for (std::size_t x = 0; x < width; ++x) {
            for (unsigned c = 0; c < channels; ++c) {
                accum_[x * channels + c] += get_raw_channel_from_row(temp_.data(), x, c, format);
            }
        }
    }

    for (std::size_t x = 0; x < width; ++x) {
        for (unsigned c = 0; c < channels; ++c) {
            set_raw_channel_to_row(out_data, x, c,
                                   static_cast<std::uint16_t>(accum_[x * channels + c] / factor_),
                                   format);
        }
    }
    return got_data;
}

ImagePipelineNodeDecimateColumns::ImagePipelineNodeDecimateColumns(ImagePipelineNode& source,
                                                                   unsigned factor) :
    source_(source),
    factor_{factor}
{
    if (factor_ == 0) {
        throw SaneException("Decimate factor must be greater than zero");
    }
    width_ = source_.get_width() / factor_;
    if (width_ == 0) {
        width_ = 1;
    }
    temp_.resize(source_.get_row_bytes());
}

bool ImagePipelineNodeDecimateColumns::get_next_row_data(std::uint8_t* out_data)
{
    auto format = get_format();
    auto channels = get_pixel_channels(format);
    bool got_data = source_.get_next_row_data(temp_.data());
    const std::size_t src_width = source_.get_width();

    for (std::size_t ox = 0; ox < width_; ++ox) {
        const std::size_t from = ox * factor_;
        for (unsigned c = 0; c < channels; ++c) {
            std::uint32_t sum = 0;
            unsigned count = 0;
            for (unsigned k = 0; k < factor_; ++k) {
                const std::size_t x = from + k;
                if (x >= src_width) {
                    break;
                }
                sum += get_raw_channel_from_row(temp_.data(), x, c, format);
                count++;
            }
            if (count == 0) {
                count = 1;
            }
            set_raw_channel_to_row(out_data, ox, c,
                                   static_cast<std::uint16_t>(sum / count),
                                   format);
        }
    }
    return got_data;
}

ImagePipelineNodeThreshold8::ImagePipelineNodeThreshold8(ImagePipelineNode& source,
                                                         std::uint8_t cutoff) :
    source_(source),
    cutoff_{cutoff}
{
    auto format = source_.get_format();
    if (get_pixel_format_depth(format) != 8 || get_pixel_channels(format) != 1) {
        throw SaneException("Threshold8 requires 8-bit gray input");
    }
}

bool ImagePipelineNodeThreshold8::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = source_.get_next_row_data(out_data);
    auto format = get_format();
    for (std::size_t x = 0, width = get_width(); x < width; ++x) {
        auto value = get_raw_channel_from_row(out_data, x, 0, format);
        set_raw_channel_to_row(out_data, x, 0, value >= cutoff_ ? 255 : 0, format);
    }
    return got_data;
}

namespace {

// 600 dpi A4 RGB16 is ~209 MB; 1200 dpi ~837 MB. 2400 dpi spills to a
// temp file instead of a 3 GB RAM buffer.
const std::size_t AUTOLEVELS_MAX_BYTES = 1024u * 1024u * 1024u;
// Same 10% floor / full-scale white as a linear CIS decode without shading.
const std::uint16_t AUTOLEVELS_FALLBACK_BLACK = 6554;
const std::uint16_t AUTOLEVELS_FALLBACK_WHITE = 65535;

double encode_srgb(double linear)
{
    if (linear <= 0.0) {
        return 0.0;
    }
    if (linear >= 1.0) {
        return 1.0;
    }
    if (linear <= 0.0031308) {
        return 12.92 * linear;
    }
    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

} // namespace

void ImagePipelineNodeAutoLevels::build_srgb_lut(std::uint16_t black, std::uint16_t white,
                                                 std::vector<std::uint16_t>& lut)
{
    lut.assign(65536, 0);
    const double lo = black;
    const double span = std::max(1.0, static_cast<double>(white) - lo);
    for (unsigned v = static_cast<unsigned>(black) + 1; v < 65536; ++v) {
        double linear = (static_cast<double>(v) - lo) / span;
        if (linear > 1.0) {
            linear = 1.0;
        }
        lut[v] = static_cast<std::uint16_t>(encode_srgb(linear) * 65535.0 + 0.5);
    }
}

ImagePipelineNodeAutoLevels::ImagePipelineNodeAutoLevels(ImagePipelineNode& source) :
    source_(source)
{
    const auto depth = get_pixel_format_depth(source_.get_format());
    const std::size_t bytes = source_.get_height() * source_.get_row_bytes();
    streaming_ = depth != 16 || bytes > AUTOLEVELS_MAX_BYTES;
    if (streaming_) {
        build_srgb_lut(AUTOLEVELS_FALLBACK_BLACK, AUTOLEVELS_FALLBACK_WHITE, lut_);
        ready_ = true;
        DBG(DBG_info, "AutoLevels: streaming fallback black=%u white=%u (%zu bytes)\n",
            AUTOLEVELS_FALLBACK_BLACK, AUTOLEVELS_FALLBACK_WHITE, bytes);
    }
}

ImagePipelineNodeAutoLevels::ImagePipelineNodeAutoLevels(ImagePipelineNode& source,
                                                         std::uint16_t black,
                                                         std::uint16_t white) :
    ImagePipelineNodeAutoLevels(source, black, white, false)
{
}

ImagePipelineNodeAutoLevels::ImagePipelineNodeAutoLevels(ImagePipelineNode& source,
                                                         std::uint16_t black,
                                                         std::uint16_t white,
                                                         bool adapt_black) :
    source_(source),
    adapt_black_{adapt_black},
    floor_black_{black},
    white_{white == 0 ? std::uint16_t{1} : white}
{
    const auto depth = get_pixel_format_depth(source_.get_format());
    const std::size_t bytes = source_.get_height() * source_.get_row_bytes();
    if (adapt_black_ && depth == 16) {
        streaming_ = false;
        ready_ = false;
        DBG(DBG_info, "AutoLevels: calibrated adapt black floor=%u white=%u (%zu bytes)%s\n",
            floor_black_, white_, bytes,
            bytes > AUTOLEVELS_MAX_BYTES ? " (spill)" : "");
        return;
    }
    streaming_ = true;
    ready_ = true;
    build_srgb_lut(floor_black_, white_, lut_);
    DBG(DBG_info, "AutoLevels: fixed black=%u white=%u (%zu bytes)\n",
        floor_black_, white_, bytes);
}

bool ImagePipelineNodeAutoLevels::eof() const
{
    if (streaming_) {
        return source_.eof();
    }
    return next_row_ >= get_height();
}

void ImagePipelineNodeAutoLevels::apply_lut_to_row(std::uint8_t* row)
{
    auto format = get_format();
    auto channels = get_pixel_channels(format);
    auto width = get_width();
    for (std::size_t x = 0; x < width; ++x) {
        for (unsigned c = 0; c < channels; ++c) {
            auto value = get_raw_channel_from_row(row, x, c, format);
            set_raw_channel_to_row(row, x, c, lut_[value], format);
        }
    }
}

void ImagePipelineNodeAutoLevels::ensure_lut()
{
    if (ready_) {
        return;
    }

    auto format = get_format();
    auto channels = get_pixel_channels(format);
    auto width = get_width();
    const std::size_t height = get_height();
    const std::size_t row_bytes = get_row_bytes();
    const std::size_t bytes = height * row_bytes;
    const bool spill = adapt_black_ && bytes > AUTOLEVELS_MAX_BYTES;

    std::vector<std::uint8_t> scratch;
    if (spill) {
        std::FILE* fp = std::tmpfile();
        if (fp == nullptr) {
            DBG(DBG_warn, "AutoLevels: tmpfile failed, using floor black=%u\n",
                floor_black_);
            build_srgb_lut(floor_black_, white_, lut_);
            streaming_ = true;
            ready_ = true;
            return;
        }
        spill_file_.reset(fp);
        scratch.resize(row_bytes);
        DBG(DBG_info, "AutoLevels: spilling %zu bytes to temp file\n", bytes);
    } else {
        buffer_.resize(bytes);
    }

    std::vector<std::uint32_t> hist(65536, 0);
    std::uint64_t total = 0;
    for (std::size_t y = 0; y < height; ++y) {
        std::uint8_t* row = spill ? scratch.data() : buffer_.data() + y * row_bytes;
        source_.get_next_row_data(row);
        if (spill && std::fwrite(row, 1, row_bytes, spill_file_.get()) != row_bytes) {
            DBG(DBG_error, "AutoLevels: temp write failed at row %zu\n", y);
        }
        for (std::size_t x = 0; x < width; ++x) {
            for (unsigned c = 0; c < channels; ++c) {
                hist[get_raw_channel_from_row(row, x, c, format)]++;
                total++;
            }
        }
    }

    std::uint16_t black = 0;
    std::uint16_t white = 65535;
    if (adapt_black_) {
        white = white_;
        black = floor_black_;
        if (total > 0) {
            const std::uint64_t idx_lo = total / 200;
            std::uint64_t acc = 0;
            for (unsigned v = 0; v < 65536; ++v) {
                acc += hist[v];
                if (acc > idx_lo) {
                    black = static_cast<std::uint16_t>(v);
                    break;
                }
            }
        }
        if (black > floor_black_) {
            black = floor_black_;
        }
        DBG(DBG_info, "AutoLevels: calibrated black=%u (floor=%u) white=%u samples=%llu\n",
            black, floor_black_, white, static_cast<unsigned long long>(total));
    } else if (total > 0) {
        const std::uint64_t idx_lo = total * 2 / 100;
        const std::uint64_t idx_hi = total * 98 / 100;
        std::uint64_t acc = 0;
        bool have_black = false;
        for (unsigned v = 0; v < 65536; ++v) {
            acc += hist[v];
            if (!have_black && acc > idx_lo) {
                black = static_cast<std::uint16_t>(v);
                have_black = true;
            }
            if (acc > idx_hi) {
                white = static_cast<std::uint16_t>(v);
                break;
            }
        }
        DBG(DBG_info, "AutoLevels: percentile black=%u white=%u samples=%llu\n",
            black, white, static_cast<unsigned long long>(total));
    }
    if (white <= black) {
        white = static_cast<std::uint16_t>(std::min(65535, static_cast<int>(black) + 1));
    }

    build_srgb_lut(black, white, lut_);

    if (spill) {
        std::rewind(spill_file_.get());
    } else {
        for (std::size_t y = 0; y < height; ++y) {
            apply_lut_to_row(buffer_.data() + y * row_bytes);
        }
    }
    ready_ = true;
}

bool ImagePipelineNodeAutoLevels::get_next_row_data(std::uint8_t* out_data)
{
    if (streaming_) {
        bool got_data = source_.get_next_row_data(out_data);
        if (get_pixel_format_depth(get_format()) == 16) {
            apply_lut_to_row(out_data);
        }
        return got_data;
    }

    ensure_lut();
    if (next_row_ >= get_height()) {
        std::fill(out_data, out_data + get_row_bytes(), 0);
        return false;
    }
    if (spill_file_) {
        const std::size_t row_bytes = get_row_bytes();
        if (std::fread(out_data, 1, row_bytes, spill_file_.get()) != row_bytes) {
            std::fill(out_data, out_data + row_bytes, 0);
            return false;
        }
        apply_lut_to_row(out_data);
    } else {
        std::memcpy(out_data, buffer_.data() + next_row_ * get_row_bytes(), get_row_bytes());
    }
    next_row_++;
    return true;
}

ImagePipelineNodeFlatField::ImagePipelineNodeFlatField(ImagePipelineNode& source,
                                                       std::vector<std::uint16_t> ref_r,
                                                       std::vector<std::uint16_t> ref_g,
                                                       std::vector<std::uint16_t> ref_b,
                                                       std::uint16_t target) :
    source_(source),
    target_{target == 0 ? std::uint16_t{1} : target}
{
    ref_[0] = std::move(ref_r);
    ref_[1] = std::move(ref_g);
    ref_[2] = std::move(ref_b);
}

ImagePipelineNodeFlatField::ImagePipelineNodeFlatField(ImagePipelineNode& source,
                                                       std::vector<std::uint16_t> ref_r,
                                                       std::vector<std::uint16_t> ref_g,
                                                       std::vector<std::uint16_t> ref_b,
                                                       std::vector<std::uint16_t> black_r,
                                                       std::vector<std::uint16_t> black_g,
                                                       std::vector<std::uint16_t> black_b,
                                                       std::uint16_t target) :
    source_(source),
    target_{target == 0 ? std::uint16_t{1} : target}
{
    ref_[0] = std::move(ref_r);
    ref_[1] = std::move(ref_g);
    ref_[2] = std::move(ref_b);
    black_[0] = std::move(black_r);
    black_[1] = std::move(black_g);
    black_[2] = std::move(black_b);
}

bool ImagePipelineNodeFlatField::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = source_.get_next_row_data(out_data);
    auto format = get_format();
    auto channels = get_pixel_channels(format);
    auto width = get_width();
    const unsigned use_ch = std::min(channels, 3u);
    for (std::size_t x = 0; x < width; ++x) {
        for (unsigned c = 0; c < use_ch; ++c) {
            if (x >= ref_[c].size()) {
                continue;
            }
            const std::uint16_t ref = ref_[c][x];
            if (ref <= 256) {
                continue;
            }
            std::uint32_t value = get_raw_channel_from_row(out_data, x, c, format);
            if (x < black_[c].size()) {
                const std::uint32_t black = black_[c][x];
                const std::uint32_t span = ref > black ? (ref - black) : 1;
                value = value > black ? value - black : 0;
                value = std::min<std::uint32_t>(65535, value * target_ / span);
            } else {
                value = std::min<std::uint32_t>(65535, value * target_ / ref);
            }
            set_raw_channel_to_row(out_data, x, c, static_cast<std::uint16_t>(value), format);
        }
    }
    return got_data;
}

bool ImagePipelineNodeExtract::get_next_row_data(std::uint8_t* out_data)
{
    bool got_data = true;

    while (current_line_ < offset_y_) {
        got_data &= source_.get_next_row_data(cached_line_.data());
        current_line_++;
    }
    if (current_line_ >= offset_y_ + source_.get_height()) {
        std::fill(out_data, out_data + get_row_bytes(), 0);
        current_line_++;
        return got_data;
    }
    // now we're sure that the following holds:
    // offset_y_ <= current_line_ < offset_y_ + source_.get_height())
    got_data &= source_.get_next_row_data(cached_line_.data());

    auto format = get_format();
    auto x_src_width = source_.get_width() > offset_x_ ? source_.get_width() - offset_x_ : 0;
    x_src_width = std::min(x_src_width, width_);
    auto x_pad_after = width_ > x_src_width ? width_ - x_src_width : 0;

    if (get_pixel_format_depth(format) < 8) {
        // we need to copy pixels one-by-one as there's no per-bit addressing
        for (std::size_t i = 0; i < x_src_width; ++i) {
            auto pixel = get_raw_pixel_from_row(cached_line_.data(), i + offset_x_, format);
            set_raw_pixel_to_row(out_data, i, pixel, format);
        }
        for (std::size_t i = 0; i < x_pad_after; ++i) {
            set_raw_pixel_to_row(out_data, i + x_src_width, RawPixel{}, format);
        }
    } else {
        std::size_t pixel_bytes = get_pixel_row_bytes(format, 1);
        if (x_src_width > 0) {
            std::memcpy(out_data, cached_line_.data() + offset_x_ * pixel_bytes,
                        x_src_width * pixel_bytes);
        }
        if (x_pad_after > 0) {
            std::fill(out_data + x_src_width * pixel_bytes,
                      out_data + (x_src_width + x_pad_after) * pixel_bytes, 0);
        }
    }

    current_line_++;

    return got_data;
}

ImagePipelineNodeCalibrate::ImagePipelineNodeCalibrate(ImagePipelineNode& source,
                                                       const std::vector<std::uint16_t>& bottom,
                                                       const std::vector<std::uint16_t>& top,
                                                       std::size_t x_start) :
    source_(source)
{
    std::size_t size = 0;
    if (bottom.size() >= x_start && top.size() >= x_start) {
        size = std::min(bottom.size() - x_start, top.size() - x_start);
    }

    offset_.reserve(size);
    multiplier_.reserve(size);

    for (std::size_t i = 0; i < size; ++i) {
        offset_.push_back(bottom[i + x_start] / 65535.0f);
        multiplier_.push_back(65535.0f / (top[i + x_start] - bottom[i + x_start]));
    }
}

bool ImagePipelineNodeCalibrate::get_next_row_data(std::uint8_t* out_data)
{
    bool ret = source_.get_next_row_data(out_data);

    auto format = get_format();
    auto depth = get_pixel_format_depth(format);
    std::size_t max_value = 1;
    switch (depth) {
        case 8: max_value = 255; break;
        case 16: max_value = 65535; break;
        default:
            throw SaneException("Unsupported depth for calibration %d", depth);
    }
    unsigned channels = get_pixel_channels(format);

    std::size_t max_calib_i = offset_.size();
    std::size_t curr_calib_i = 0;

    for (std::size_t x = 0, width = get_width(); x < width && curr_calib_i < max_calib_i; ++x) {
        for (unsigned ch = 0; ch < channels && curr_calib_i < max_calib_i; ++ch) {
            std::int32_t value = get_raw_channel_from_row(out_data, x, ch, format);

            float value_f = static_cast<float>(value) / max_value;
            value_f = (value_f - offset_[curr_calib_i]) * multiplier_[curr_calib_i];
            value_f = std::round(value_f * max_value);
            value = clamp<std::int32_t>(static_cast<std::int32_t>(value_f), 0, max_value);
            set_raw_channel_to_row(out_data, x, ch, value, format);

            curr_calib_i++;
        }
    }
    return ret;
}

ImagePipelineNodeDebug::ImagePipelineNodeDebug(ImagePipelineNode& source,
                                               const std::string& path) :
    source_(source),
    path_{path},
    buffer_{source_.get_row_bytes()}
{}

ImagePipelineNodeDebug::~ImagePipelineNodeDebug()
{
    catch_all_exceptions(__func__, [&]()
    {
        if (buffer_.empty())
            return;

        auto format = get_format();
        buffer_.linearize();
        write_tiff_file(path_, buffer_.get_front_row_ptr(), get_pixel_format_depth(format),
                        get_pixel_channels(format), get_width(), buffer_.height());
    });
}

bool ImagePipelineNodeDebug::get_next_row_data(std::uint8_t* out_data)
{
    buffer_.push_back();
    bool got_data = source_.get_next_row_data(out_data);
    std::memcpy(buffer_.get_back_row_ptr(), out_data, get_row_bytes());
    return got_data;
}

std::size_t ImagePipelineStack::get_input_width() const
{
    ensure_node_exists();
    return nodes_.front()->get_width();
}

std::size_t ImagePipelineStack::get_input_height() const
{
    ensure_node_exists();
    return nodes_.front()->get_height();
}

PixelFormat ImagePipelineStack::get_input_format() const
{
    ensure_node_exists();
    return nodes_.front()->get_format();
}

std::size_t ImagePipelineStack::get_input_row_bytes() const
{
    ensure_node_exists();
    return nodes_.front()->get_row_bytes();
}

std::size_t ImagePipelineStack::get_output_width() const
{
    ensure_node_exists();
    return nodes_.back()->get_width();
}

std::size_t ImagePipelineStack::get_output_height() const
{
    ensure_node_exists();
    return nodes_.back()->get_height();
}

PixelFormat ImagePipelineStack::get_output_format() const
{
    ensure_node_exists();
    return nodes_.back()->get_format();
}

std::size_t ImagePipelineStack::get_output_row_bytes() const
{
    ensure_node_exists();
    return nodes_.back()->get_row_bytes();
}

void ImagePipelineStack::ensure_node_exists() const
{
    if (nodes_.empty()) {
        throw SaneException("The pipeline does not contain any nodes");
    }
}

void ImagePipelineStack::clear()
{
    // we need to destroy the nodes back to front, so that the destructors still have valid
    // references to sources
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        it->reset();
    }
    nodes_.clear();
}

std::vector<std::uint8_t> ImagePipelineStack::get_all_data()
{
    auto row_bytes = get_output_row_bytes();
    auto height = get_output_height();

    std::vector<std::uint8_t> ret;
    ret.resize(row_bytes * height);

    for (std::size_t i = 0; i < height; ++i) {
        get_next_row_data(ret.data() + row_bytes * i);
    }
    return ret;
}

Image ImagePipelineStack::get_image()
{
    auto height = get_output_height();

    Image ret;
    ret.resize(get_output_width(), height, get_output_format());

    for (std::size_t i = 0; i < height; ++i) {
        get_next_row_data(ret.get_row_ptr(i));
    }
    return ret;
}

} // namespace genesys
