/* Unit regression for the real mixer; no SDL device or game window.
 * Run with python3 tools/test_audio_stream.py. */
#include <assert.h>
#include "../src/pc/audio.c"

int OSDisableInterrupts(void) {
    return 1;
}
int OSRestoreInterrupts(int enabled) {
    return enabled;
}
static u8 memory[PC_ARAM_SIZE];

static Voice voice(u16 format, u32 current, u32 end, u32 loop) {
    Voice v = {0};
    v.vpb.pb.state = 1;
    v.vpb.pb.addr.format = format;
    v.vpb.pb.addr.loopFlag = loop != 0;
    v.vpb.pb.src.ratioHi = 1;
    v.cur_addr = current;
    v.end_addr = end;
    v.loop_addr = loop;
    return v;
}

static void inclusive_end(u16 format) {
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

static void ring_transition(u32 from, u32 to) {
    /* HPS blocks have 64 KiB spacing, with nibble-addressed ADPCM.
     * Decode across a boundary, leaving the old end address in place until
     * the next 5 ms synth callback. The new block must advance normally. */
    u32 old_end = from + 15;
    Voice v = voice(AX_FORMAT_ADPCM, old_end, old_end, to + 2);
    float output[AX_FRAME * 2] = {0};
    memory[old_end / 2] = 0x07;
    for (u32 i = to / 2; i < to / 2 + 256; i += 8) {
        memory[i] = 0;
        memset(memory + i + 1, 0x33, 7);
    }
    mix_voice(&v, output);
    assert(v.vpb.pb.state == 1);
    assert(v.cur_addr > to + 100 && v.cur_addr < to + 256);
    assert(addr32(v.vpb.pb.addr.currentAddressHi, v.vpb.pb.addr.currentAddressLo) == v.cur_addr);
    AXSetVoiceEndAddr(&v.vpb, to + 1023);
    mix_voice(&v, output);
    assert(v.vpb.pb.state == 1);
    assert(v.cur_addr > to + 256);
}

static void loop_history(void) {
    Voice v = voice(AX_FORMAT_ADPCM, 15, 15, 0x20002);
    s16 sample;
    memory[7] = 7;
    v.vpb.pb.adpcmLoop.loop_pred_scale = 0x13;
    v.vpb.pb.adpcmLoop.loop_yn1 = (u16)-123;
    v.vpb.pb.adpcmLoop.loop_yn2 = 456;
    assert(next_sample(&v, &sample));
    assert(sample == 7);
    assert(v.cur_addr == 0x20002);
    assert(v.pred_scale == 0x13);
    assert(v.yn1 == -123 && v.yn2 == 456);
}

static void negative_samples(void) {
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

static void volume_scaling(void) {
    /* Initialize memory for PCM16 test samples */
    for (int i = 0; i < 200; i++) {
        memory[i * 2] = 0x40; /* 16384 (0.5f full-scale) */
        memory[i * 2 + 1] = 0x00;
    }

    /* 1. Verify default volumes are 1.0f */
    assert(pc_get_music_volume() == 1.0f);
    assert(pc_get_sfx_volume() == 1.0f);

    /* 2. SFX voice (PCM16, no loop) */
    Voice sfx = voice(AX_FORMAT_PCM16, 2, 100, 0);
    sfx.vpb.pb.mix.vL = 32767;
    sfx.vpb.pb.mix.vR = 32767;
    sfx.vpb.pb.ve.currentVolume = 32767;

    /* 3. Music stream voice (ADPCM across 64 KiB ring buffer boundary) */
    Voice bgm = voice(AX_FORMAT_ADPCM, 0x20000 + 15, 0x20000 + 15, 0x40000 + 2);
    bgm.vpb.pb.mix.vL = 32767;
    bgm.vpb.pb.mix.vR = 32767;
    bgm.vpb.pb.ve.currentVolume = 32767;
    memory[(0x20000 + 15) / 2] = 0x07;
    for (u32 i = 0x40000 / 2; i < 0x40000 / 2 + 256; i += 8) {
        memory[i] = 0;
        memset(memory + i + 1, 0x33, 7);
    }

    /* Verify stream voice is correctly identified */
    float bgm_out[AX_FRAME * 2] = {0};
    mix_voice(&bgm, bgm_out);
    assert(bgm.is_stream == true);

    /* Set independent volumes: SFX=40%, Music=80% */
    pc_audio_set_sfx_volume(0.4f);
    pc_audio_set_music_volume(0.8f);
    assert(pc_get_sfx_volume() == 0.4f);
    assert(pc_get_music_volume() == 0.8f);

    float sfx_out[AX_FRAME * 2] = {0};
    mix_voice(&sfx, sfx_out);
    /* Sample 1: 0.5f * 0.4f = 0.20f */
    assert(fabsf(sfx_out[2] - 0.20f) < 0.01f);

    /* Test mute (0.0f): voice advances without producing output */
    pc_audio_set_sfx_volume(0.0f);
    Voice sfx_mute = voice(AX_FORMAT_PCM16, 2, 100, 0);
    sfx_mute.vpb.pb.mix.vL = 32767;
    sfx_mute.vpb.pb.mix.vR = 32767;
    sfx_mute.vpb.pb.ve.currentVolume = 32767;
    float mute_out[AX_FRAME * 2] = {0};
    mix_voice(&sfx_mute, mute_out);
    assert(mute_out[2] == 0.0f && mute_out[3] == 0.0f);
    assert(sfx_mute.cur_addr > 2);

    /* Test music mute (0.0f) */
    pc_audio_set_music_volume(0.0f);
    Voice bgm_mute = voice(AX_FORMAT_ADPCM, 0x20000 + 15, 0x20000 + 15, 0x40000 + 2);
    bgm_mute.vpb.pb.mix.vL = 32767;
    bgm_mute.vpb.pb.mix.vR = 32767;
    bgm_mute.vpb.pb.ve.currentVolume = 32767;
    float bgm_mute_out[AX_FRAME * 2] = {0};
    mix_voice(&bgm_mute, bgm_mute_out);
    for (int i = 0; i < AX_FRAME * 2; i++) {
        assert(bgm_mute_out[i] == 0.0f);
    }
    assert(bgm_mute.cur_addr > 0x40000);

    /* Reset volumes back to default */
    pc_audio_set_music_volume(1.0f);
    pc_audio_set_sfx_volume(1.0f);
}

int main(void) {
    s_aram = memory;
    ring_transition(0x20000, 0x40000);
    inclusive_end(AX_FORMAT_PCM8);
    inclusive_end(AX_FORMAT_PCM16);
    inclusive_end(AX_FORMAT_ADPCM);
    ring_transition(0x40000, 0);
    loop_history();
    negative_samples();
    volume_scaling();
    puts("PASS: inclusive ends, HPS ring transitions, loop history, signed samples, and volume "
         "scaling");
}
