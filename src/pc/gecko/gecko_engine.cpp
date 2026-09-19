/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gecko_engine.hpp"

#include <cctype>
#include <sstream>

namespace pc::gecko {

GeckoEngine& GeckoEngine::instance() {
    static GeckoEngine s_instance;
    return s_instance;
}

GeckoEngine::GeckoEngine() {
    m_ram.resize(k_mem_size, 0);
    m_modified_mask.resize(k_mem_size, false);
}

void GeckoEngine::reset() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    std::fill(m_modified_mask.begin(), m_modified_mask.end(), false);
    m_applied_codes.clear();
}

bool GeckoEngine::is_valid_addr(uint32_t gc_addr, size_t len) const {
    if (gc_addr < k_mem_base)
        return false;
    uint32_t offset = gc_addr - k_mem_base;
    return (offset + len <= k_mem_size);
}

uint32_t GeckoEngine::read32(uint32_t gc_addr) const {
    if (!is_valid_addr(gc_addr, 4))
        return 0;
    uint32_t offset = gc_addr - k_mem_base;
    return (static_cast<uint32_t>(m_ram[offset]) << 24) |
           (static_cast<uint32_t>(m_ram[offset + 1]) << 16) |
           (static_cast<uint32_t>(m_ram[offset + 2]) << 8) |
           (static_cast<uint32_t>(m_ram[offset + 3]));
}

uint16_t GeckoEngine::read16(uint32_t gc_addr) const {
    if (!is_valid_addr(gc_addr, 2))
        return 0;
    uint32_t offset = gc_addr - k_mem_base;
    return (static_cast<uint16_t>(m_ram[offset]) << 8) | (static_cast<uint16_t>(m_ram[offset + 1]));
}

uint8_t GeckoEngine::read8(uint32_t gc_addr) const {
    if (!is_valid_addr(gc_addr, 1))
        return 0;
    uint32_t offset = gc_addr - k_mem_base;
    return m_ram[offset];
}

void GeckoEngine::write32(uint32_t gc_addr, uint32_t value) {
    if (!is_valid_addr(gc_addr, 4))
        return;
    uint32_t offset = gc_addr - k_mem_base;
    m_ram[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
    m_ram[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    m_ram[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    m_ram[offset + 3] = static_cast<uint8_t>(value & 0xFF);

    m_modified_mask[offset] = true;
    m_modified_mask[offset + 1] = true;
    m_modified_mask[offset + 2] = true;
    m_modified_mask[offset + 3] = true;
}

void GeckoEngine::write16(uint32_t gc_addr, uint16_t value) {
    if (!is_valid_addr(gc_addr, 2))
        return;
    uint32_t offset = gc_addr - k_mem_base;
    m_ram[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    m_ram[offset + 1] = static_cast<uint8_t>(value & 0xFF);

    m_modified_mask[offset] = true;
    m_modified_mask[offset + 1] = true;
}

void GeckoEngine::write8(uint32_t gc_addr, uint8_t value) {
    if (!is_valid_addr(gc_addr, 1))
        return;
    uint32_t offset = gc_addr - k_mem_base;
    m_ram[offset] = value;
    m_modified_mask[offset] = true;
}

bool GeckoEngine::is_modified(uint32_t gc_addr) const {
    if (!is_valid_addr(gc_addr, 1))
        return false;
    return m_modified_mask[gc_addr - k_mem_base];
}

std::vector<GeckoCode> GeckoEngine::parse_codes(
    const std::string& code_str, const std::string& default_name) {
    std::vector<GeckoCode> result;
    GeckoCode current_code;
    current_code.name = default_name;

    std::istringstream stream(code_str);
    std::string token1, token2;

    while (stream >> token1) {
        // If line is a comment or title (starts with '$' or '#')
        if (token1.front() == '$' || token1.front() == '#') {
            if (!current_code.lines.empty()) {
                result.push_back(std::move(current_code));
                current_code = GeckoCode();
            }
            std::string rest_of_line;
            std::getline(stream, rest_of_line);
            current_code.name = token1.substr(1) + rest_of_line;
            continue;
        }

        if (stream >> token2) {
            try {
                uint32_t w0 = static_cast<uint32_t>(std::stoul(token1, nullptr, 16));
                uint32_t w1 = static_cast<uint32_t>(std::stoul(token2, nullptr, 16));
                current_code.lines.push_back(GeckoLine{.address = w0, .value = w1});
            } catch (...) {
                // Ignore non-hex token pairs
            }
        }
    }

    if (!current_code.lines.empty()) {
        result.push_back(std::move(current_code));
    }

    return result;
}

void GeckoEngine::apply_codes(const std::vector<GeckoCode>& codes) {
    for (const auto& code : codes) {
        size_t i = 0;
        while (i < code.lines.size()) {
            const auto& line = code.lines[i++];
            uint8_t opcode = static_cast<uint8_t>((line.address >> 24) & 0xFE);
            uint32_t target_addr = (line.address & 0x01FFFFFF) | k_mem_base;

            switch (opcode) {
            case 0x04:  // 32-bit RAM write
                write32(target_addr, line.value);
                break;
            case 0x02:  // 16-bit RAM write
                write16(target_addr, static_cast<uint16_t>(line.value & 0xFFFF));
                break;
            case 0x00:  // 8-bit RAM write
                write8(target_addr, static_cast<uint8_t>(line.value & 0xFF));
                break;
            case 0xC2: {  // Insert assembly hook
                uint32_t num_lines = line.value;
                uint32_t hook_dest = target_addr;
                for (uint32_t l = 0; l < num_lines && i < code.lines.size(); ++l, ++i) {
                    write32(hook_dest, code.lines[i].address);
                    write32(hook_dest + 4, code.lines[i].value);
                    hook_dest += 8;
                }
                break;
            }
            default:
                // Unhandled opcode, advance
                break;
            }
        }
        m_applied_codes.push_back(code);
    }
}

void GeckoEngine::apply_code_string(const std::string& code_str) {
    auto parsed = parse_codes(code_str);
    apply_codes(parsed);
}

}  // namespace pc::gecko

extern "C" {

void pc_gecko_reset(void) {
    pc::gecko::GeckoEngine::instance().reset();
}

void pc_gecko_apply_string(const char* codes) {
    if (codes != nullptr) {
        pc::gecko::GeckoEngine::instance().apply_code_string(codes);
    }
}

uint32_t pc_gecko_read32(uint32_t gc_addr) {
    return pc::gecko::GeckoEngine::instance().read32(gc_addr);
}

void pc_gecko_write32(uint32_t gc_addr, uint32_t val) {
    pc::gecko::GeckoEngine::instance().write32(gc_addr, val);
}
}
