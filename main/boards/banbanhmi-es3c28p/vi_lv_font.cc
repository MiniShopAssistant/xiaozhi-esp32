#include "vi_lv_font.h"

#include "fonts/font_vi.h"

#include <cstdint>

namespace {

constexpr uint8_t kOpa4Table[16] = {
    0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255,
};

int FindGlyphIndex(const vi_font_t* font, uint32_t codepoint) {
    int lo = 0;
    int hi = static_cast<int>(font->glyph_count) - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        uint32_t mid_cp = font->glyphs[mid].codepoint;
        if (mid_cp == codepoint) {
            return mid;
        }
        if (mid_cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

bool ViGetGlyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t letter,
                   uint32_t /*letter_next*/) {
    if (font == nullptr || font->dsc == nullptr || dsc_out == nullptr) {
        return false;
    }
    const auto* vi_font = static_cast<const vi_font_t*>(font->dsc);
    if (letter == '\t') {
        letter = ' ';
    }
    int idx = FindGlyphIndex(vi_font, letter);
    if (idx < 0) {
        return false;
    }

    const vi_glyph_t& g = vi_font->glyphs[idx];
    dsc_out->adv_w = g.advance;
    dsc_out->box_w = g.width;
    dsc_out->box_h = g.height;
    dsc_out->ofs_x = g.x_offset;
    // vi_glyph_t::y_offset is measured downward from the baseline (negative = above baseline).
    // LVGL places letter_coords.y1 = pos->y + (line_height - base_line) - box_h - ofs_y.
    dsc_out->ofs_y =
        static_cast<int16_t>(-static_cast<int16_t>(g.y_offset) - static_cast<int16_t>(g.height));
    dsc_out->stride = 0;
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A4;
    dsc_out->is_placeholder = 0;
    dsc_out->gid.index = static_cast<uint32_t>(idx);
    return true;
}

const void* ViGetGlyphBitmap(lv_font_glyph_dsc_t* g_dsc, lv_draw_buf_t* draw_buf) {
    if (g_dsc == nullptr || g_dsc->resolved_font == nullptr ||
        g_dsc->resolved_font->dsc == nullptr) {
        return nullptr;
    }
    const auto* vi_font = static_cast<const vi_font_t*>(g_dsc->resolved_font->dsc);
    if (g_dsc->gid.index >= vi_font->glyph_count) {
        return nullptr;
    }

    const vi_glyph_t& g = vi_font->glyphs[g_dsc->gid.index];
    if (g.width == 0 || g.height == 0) {
        return nullptr;
    }

    const uint8_t* src = &vi_font->bitmap[g.data_offset];
    if (g_dsc->req_raw_bitmap) {
        return src;
    }

    if (draw_buf == nullptr || draw_buf->data == nullptr) {
        return nullptr;
    }

    const uint32_t stride_out =
        draw_buf->header.stride != 0
            ? draw_buf->header.stride
            : lv_draw_buf_width_to_stride(g.width, LV_COLOR_FORMAT_A8);
    uint8_t* dst = draw_buf->data;

    // 4-bit nibbles packed tightly across rows (high nibble = even pixel, low nibble = odd pixel).
    uint32_t pix_idx = 0;
    for (uint32_t y = 0; y < g.height; ++y) {
        uint8_t* row = dst + y * stride_out;
        for (uint32_t x = 0; x < g.width; ++x, ++pix_idx) {
            uint8_t packed = src[pix_idx >> 1];
            uint8_t alpha4 = (pix_idx & 1u) ? (packed & 0x0Fu) : static_cast<uint8_t>(packed >> 4);
            row[x] = kOpa4Table[alpha4];
        }
    }

    lv_draw_buf_flush_cache(draw_buf, nullptr);
    return draw_buf;
}

lv_font_t MakeLvFont(const vi_font_t* vi_font) {
    lv_font_t font = {};
    font.get_glyph_dsc = ViGetGlyphDsc;
    font.get_glyph_bitmap = ViGetGlyphBitmap;
    font.release_glyph = nullptr;
    font.line_height = vi_font->line_height;
    font.base_line = vi_font->descent;
    font.subpx = LV_FONT_SUBPX_NONE;
    font.kerning = LV_FONT_KERNING_NONE;
    font.static_bitmap = 0;
    font.underline_position = 0;
    font.underline_thickness = 0;
    font.dsc = vi_font;
    font.fallback = nullptr;
    font.user_data = nullptr;
    return font;
}

}  // namespace

const lv_font_t kViFont38 = MakeLvFont(&font_vi_38);
const lv_font_t kViFont20 = MakeLvFont(&font_vi_20);
const lv_font_t kViFontDigits72 = MakeLvFont(&font_digits_72);

bool ViFont20HasGlyph(uint32_t codepoint) {
    return FindGlyphIndex(&font_vi_20, codepoint) >= 0;
}
