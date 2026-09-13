/* Unit regression for the real mixer; no SDL device or game window.
 * Run with python3 tools/test_audio_stream.py. */
#include <assert.h>
#include "../src/pc/audio.c"

int OSDisableInterrupts(void) { return 1; }
int OSRestoreInterrupts(int enabled) { return enabled; }
static u8 memory[PC_ARAM_SIZE];

static Voice voice(u16 format, u32 current, u32 end, u32 loop)
{
    Voice v = { 0 };
    v.vpb.pb.state = 1;
    v.vpb.pb.addr.format = format;
    v.vpb.pb.addr.loopFlag = loop != 0;
    v.vpb.pb.src.ratioHi = 1;
    v.cur_addr = current;
    v.end_addr = end;
    v.loop_addr = loop;
    return v;
}

static void inclusive_end(u16 format)
{
    Voice v = voice(format, 2, 3, 0);
    s16 sample;
    memory[1] = 0x27; /* ADPCM samples 2 and 7 */
    memory[2] = 2;
    memory[3] = 7;
    memory[4] = 0;
    memory[5] = 2;
    memory[6] = 0;
    memory[7] = 7;
    assert(next_sample(&v, &sample));
    assert(next_sample(&v, &sample)); /* address 3 is a sample, not a sentinel */
    assert(sample == (format == AX_FORMAT_PCM8 ? 7 * 256 : 7));
    assert(!next_sample(&v, &sample));
    assert(v.vpb.pb.state == 0);
}

static void ring_transition(u32 from, u32 to)
{
    /* HPS blocks have 64 KiB spacing, with nibble-addressed ADPCM.
     * Decode across a boundary, leaving the old end address in place until
     * the next 5 ms synth callback. The new block must advance normally. */
    u32 old_end = from + 15;
    Voice v = voice(AX_FORMAT_ADPCM, old_end, old_end, to + 2);
    float output[AX_FRAME * 2] = { 0 };
    memory[old_end / 2] = 0x07;
    for (u32 i = to / 2; i < to / 2 + 256; i += 8) {
        memory[i] = 0;
        memset(memory + i + 1, 0x33, 7);
    }
    mix_voice(&v, output);
    assert(v.vpb.pb.state == 1);
    assert(v.cur_addr > to + 100 && v.cur_addr < to + 256);
    assert(addr32(v.vpb.pb.addr.currentAddressHi,
                  v.vpb.pb.addr.currentAddressLo) == v.cur_addr);
    AXSetVoiceEndAddr(&v.vpb, to + 1023);
    mix_voice(&v, output);
    assert(v.vpb.pb.state == 1);
    assert(v.cur_addr > to + 256);
}

static void loop_history(void)
{
    Voice v = voice(AX_FORMAT_ADPCM, 15, 15, 0x20002);
    s16 sample;
    memory[7] = 7;
    v.vpb.pb.adpcmLoop.loop_pred_scale = 0x13;
    v.vpb.pb.adpcmLoop.loop_yn1 = (u16) -123;
    v.vpb.pb.adpcmLoop.loop_yn2 = 456;
    assert(next_sample(&v, &sample));
    assert(sample == 7);
    assert(v.cur_addr == 0x20002);
    assert(v.pred_scale == 0x13);
    assert(v.yn1 == -123 && v.yn2 == 456);
}

static void negative_samples(void)
{
    s16 sample;
    Voice pcm = voice(AX_FORMAT_PCM8, 2, 3, 0);
    memory[2] = 0x80;
    memory[3] = 0xff;
    assert(next_sample(&pcm, &sample) && sample == -32768);
    assert(next_sample(&pcm, &sample) && sample == -256);

    Voice adpcm = voice(AX_FORMAT_ADPCM, 2, 3, 0);
    memory[1] = 0x8f;
    assert(next_sample(&adpcm, &sample) && sample == -8);
    assert(next_sample(&adpcm, &sample) && sample == -1);
}

int main(void)
{
    s_aram = memory;
    ring_transition(0x20000, 0x40000);
    inclusive_end(AX_FORMAT_PCM8);
    inclusive_end(AX_FORMAT_PCM16);
    inclusive_end(AX_FORMAT_ADPCM);
    ring_transition(0x40000, 0);
    loop_history();
    negative_samples();
    puts("PASS: inclusive ends, HPS ring transitions, loop history and signed samples");
}
