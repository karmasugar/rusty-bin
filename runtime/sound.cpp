/* YM2608 (OPNA) of the PC-9801-86 board, emulated by ymfm. Times are emulated nanoseconds (pc98.c time base).
 * Before each chip access the output is generated up to that time, so register writes land at the right sample.
 * Output: 166.4 kHz from ymfm, averaged by 3 to 55.47 kHz stereo 16-bit, sent to the host (display.c) and, with
 * RUSTY_WAV=<file>, written as a WAV file. Timers A/B drive the board IRQ line (rising edges are counted, so an
 * edge inside an interrupt handler is not lost); SSG register 0Eh (port A input)
 * returns the IRQ setting in bits 6-7 (11 = INT5, IRQ 12) and no joystick buttons. The rhythm samples come from the
 * YM2608 ADPCM-A ROM (ym2608_adpcm_rom.bin in the ROM folders of roms.c, or RUSTY_RHYTHM_ROM; not included, silent
 * without it). ADPCM RAM is 256 KB. Without a listener (no SDL audio, no WAV) no samples
 * are generated: timers run separately and the game can read back only ADPCM state, so generation starts again at
 * the first ADPCM playback. */
#include "../third_party/ymfm/ymfm_opn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "roms.h"
#include "sound.h"
}

namespace {

constexpr uint32_t CHIP_CLOCK = 7987200;
constexpr int64_t NEVER = INT64_MAX;
constexpr int AVERAGE = 3;
constexpr const char *RHYTHM_ROM_NAME = "ym2608_adpcm_rom.bin";
// Mix levels matched to DOSBox-X (Neko Project II sound code): SSG against FM from the C-Lab logo (voice/jingle
// loudness 0.42), the output gain from the overall level.
constexpr int32_t SSG_GAIN_PERCENT = 18;
constexpr int32_t OUTPUT_GAIN_PERCENT = 160;

class Board : public ymfm::ymfm_interface {
public:
    Board() : chip(*this)
    {
        chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
        chip.reset();
        chip_rate = chip.sample_rate(CHIP_CLOCK);
        adpcm_ram.assign(0x40000, 0);
    }

    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
    {
        timer_end[tnum] = duration_in_clocks < 0 ? NEVER : now + (int64_t)duration_in_clocks * 1000000000LL / CHIP_CLOCK;
    }

    void ymfm_update_irq(bool asserted) override
    {
        if (asserted && !irq) irq_edges++;
        irq = asserted;
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type == ymfm::ACCESS_IO) return 0xFF;
        if (type == ymfm::ACCESS_ADPCM_B) return adpcm_ram[address & 0x3FFFF];
        if (type == ymfm::ACCESS_ADPCM_A) return rhythm_rom[address & 0x1FFF];
        return 0;
    }

    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B) adpcm_ram[address & 0x3FFFF] = data;
    }

    void run_timers(int64_t until)
    {
        for (;;) {
            int which = timer_end[0] <= timer_end[1] ? 0 : 1;
            if (timer_end[which] > until) return;
            render(timer_end[which]);
            now = timer_end[which];
            timer_end[which] = NEVER;
            expired[which]++;
            m_engine->engine_timer_expired(which);
        }
    }

    void render(int64_t until)
    {
        int64_t target = until * (int64_t)chip_rate / 1000000000LL;
        if (!wav && !audio_output && !adpcm_started) {
            generated = target;
            return;
        }
        if (target - generated > (int64_t)chip_rate) generated = target - chip_rate;
        while (generated < target) {
            ymfm::ym2608::output_data output;
            chip.generate(&output, 1);
            generated++;
            int32_t ssg = output.data[2] * SSG_GAIN_PERCENT / 100;
            sum_left += output.data[0] + ssg;
            sum_right += output.data[1] + ssg;
            if (++sum_count == AVERAGE) {
                emit(sum_left * OUTPUT_GAIN_PERCENT / (100 * AVERAGE), sum_right * OUTPUT_GAIN_PERCENT / (100 * AVERAGE));
                sum_left = sum_right = 0;
                sum_count = 0;
            }
        }
    }

    ymfm::ym2608 chip;
    uint32_t chip_rate = 0;
    int64_t now = 0;
    int64_t timer_end[2] = {NEVER, NEVER};
    bool irq = false;
    bool audio_output = false;
    bool adpcm_started = false;
    uint16_t address[2] = {0, 0};
    unsigned irq_edges = 0;
    unsigned long expired[2] = {0, 0};
    int64_t generated = 0;
    int32_t sum_left = 0, sum_right = 0;
    int sum_count = 0;
    std::vector<uint8_t> adpcm_ram;
    uint8_t rhythm_rom[0x2000] = {};
    std::vector<int16_t> pending;
    FILE *wav = nullptr;
    uint32_t wav_samples = 0;

private:
    static int16_t clip(int32_t value) { return (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value); }

    void emit(int32_t left, int32_t right)
    {
        pending.push_back(clip(left));
        pending.push_back(clip(right));
        if (pending.size() >= 2048) flush();
    }

public:
    void flush()
    {
        if (pending.empty()) return;
        display_audio(pending.data(), (int)(pending.size() / 2));
        if (wav) {
            fwrite(pending.data(), sizeof(int16_t), pending.size(), wav);
            wav_samples += (uint32_t)(pending.size() / 2);
        }
        pending.clear();
    }
};

Board *board;

void write_u32(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    fwrite(bytes, 1, 4, file);
}

void write_wav_header(FILE *file, uint32_t rate, uint32_t samples)
{
    fseek(file, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, file);
    write_u32(file, 36 + samples * 4);
    fwrite("WAVEfmt ", 1, 8, file);
    write_u32(file, 16);
    uint8_t format[4] = {1, 0, 2, 0};
    fwrite(format, 1, 4, file);
    write_u32(file, rate);
    write_u32(file, rate * 4);
    uint8_t align[4] = {4, 0, 16, 0};
    fwrite(align, 1, 4, file);
    fwrite("data", 1, 4, file);
    write_u32(file, samples * 4);
}

/* The YM2608 rhythm samples (ADPCM-A ROM, 8 KB, the name MAME uses): RUSTY_RHYTHM_ROM ("" = none) or the ROM folders
 * (roms.c). */
void load_rhythm_rom()
{
    char path[1024];
    int found = rom_find("RUSTY_RHYTHM_ROM", RHYTHM_ROM_NAME, nullptr, path, sizeof(path));
    if (found == ROM_DISABLED) return;
    if (found == ROM_MISSING) {
        fprintf(stderr,
                "[rt] sound: rhythm ROM %s (%zu bytes) not found in: %s. Put it in one of these folders or set "
                "RUSTY_RHYTHM_ROM=<file>. Without it the drums of the stage songs are silent\n",
                RHYTHM_ROM_NAME, sizeof(board->rhythm_rom), path);
        display_message("ym2608_adpcm_rom.bin (YM2608 rhythm ROM, 8192 bytes) not found: the drums are silent. Put "
                        "it next to the game image, or in roms/ next to the program (see README.txt).",
                        10);
        return;
    }
    FILE *file = fopen(path, "rb");
    size_t size = file ? fread(board->rhythm_rom, 1, sizeof(board->rhythm_rom), file) : 0;
    if (file && fgetc(file) != EOF) size++;
    if (file) fclose(file);
    if (size == sizeof(board->rhythm_rom)) {
        fprintf(stderr, "[rt] sound: rhythm ROM loaded (%s)\n", path);
        return;
    }
    memset(board->rhythm_rom, 0, sizeof(board->rhythm_rom));
    fprintf(stderr, "[rt] sound: rhythm ROM %s: %s, expected %zu bytes. Without it the drums are silent\n", path,
            file ? "wrong size" : "cannot read", sizeof(board->rhythm_rom));
    display_message(file ? "ym2608_adpcm_rom.bin has the wrong size (8192 bytes expected): the drums are silent."
                         : "ym2608_adpcm_rom.bin cannot be read: the drums are silent.",
                    10);
}

void report_timers()
{
    fprintf(stderr, "[opna] timer A expired %lu times, timer B %lu times, IRQ edges %u\n", board->expired[0],
            board->expired[1], board->irq_edges);
}

void close_wav()
{
    if (!board || !board->wav) return;
    board->flush();
    write_wav_header(board->wav, sound_output_rate(), board->wav_samples);
    fclose(board->wav);
    board->wav = nullptr;
}

}  // namespace

extern "C" {

void sound_init(void)
{
    board = new Board();
    load_rhythm_rom();
    if (getenv("RUSTY_TRACE_OPNA")) atexit(report_timers);
    const char *path = getenv("RUSTY_WAV");
    if (path) {
        board->wav = fopen(path, "wb");
        if (board->wav) {
            write_wav_header(board->wav, sound_output_rate(), 0);
            atexit(close_wav);
        }
    }
}

void sound_set_audio_output(int on)
{
    if (board) board->audio_output = on != 0;
}

unsigned sound_output_rate(void)
{
    return board ? board->chip_rate / AVERAGE : 55466;
}

void sound_write(int port, uint8_t value, int64_t now)
{
    board->run_timers(now);
    board->render(now);
    board->now = now;
    uint16_t *address = board->address;
    if (!(port & 1)) address[port >> 1] = value;
    static int trace = -1;
    if (trace < 0) trace = getenv("RUSTY_TRACE_OPNA") != nullptr;
    uint16_t reg = (uint16_t)((port >> 1) * 0x100 + address[port >> 1]);
    if ((port & 1) && reg == 0x100 && (value & 0x80) && !board->adpcm_started) {
        board->adpcm_started = true;
        fprintf(stderr, "[opna] ADPCM playback started: audio is generated from now on\n");
    }
    if (trace && (port & 1) && ((reg >= 0x24 && reg <= 0x26) || (reg >= 0x2d && reg <= 0x2f))) {
        fprintf(stderr, "[opna] %.3f s: %03x = %02x\n", now / 1e9, reg, value);
    }
    board->chip.write((uint32_t)port, value);
}

uint8_t sound_read(int port, int64_t now)
{
    board->run_timers(now);
    board->render(now);
    board->now = now;
    uint8_t value = board->chip.read((uint32_t)port);
    return value;
}

unsigned sound_update(int64_t now)
{
    board->run_timers(now);
    board->render(now);
    return board->irq_edges;
}

int64_t sound_next_event(void)
{
    if (!board) return NEVER;
    return board->timer_end[0] < board->timer_end[1] ? board->timer_end[0] : board->timer_end[1];
}

void sound_flush(void)
{
    if (board) board->flush();
}

/* Snapshot: the YM2608 (ymfm's own save_restore), the ADPCM RAM, the timers and the IRQ line of the board, and the
 * sample generation position. Samples not yet sent to the host are dropped on a load. */
void sound_snapshot(struct StateStream *s)
{
    uint8_t present = board != nullptr;
    state_io(s, &present, sizeof(present));
    if (!present || !board) return;
    std::vector<uint8_t> chip_state;
    uint32_t size = 0;
    if (!state_loading(s)) {
        ymfm::ymfm_saved_state saved(chip_state, true);
        board->chip.save_restore(saved);
        size = (uint32_t)chip_state.size();
    }
    state_io(s, &size, sizeof(size));
    chip_state.resize(size);
    state_io(s, chip_state.data(), size);
    if (state_loading(s)) {
        ymfm::ymfm_saved_state restored(chip_state, false);
        board->chip.save_restore(restored);
        board->pending.clear();
    }
    state_io(s, &board->now, sizeof(board->now));
    state_io(s, board->timer_end, sizeof(board->timer_end));
    state_io(s, &board->irq, sizeof(board->irq));
    state_io(s, &board->adpcm_started, sizeof(board->adpcm_started));
    state_io(s, board->address, sizeof(board->address));
    state_io(s, &board->irq_edges, sizeof(board->irq_edges));
    state_io(s, &board->generated, sizeof(board->generated));
    state_io(s, &board->sum_left, sizeof(board->sum_left));
    state_io(s, &board->sum_right, sizeof(board->sum_right));
    state_io(s, &board->sum_count, sizeof(board->sum_count));
    state_io(s, board->adpcm_ram.data(), board->adpcm_ram.size());
}

}
