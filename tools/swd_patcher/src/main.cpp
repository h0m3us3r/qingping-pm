// ============================================================================
// ESP32 → HT32F52243 one-shot SWD patcher for Qingping PM sensor module
// ============================================================================
// Bit-bangs the SWD wire protocol from two ESP32 GPIOs, brings up the M0+ DP,
// halts the CPU, and uses the on-chip Flash Memory Controller (FMC) to flip
// two flash bytes that permanently switch the module into mode-1 / 9600 baud
// USART1 BM-frame output on PA4.
//
// All credit for the reverse-engineering goes in ../../docs/TECHNICAL.md.
// ============================================================================

#include <Arduino.h>
#include "soc/gpio_struct.h"
#include "gold_pages.h"   // embedded stock images of pages 0x2C00 and 0x4400

// ----- USER CONFIG ----------------------------------------------------------
static constexpr int  PIN_SWCLK       = 25;
static constexpr int  PIN_SWDIO       = 26;
static constexpr bool PATCH_REVERT    = false; // set true to undo the patch
// DIAGNOSE_ONLY: connect + halt + read & dump the three touched pages over
// serial, but do NOT erase or program. Use this to capture a pre/post image of
// flash and to confirm the SWD link is reliable before committing changes.
static constexpr bool DIAGNOSE_ONLY   = false;
// FORCE_RECOVERY: ignore the live snapshot and rewrite every touched page from
// the embedded gold image (then re-apply patches unless PATCH_REVERT). Use this
// to unbrick a module whose flash was corrupted by an earlier patch attempt.
static constexpr bool FORCE_RECOVERY  = true;
// ----------------------------------------------------------------------------

static constexpr uint32_t MASK_SWCLK = 1U << PIN_SWCLK;
static constexpr uint32_t MASK_SWDIO = 1U << PIN_SWDIO;

// Flash layout
static constexpr uint32_t FLASH_PAGE_SIZE = 0x400;          // 1 KiB
static constexpr uint32_t PATCH_CODE_ADDR = 0x00002E08;     // pointer slot
// Flash word at 0x2E08 is a little-endian RAM pointer. Stock bytes are
// B4 00 00 20 (= pointer to 0x200000B4, a dead BSS slot). Patched bytes are
// 38 00 00 20 (= pointer to 0x20000038, the real mode byte).
static constexpr uint32_t PATCH_CODE_OLD  = 0x200000B4;
static constexpr uint32_t PATCH_CODE_NEW  = 0x20000038;
static constexpr uint32_t PATCH_MODE_ADDR = 0x0000C400;     // mode byte
static constexpr uint32_t PATCH_MODE_OLD  = 0xFFFFFFFF;     // erased word
static constexpr uint32_t PATCH_MODE_NEW  = 0xFFFFFF01;     // 0x01 in LSB

// ---- Unfilter patches: turn the 7-window boxcar smoother into a pass-through.
// Both patches sit inside flash page 0x4400 (the running-sum loop in
// FUN_00004154). See ../../docs/TECHNICAL.md “Patch 2 — un-smoothing” for derivation.
//
//   0x4570 stock halfword = 0x8BDB  ldrh r3,[r3,#0x1e]   ; iter ← state[0x1e] (=0)
//   0x4570 new   halfword = 0x7B5B  ldrb r3,[r3,#0x0d]   ; iter ← state[0x0d] (=d)
//   0x45E8 stock halfword = 0x2B07  cmp  r3,#0x7         ; loop while iter < 7
//   0x45E8 new   halfword = 0x2B01  cmp  r3,#0x1         ; loop body runs once
//
// Combined effect: running_sum[bin] = history[d][bin] = the most recent
// 1-second window’s raw count, no smoothing.
static constexpr uint32_t PATCH_UNFILTER_ADDR_A = 0x00004570;
static constexpr uint32_t PATCH_UNFILTER_OLD_A  = 0xE02D8BDB;
static constexpr uint32_t PATCH_UNFILTER_NEW_A  = 0xE02D7B5B;
static constexpr uint32_t PATCH_UNFILTER_ADDR_B = 0x000045E8;
static constexpr uint32_t PATCH_UNFILTER_OLD_B  = 0x83EB2B07;
static constexpr uint32_t PATCH_UNFILTER_NEW_B  = 0x83EB2B01;
static constexpr bool     PATCH_UNFILTER        = true;     // false = keep firmware smoothing

// ---- Histogram-mode patch: force the BM emit path to build the raw 11-bin
// histogram frame (FUN_00004154 sub-block at 0x48F2) instead of falling into
// the µg/m³ pipeline that ends in FUN_00001c5c (which builds a Plantower
// PMSX003-format frame with hardcoded ×150 scaling on the >0.3 µm count).
// See ../../docs/TECHNICAL.md "Patch 4 — histogram mode".
//
//   Site: 0x460E   beq 0x470C   ; taken iff state[0x12]==0 (never, stock)
//                  bytes (LE half) = 0xD07D       ; cond = 0000 (EQ)
//   New : 0x460E   b   0x470C   ; unconditional
//                  bytes (LE half) = 0xE07D       ; b imm11
//
// The patch is a single-byte change (0xD0 -> 0xE0 at flash byte 0x460F).
// Word-aligned 4-byte patch at 0x460C covers it; the low halfword
// (cmp r0,#0; bytes 00 28) is unchanged on both sides.
static constexpr uint32_t PATCH_HISTMODE_ADDR   = 0x0000460C;
static constexpr uint32_t PATCH_HISTMODE_OLD    = 0xD07D2800;
static constexpr uint32_t PATCH_HISTMODE_NEW    = 0xE07D2800;
static constexpr bool     PATCH_HISTMODE        = true;     // false = keep Plantower frame

// HT32 FMC registers
static constexpr uint32_t FMC_BASE = 0x40080000;
static constexpr uint32_t FMC_TADR = FMC_BASE + 0x00;
static constexpr uint32_t FMC_WRDR = FMC_BASE + 0x04;
static constexpr uint32_t FMC_OCMR = FMC_BASE + 0x0C;
static constexpr uint32_t FMC_OPCR = FMC_BASE + 0x10;
static constexpr uint32_t FMC_OISR = FMC_BASE + 0x18;
static constexpr uint32_t FMC_CMD_WORD_PROG  = 0x4;
static constexpr uint32_t FMC_CMD_PAGE_ERASE = 0x8;

// ARM debug
static constexpr uint32_t DHCSR  = 0xE000EDF0;
static constexpr uint32_t DEMCR  = 0xE000EDFC;
static constexpr uint32_t AIRCR  = 0xE000ED0C;
static constexpr uint32_t DBGKEY = 0xA05F0000;

// ============================================================================
// SWD line bit-bang primitives.
// SWCLK runs at ~500 kHz with delayMicroseconds(1) holds; reliable on a
// breadboard and finishes the whole patch in well under a second.
// ============================================================================
static inline void clk_lo()  { GPIO.out_w1tc = MASK_SWCLK; }
static inline void clk_hi()  { GPIO.out_w1ts = MASK_SWCLK; }
static inline void dio_lo()  { GPIO.out_w1tc = MASK_SWDIO; }
static inline void dio_hi()  { GPIO.out_w1ts = MASK_SWDIO; }
static inline int  dio_rd()  { return (GPIO.in & MASK_SWDIO) ? 1 : 0; }
static inline void dio_out() { GPIO.enable_w1ts = MASK_SWDIO; }
static inline void dio_in()  { GPIO.enable_w1tc = MASK_SWDIO; }
static inline void hold()    { delayMicroseconds(1); }

static inline void swd_clock(int n) {
    for (int i = 0; i < n; i++) {
        clk_lo(); hold();
        clk_hi(); hold();
    }
}

// Host writes `n` LSB-first bits from `val`.
static void swd_write_bits(uint32_t val, int n) {
    for (int i = 0; i < n; i++) {
        clk_lo();
        if (val & 1) dio_hi(); else dio_lo();
        val >>= 1;
        hold();
        clk_hi();
        hold();
    }
}

// Host reads `n` LSB-first bits while target drives.
static uint32_t swd_read_bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        clk_lo(); hold();
        clk_hi();
        if (dio_rd()) v |= (1U << i);
        hold();
    }
    return v;
}

// One clock with neither side driving (turnaround).
static inline void swd_trn() {
    clk_lo(); hold();
    clk_hi(); hold();
}

// Even parity over the low 32 bits of v.
static inline int parity32(uint32_t v) {
    v ^= v >> 16;  v ^= v >> 8;  v ^= v >> 4;
    v ^= v >> 2;   v ^= v >> 1;
    return v & 1;
}

// ============================================================================
// SWD packet level.
// ============================================================================
enum : int { ACK_OK = 1, ACK_WAIT = 2, ACK_FAULT = 4, ACK_PROTOERR = 7 };

static uint8_t build_req(bool apndp, bool rnw, uint8_t a23) {
    uint8_t r = 0x81;                       // start | park
    if (apndp) r |= 0x02;
    if (rnw)   r |= 0x04;
    r |= (a23 & 0x3) << 3;
    int p = (apndp ? 1 : 0) ^ (rnw ? 1 : 0) ^ (a23 & 1) ^ ((a23 >> 1) & 1);
    if (p) r |= 0x20;
    return r;
}

static int swd_xfer_raw(bool apndp, bool rnw, uint8_t a23, uint32_t *data) {
    dio_out();
    swd_write_bits(build_req(apndp, rnw, a23), 8);
    dio_in();
    swd_trn();                              // trn before ACK
    uint32_t ack = swd_read_bits(3);

    if (ack == ACK_OK) {
        if (rnw) {
            uint32_t v = swd_read_bits(32);
            int par   = swd_read_bits(1);
            swd_trn();
            dio_out();
            if (par != parity32(v)) return ACK_PROTOERR;
            *data = v;
        } else {
            swd_trn();                      // trn back to host
            dio_out();
            uint32_t v = *data;
            swd_write_bits(v, 32);
            swd_write_bits(parity32(v), 1);
        }
    } else {
        swd_trn();
        dio_out();
    }
    // Idle bits — many DPs require some clocks between transactions.
    swd_write_bits(0, 8);
    return (int)ack;
}

static int swd_xfer(bool apndp, bool rnw, uint8_t a23, uint32_t *data) {
    for (int retry = 0; retry < 64; retry++) {
        int ack = swd_xfer_raw(apndp, rnw, a23, data);
        if (ack != ACK_WAIT) return ack;
    }
    return ACK_WAIT;
}

// ============================================================================
// SWD reset / connect.
// ============================================================================
static void swd_line_reset() {
    dio_out(); dio_hi();
    swd_clock(64);                          // ≥50 high cycles with SWDIO=1
}

static void swd_jtag_to_swd() {
    // 16-bit magic 0xE79E, LSB first.
    dio_out();
    uint16_t m = 0xE79E;
    for (int i = 0; i < 16; i++) {
        clk_lo();
        if (m & 1) dio_hi(); else dio_lo();
        m >>= 1;
        hold();
        clk_hi();
        hold();
    }
}

// ============================================================================
// Debug Port / Access Port helpers.
// ============================================================================
static uint32_t cached_select = 0xFFFFFFFF;

static int dp_read(uint8_t a23, uint32_t *v)  { return swd_xfer(false, true,  a23, v); }
static int dp_write(uint8_t a23, uint32_t v)  { return swd_xfer(false, false, a23, &v); }
static int ap_read_raw(uint8_t a23, uint32_t *v)  { return swd_xfer(true,  true,  a23, v); }
static int ap_write_raw(uint8_t a23, uint32_t v)  { return swd_xfer(true,  false, a23, &v); }

static int dp_select(uint32_t sel) {
    if (sel == cached_select) return ACK_OK;
    int a = dp_write(0x2 /* SELECT */, sel);
    if (a == ACK_OK) cached_select = sel;
    return a;
}

// AP read is posted: first read returns previous AP transaction's data; the
// fresh value comes from a follow-up DP read of RDBUFF (a23=0x3).
static int ap_read(uint8_t bank, uint8_t a23, uint32_t *v) {
    int a = dp_select(((uint32_t)bank << 4) | 0 /* AP=0 */);
    if (a != ACK_OK) return a;
    uint32_t dummy;
    a = ap_read_raw(a23, &dummy);           // discarded
    if (a != ACK_OK) return a;
    return dp_read(0x3 /* RDBUFF */, v);
}

static int ap_write(uint8_t bank, uint8_t a23, uint32_t v) {
    int a = dp_select(((uint32_t)bank << 4) | 0);
    if (a != ACK_OK) return a;
    return ap_write_raw(a23, v);
}

// ============================================================================
// AHB-AP memory access (32-bit, no auto-increment for simplicity).
// ============================================================================
static int mem_init() {
    // CSW: 32-bit transfer, master debug, privileged data access.
    return ap_write(0, 0x0 /* CSW */, 0x23000052);
}

static int mem_read32(uint32_t addr, uint32_t *out) {
    int a = ap_write(0, 0x1 /* TAR */, addr); if (a != ACK_OK) return a;
    return ap_read(0, 0x3 /* DRW */, out);
}

static int mem_write32(uint32_t addr, uint32_t val) {
    int a = ap_write(0, 0x1 /* TAR */, addr); if (a != ACK_OK) return a;
    return ap_write(0, 0x3 /* DRW */, val);
}

// ============================================================================
// HT32 FMC operations (run from SWD with CPU halted).
//
// Replays exactly what FUN_00002348 / FUN_00002304 / FUN_00002318 in the
// firmware itself do: write TADR (+ WRDR for prog), set OCMR, then OR bit 1
// of OPCR, then write 0x14 to OPCR, then poll bits[3:2] of OPCR for state 3.
// ============================================================================
// HT32 FMC OPCR[3:2] codes (per V1.0.03 firmware's polling loop):
//   00b = idle / no recent operation
//   01b = operation in progress
//   10b = error  (write protect, target outside flash, etc.)
//   11b = success
// OISR has a write-protect-error bit and an operation-finished bit; we clear
// them defensively before every op so a stale flag from a previous run
// doesn't poison the next poll.
static void fmc_clear_status() {
    uint32_t oisr;
    if (mem_read32(FMC_OISR, &oisr) == ACK_OK) {
        // Write-1-to-clear on most HT32 status bits; mirror the firmware
        // which masks bit1. Clear everything we read except reserved high
        // bits, which is the conservative thing to do.
        mem_write32(FMC_OISR, oisr & 0xFFU);
    }
}

// Returns: 1 = success, 0 = hardware error, -1 = poll timeout.
static int fmc_wait_done_ex() {
    for (int i = 0; i < 200000; i++) {
        uint32_t opcr;
        if (mem_read32(FMC_OPCR, &opcr) != ACK_OK) return -1;
        uint32_t st = (opcr >> 2) & 0x3;
        if (st == 0x3) {
            fmc_clear_status();
            return 1;
        }
        if (st == 0x2) {
            uint32_t oisr = 0;
            mem_read32(FMC_OISR, &oisr);
            Serial.printf("[FMC err OPCR=0x%08lX OISR=0x%08lX] ",
                          (unsigned long)opcr, (unsigned long)oisr);
            fmc_clear_status();
            return 0;
        }
    }
    return -1;
}

static bool fmc_wait_done() { return fmc_wait_done_ex() == 1; }

static bool fmc_trigger(uint32_t cmd) {
    uint32_t opcr;
    fmc_clear_status();
    if (mem_write32(FMC_OCMR, cmd)        != ACK_OK) return false;
    if (mem_read32 (FMC_OPCR, &opcr)      != ACK_OK) return false;
    if (mem_write32(FMC_OPCR, opcr | 0x2) != ACK_OK) return false;
    if (mem_write32(FMC_OPCR, 0x14)       != ACK_OK) return false;
    return fmc_wait_done();
}

static bool fmc_erase_page(uint32_t addr) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (mem_write32(FMC_TADR, addr) != ACK_OK) continue;
        if (fmc_trigger(FMC_CMD_PAGE_ERASE)) return true;
        Serial.printf("[erase retry %d @0x%08lX] ", attempt + 1, (unsigned long)addr);
        delay(2);
    }
    return false;
}

// Program one word, but only if the post-erase cell isn't already at the
// target value (avoids a redundant FMC op that some HT32 silicon flags as a
// program-over-non-FF error). Retries up to 3 times on transient FMC errors
// or SWD poll timeouts, and treats "cell already reads correct" as success.
static bool fmc_program_word(uint32_t addr, uint32_t data) {
    uint32_t cur;
    if (mem_read32(addr, &cur) == ACK_OK && cur == data) return true;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (mem_write32(FMC_TADR, addr) != ACK_OK) continue;
        if (mem_write32(FMC_WRDR, data) != ACK_OK) continue;
        fmc_clear_status();
        uint32_t opcr;
        if (mem_write32(FMC_OCMR, FMC_CMD_WORD_PROG) != ACK_OK) continue;
        if (mem_read32 (FMC_OPCR, &opcr)             != ACK_OK) continue;
        if (mem_write32(FMC_OPCR, opcr | 0x2)        != ACK_OK) continue;
        if (mem_write32(FMC_OPCR, 0x14)              != ACK_OK) continue;
        int rc = fmc_wait_done_ex();
        // Read-back confirms success regardless of OPCR status, because
        // verify is what we actually care about.
        uint32_t v = 0xDEADBEEFU;
        if (mem_read32(addr, &v) == ACK_OK && v == data) return true;
        Serial.printf("[prog retry %d @0x%08lX rc=%d got=0x%08lX want=0x%08lX] ",
                      attempt + 1, (unsigned long)addr, rc,
                      (unsigned long)v, (unsigned long)data);
        delay(2);
    }
    return false;
}

// ============================================================================
// Connect / halt / reset.
// ============================================================================
static bool dp_connect(uint32_t *idcode_out) {
    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();
    // Send at least two idle bits before first transaction.
    dio_out(); dio_lo(); swd_clock(8);
    return dp_read(0x0 /* DPIDR */, idcode_out) == ACK_OK;
}

static bool dp_powerup() {
    // Clear sticky errors.
    if (dp_write(0x0 /* ABORT */, 0x1E) != ACK_OK) return false;
    // Request system + debug power.
    if (dp_write(0x1 /* CTRL/STAT */, (1U<<30) | (1U<<28)) != ACK_OK) return false;
    for (int i = 0; i < 100; i++) {
        uint32_t s;
        if (dp_read(0x1, &s) != ACK_OK) return false;
        if ((s & ((1U<<31) | (1U<<29))) == ((1U<<31) | (1U<<29))) return true;
        delay(1);
    }
    return false;
}

static bool core_halt() {
    if (mem_write32(DEMCR, 0x01000000) != ACK_OK) return false;   // TRCENA
    if (mem_write32(DHCSR, DBGKEY | 0x3 /* C_DEBUGEN | C_HALT */) != ACK_OK) return false;
    for (int i = 0; i < 100; i++) {
        uint32_t d;
        if (mem_read32(DHCSR, &d) != ACK_OK) return false;
        if (d & (1U << 17) /* S_HALT */) return true;
        delay(1);
    }
    return false;
}

static bool core_reset_run() {
    // Clear C_HALT so the core runs after reset.
    mem_write32(DHCSR, DBGKEY | 0x1 /* C_DEBUGEN */);
    mem_write32(DEMCR, 0);
    // System reset request via SCB AIRCR.
    return mem_write32(AIRCR, 0x05FA0004) == ACK_OK;
}

// ============================================================================
// Patch orchestration.
// ============================================================================
struct WordPatch {
    uint32_t off;
    uint32_t want_old;
    uint32_t want_new;
};

// Static page buffers — keep off the stack (ESP32 Arduino default task is 8KB).
static uint32_t g_snapshot[FLASH_PAGE_SIZE / 4];
static uint32_t g_stock   [FLASH_PAGE_SIZE / 4];
static uint32_t g_target  [FLASH_PAGE_SIZE / 4];
static uint32_t g_readback[FLASH_PAGE_SIZE / 4];
static uint32_t g_scratch [FLASH_PAGE_SIZE / 4];

static void dump_page_hex(const char *label, uint32_t base, const uint32_t *buf) {
    Serial.printf("--- %s @ 0x%08lX ---\n", label, (unsigned long)base);
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i += 8) {
        Serial.printf("  %04lX:", (unsigned long)(i * 4));
        for (uint32_t j = 0; j < 8 && (i + j) < FLASH_PAGE_SIZE / 4; j++) {
            Serial.printf(" %08lX", (unsigned long)buf[i + j]);
        }
        Serial.println();
    }
}

static uint32_t page_crc32(const uint32_t *buf) {
    // Cheap CRC32 (poly 0xEDB88320) — used as a fingerprint, not for security.
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(buf);
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & -(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

// Read a full page into `out`. Re-reads any word that initially fails up to
// 3 times. Returns false if a word permanently fails to read.
static bool read_page_once(uint32_t page_base, uint32_t *out) {
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        int tries = 0;
        while (mem_read32(page_base + i * 4, &out[i]) != ACK_OK) {
            if (++tries >= 3) {
                Serial.printf("  read FAIL at +0x%lX\n", (unsigned long)(i * 4));
                return false;
            }
            delayMicroseconds(50);
        }
    }
    return true;
}

// Snapshot: read the page twice, ensure both reads agree. Defends against a
// single bit-bang glitch that would otherwise be silently written back during
// the program phase, corrupting an untouched word.
static bool snapshot_page(uint32_t page_base, uint32_t *snapshot) {
    if (!read_page_once(page_base, snapshot)) return false;
    if (!read_page_once(page_base, g_scratch)) return false;
    uint32_t diffs = 0;
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        if (snapshot[i] != g_scratch[i]) {
            if (diffs < 4) {
                Serial.printf("  snapshot mismatch +0x%lX: %08lX vs %08lX\n",
                              (unsigned long)(i * 4),
                              (unsigned long)snapshot[i],
                              (unsigned long)g_scratch[i]);
            }
            diffs++;
        }
    }
    if (diffs) {
        Serial.printf("  snapshot UNSTABLE (%lu mismatches) — refusing\n",
                      (unsigned long)diffs);
        return false;
    }
    return true;
}

// Program every non-erased word from `source` into `page_base`. Caller must
// have already erased the page. Returns count of words that ultimately failed.
static uint32_t program_page_from(uint32_t page_base, const uint32_t *source) {
    uint32_t fails = 0;
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        if (source[i] == 0xFFFFFFFFU) continue;
        if (!fmc_program_word(page_base + i * 4, source[i])) {
            fails++;
            Serial.printf("[!w+0x%lX] ", (unsigned long)(i * 4));
        }
    }
    return fails;
}

// Read-back + verify entire page against `expected`. Returns count of words
// that don't match. Diffs are logged (first few only).
static uint32_t verify_page(uint32_t page_base, const uint32_t *expected) {
    if (!read_page_once(page_base, g_readback)) return FLASH_PAGE_SIZE / 4;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        if (g_readback[i] != expected[i]) {
            if (bad < 6) {
                Serial.printf("  diff +0x%lX got=%08lX want=%08lX\n",
                              (unsigned long)(i * 4),
                              (unsigned long)g_readback[i],
                              (unsigned long)expected[i]);
            }
            bad++;
        }
    }
    return bad;
}

// Write `desired` to the page, with snapshot-based recovery if anything goes
// wrong. Returns true on success, false on an unrecoverable failure (the
// caller should not continue patching other pages).
static bool program_page_with_recovery(uint32_t page_base,
                                       const uint32_t *snapshot,
                                       const uint32_t *desired,
                                       const char *what)
{
    Serial.printf("Erasing page 0x%08lX ... ", (unsigned long)page_base);
    if (!fmc_erase_page(page_base)) { Serial.println("FAIL"); goto recover; }
    Serial.println("ok");

    Serial.printf("Programming %s ... ", what);
    {
        uint32_t fails = program_page_from(page_base, desired);
        Serial.println(fails == 0 ? "ok" : "with errors");
    }

    Serial.print("Verifying ... ");
    {
        uint32_t bad = verify_page(page_base, desired);
        if (bad == 0) { Serial.println("ok"); return true; }
        Serial.printf("FAIL (%lu mismatches)\n", (unsigned long)bad);
    }

recover:
    Serial.println("*** Attempting page recovery from gold image ***");
    Serial.print("  re-erasing ... ");
    if (!fmc_erase_page(page_base)) {
        Serial.println("FAIL — page may be left blank/corrupt");
        return false;
    }
    Serial.println("ok");
    Serial.print("  reprogramming from gold ... ");
    {
        uint32_t fails = program_page_from(page_base, snapshot);
        Serial.println(fails == 0 ? "ok" : "with errors");
    }
    Serial.print("  verifying recovery ... ");
    {
        uint32_t bad = verify_page(page_base, snapshot);
        if (bad == 0) {
            Serial.println("ok — page restored to gold stock");
        } else {
            Serial.printf("FAIL (%lu mismatches) — MODULE MAY BE BRICKED\n",
                          (unsigned long)bad);
        }
    }
    return false;
}

static bool rewrite_page_with_patches(uint32_t page_base,
                                      const WordPatch *patches, size_t n,
                                      const uint32_t *gold /* may be nullptr */)
{
    Serial.printf("\n[page 0x%08lX] snapshotting ... ", (unsigned long)page_base);
    if (!snapshot_page(page_base, g_snapshot)) return false;
    uint32_t snap_crc = page_crc32(g_snapshot);
    Serial.printf("ok crc=%08lX\n", (unsigned long)snap_crc);

    // Build the expected stock image. If we have a gold reference, use it;
    // otherwise (e.g. the all-FF mode page) synthesize an all-erased page.
    if (gold) {
        memcpy_P(g_stock, gold, FLASH_PAGE_SIZE);
    } else {
        memset(g_stock, 0xFF, FLASH_PAGE_SIZE);
    }
    uint32_t stock_crc = page_crc32(g_stock);

    // Build the desired post-patch image from the stock reference + patches.
    memcpy(g_target, g_stock, FLASH_PAGE_SIZE);
    for (size_t k = 0; k < n; k++) {
        uint32_t want = PATCH_REVERT ? patches[k].want_old : patches[k].want_new;
        uint32_t have = PATCH_REVERT ? patches[k].want_new : patches[k].want_old;
        // Sanity: assert the gold image at this offset actually holds the
        // pre-patch value our constants claim. Catches a bad gold_pages.h.
        if (g_stock[patches[k].off / 4] != have) {
            Serial.printf("  ERROR: gold[+0x%lX]=%08lX but want_pre=%08lX — "
                          "gold_pages.h is out of sync with patch constants\n",
                          (unsigned long)patches[k].off,
                          (unsigned long)g_stock[patches[k].off / 4],
                          (unsigned long)have);
            return false;
        }
        g_target[patches[k].off / 4] = want;
    }
    uint32_t target_crc = page_crc32(g_target);
    Serial.printf("  stock crc=%08lX  target crc=%08lX\n",
                  (unsigned long)stock_crc, (unsigned long)target_crc);

    // Classify the live snapshot.
    bool snap_is_stock  = (snap_crc == stock_crc);
    bool snap_is_target = (snap_crc == target_crc);

    // Word-level diff vs. stock for visibility (first 8).
    uint32_t diffs_vs_stock = 0;
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) {
        if (g_snapshot[i] != g_stock[i]) {
            if (diffs_vs_stock < 8) {
                Serial.printf("  snap≠stock  +0x%lX: live=%08lX gold=%08lX\n",
                              (unsigned long)(i * 4),
                              (unsigned long)g_snapshot[i],
                              (unsigned long)g_stock[i]);
            }
            diffs_vs_stock++;
        }
    }

    if (snap_is_stock) {
        Serial.println("  status: live page matches GOLD STOCK");
    } else if (snap_is_target) {
        Serial.printf("  status: live page already at target state (%lu word(s) diff vs stock — by patch)\n",
                      (unsigned long)diffs_vs_stock);
    } else {
        Serial.printf("  status: live page is UNEXPECTED — %lu word(s) diff vs gold stock\n",
                      (unsigned long)diffs_vs_stock);
    }

    bool need_write = !snap_is_target;
    bool force = FORCE_RECOVERY;
    if (force) {
        Serial.println("  FORCE_RECOVERY=true — rewriting page from gold image");
        need_write = true;
    } else if (!snap_is_stock && !snap_is_target) {
        // Live page is neither stock nor the expected post-patch state. Refuse
        // to write unless the user explicitly opts into recovery.
        Serial.println("  REFUSING to write: live page does not match gold stock "
                       "or target. Re-run with FORCE_RECOVERY=true to rewrite "
                       "from the embedded gold image.");
        return false;
    }

    if (!need_write) {
        Serial.println("  page already at target state, skipping erase/program");
        return true;
    }

    if (DIAGNOSE_ONLY) {
        Serial.println("  DIAGNOSE_ONLY=true — NOT writing.");
        dump_page_hex("snapshot", page_base, g_snapshot);
        return true;
    }

    // Recovery source is ALWAYS the gold image (or all-FF), never the
    // possibly-corrupt live snapshot.
    return program_page_with_recovery(page_base, g_stock, g_target, "patch");
}


static bool rewrite_page_with_patch(uint32_t page_base, uint32_t patch_off,
                                    uint32_t want_old, uint32_t want_new,
                                    const uint32_t *gold)
{
    WordPatch one = { patch_off, want_old, want_new };
    return rewrite_page_with_patches(page_base, &one, 1, gold);
}

static bool run_patch() {
    Serial.println("\n=== Qingping PM SWD patcher ===");

    pinMode(PIN_SWCLK, OUTPUT); digitalWrite(PIN_SWCLK, HIGH);
    pinMode(PIN_SWDIO, OUTPUT); digitalWrite(PIN_SWDIO, HIGH);
    delay(10);

    uint32_t idcode;
    Serial.print("SWD connect ... ");
    if (!dp_connect(&idcode)) { Serial.println("no response"); return false; }
    Serial.printf("DPIDR = 0x%08lX\n", (unsigned long)idcode);
    if (idcode == 0 || idcode == 0xFFFFFFFFU) {
        Serial.println("  garbage IDCODE — check wiring / GND");
        return false;
    }

    Serial.print("DP power-up ... ");
    if (!dp_powerup()) { Serial.println("FAIL"); return false; }
    Serial.println("ok");

    Serial.print("AHB-AP init ... ");
    if (mem_init() != ACK_OK) { Serial.println("FAIL"); return false; }
    Serial.println("ok");

    Serial.print("Halt CPU ... ");
    if (!core_halt()) { Serial.println("FAIL"); return false; }
    Serial.println("ok");

    // Patch decision matrix: forward = stock→patched; revert = patched→stock.
    uint32_t code_old = PATCH_REVERT ? PATCH_CODE_NEW : PATCH_CODE_OLD;
    uint32_t code_new = PATCH_REVERT ? PATCH_CODE_OLD : PATCH_CODE_NEW;
    uint32_t mode_old = PATCH_REVERT ? PATCH_MODE_NEW : PATCH_MODE_OLD;
    uint32_t mode_new = PATCH_REVERT ? PATCH_MODE_OLD : PATCH_MODE_NEW;

    uint32_t code_page = PATCH_CODE_ADDR & ~(FLASH_PAGE_SIZE - 1);
    uint32_t mode_page = PATCH_MODE_ADDR & ~(FLASH_PAGE_SIZE - 1);
    uint32_t code_off  = PATCH_CODE_ADDR -  code_page;
    uint32_t mode_off  = PATCH_MODE_ADDR -  mode_page;

    Serial.println("\n--- Patch 1: code-pointer redirect ---");
    if (!rewrite_page_with_patch(code_page, code_off, code_old, code_new, GOLD_PAGE_2C00))
        return false;

    if (PATCH_UNFILTER || PATCH_HISTMODE) {
        // Both unfilter sites and the histmode site live in page 0x4400.
        uint32_t un_page = 0x4400;
        if ((PATCH_UNFILTER_ADDR_A   & ~(FLASH_PAGE_SIZE - 1)) != un_page ||
            (PATCH_UNFILTER_ADDR_B   & ~(FLASH_PAGE_SIZE - 1)) != un_page ||
            (PATCH_HISTMODE_ADDR     & ~(FLASH_PAGE_SIZE - 1)) != un_page) {
            Serial.println("page-0x4400 patches misaligned — refusing");
            return false;
        }
        WordPatch patches[3];
        size_t n = 0;
        if (PATCH_UNFILTER) {
            patches[n++] = { PATCH_UNFILTER_ADDR_A - un_page,
                             PATCH_REVERT ? PATCH_UNFILTER_NEW_A : PATCH_UNFILTER_OLD_A,
                             PATCH_REVERT ? PATCH_UNFILTER_OLD_A : PATCH_UNFILTER_NEW_A };
            patches[n++] = { PATCH_UNFILTER_ADDR_B - un_page,
                             PATCH_REVERT ? PATCH_UNFILTER_NEW_B : PATCH_UNFILTER_OLD_B,
                             PATCH_REVERT ? PATCH_UNFILTER_OLD_B : PATCH_UNFILTER_NEW_B };
        }
        if (PATCH_HISTMODE) {
            patches[n++] = { PATCH_HISTMODE_ADDR - un_page,
                             PATCH_REVERT ? PATCH_HISTMODE_NEW : PATCH_HISTMODE_OLD,
                             PATCH_REVERT ? PATCH_HISTMODE_OLD : PATCH_HISTMODE_NEW };
        }
        Serial.println("\n--- Patch 2/4: page 0x4400 (unfilter + histmode) ---");
        if (!rewrite_page_with_patches(un_page, patches, n, GOLD_PAGE_4400))
            return false;
    }

    Serial.println("\n--- Patch 3: persistent mode byte ---");
    // Mode page is all-FF in stock — no embedded reference needed.
    if (!rewrite_page_with_patch(mode_page, mode_off, mode_old, mode_new, nullptr))
        return false;

    Serial.println("\nResetting target ...");
    core_reset_run();

    Serial.println("\nPATCH OK — module is now in mode 1");
    Serial.println("Disconnect SWD pogos; wire PA4 to your ESP32 UART RX at "
                   "9600-N-1 and you should start seeing BM frames.");
    return true;
}

// ============================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) ;
    delay(500);
    if (!run_patch()) {
        Serial.println("\n*** PATCH FAILED ***");
        Serial.println("Power-cycle the module, double-check SWCLK/SWDIO/GND, "
                       "press the ESP32 reset button to retry.");
    }
}

void loop() { delay(1000); }
