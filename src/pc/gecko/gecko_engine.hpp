/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_GECKO_ENGINE_HPP
#define PC_GECKO_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace pc::gecko {

struct GeckoLine {
    uint32_t address = 0;
    uint32_t value = 0;
};

struct GeckoCode {
    std::string name;
    std::vector<GeckoLine> lines;
};

class GeckoEngine {
public:
    static GeckoEngine& instance();

    // Reset simulated memory and clear loaded codes.
    void reset();

    // Parses raw Gecko code string (whitespace/newline separated hex pairs).
    std::vector<GeckoCode> parse_codes(
        const std::string& code_str, const std::string& default_name = "Custom Code");

    // Loads and executes a list of Gecko codes against simulated memory.
    void apply_codes(const std::vector<GeckoCode>& codes);

    // Apply raw code string directly.
    void apply_code_string(const std::string& code_str);

    // Memory access into simulated GameCube address space (0x80000000 - 0x817FFFFF)
    uint32_t read32(uint32_t gc_addr) const;
    uint16_t read16(uint32_t gc_addr) const;
    uint8_t read8(uint32_t gc_addr) const;

    void write32(uint32_t gc_addr, uint32_t value);
    void write16(uint32_t gc_addr, uint16_t value);
    void write8(uint32_t gc_addr, uint8_t value);

    // Check if an address was written to by Gecko codes
    bool is_modified(uint32_t gc_addr) const;

private:
    GeckoEngine();
    static constexpr size_t k_mem_size = 24 * 1024 * 1024;  // 24 MB GameCube 1T-SRAM
    static constexpr uint32_t k_mem_base = 0x80000000;
    std::vector<uint8_t> m_ram;
    std::vector<bool> m_modified_mask;
    std::vector<GeckoCode> m_applied_codes;

    bool is_valid_addr(uint32_t gc_addr, size_t len = 1) const;
};

}  // namespace pc::gecko

// C ABI helpers
#ifdef __cplusplus
extern "C" {
#endif

void pc_gecko_reset(void);
void pc_gecko_apply_string(const char* codes);
uint32_t pc_gecko_read32(uint32_t gc_addr);
void pc_gecko_write32(uint32_t gc_addr, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif  // PC_GECKO_ENGINE_HPP
