#include "test.h"
#include "fluidsynth.h"
#include "fluid_synth.h"
#include "fluid_voice.h"

#include <string.h>

enum
{
    TAIL_FRAMES = 1024,
    PREFIX_FRAMES = 137,
    RESET_FRAMES = 384
};

typedef struct
{
    unsigned int volume;
    unsigned int expression;
    unsigned int pan;
    unsigned int modulation;
    int pitch_bend;
    unsigned int volenv_section;
    unsigned int volenv_count;
    fluid_real_t volenv_val;
    unsigned int modenv_section;
    unsigned int modenv_count;
    fluid_real_t modenv_val;
    fluid_real_t modlfo_val;
    fluid_real_t modlfo_incr;
    fluid_real_t viblfo_val;
    fluid_real_t viblfo_incr;
    fluid_phase_t phase;
    fluid_real_t filter_hist1;
    fluid_real_t filter_hist2;
} voice_checkpoint_t;

static int find_voice(fluid_synth_t *synth, int chan, int key,
                      fluid_voice_t **result)
{
    fluid_voice_t *voices[256];
    int i;

    fluid_synth_get_voicelist(synth, voices, 256, -1);

    for(i = 0; i < 256 && voices[i] != NULL; ++i)
    {
        if(fluid_voice_get_channel(voices[i]) == chan
           && fluid_voice_get_key(voices[i]) == key
           && fluid_voice_is_playing(voices[i]))
        {
            *result = voices[i];
            return 1;
        }
    }

    return 0;
}

static void capture_voice_checkpoint(fluid_synth_t *synth,
                                     voice_checkpoint_t *checkpoint)
{
    fluid_voice_t *voice;
    int volume;
    int expression;
    int pan;
    int modulation;
    int pitch_bend;

    TEST_ASSERT(find_voice(synth, 0, 60, &voice));
    TEST_SUCCESS(fluid_synth_get_cc(synth, 0, 7, &volume));
    TEST_SUCCESS(fluid_synth_get_cc(synth, 0, 11, &expression));
    TEST_SUCCESS(fluid_synth_get_cc(synth, 0, 10, &pan));
    TEST_SUCCESS(fluid_synth_get_cc(synth, 0, 1, &modulation));
    TEST_SUCCESS(fluid_synth_get_pitch_bend(synth, 0, &pitch_bend));

    checkpoint->volume = (unsigned int) volume;
    checkpoint->expression = (unsigned int) expression;
    checkpoint->pan = (unsigned int) pan;
    checkpoint->modulation = (unsigned int) modulation;
    checkpoint->pitch_bend = pitch_bend;
    checkpoint->volenv_section = voice->rvoice->envlfo.volenv.section;
    checkpoint->volenv_count = voice->rvoice->envlfo.volenv.count;
    checkpoint->volenv_val = voice->rvoice->envlfo.volenv.val;
    checkpoint->modenv_section = voice->rvoice->envlfo.modenv.section;
    checkpoint->modenv_count = voice->rvoice->envlfo.modenv.count;
    checkpoint->modenv_val = voice->rvoice->envlfo.modenv.val;
    checkpoint->modlfo_val = voice->rvoice->envlfo.modlfo.val;
    checkpoint->modlfo_incr = voice->rvoice->envlfo.modlfo.increment;
    checkpoint->viblfo_val = voice->rvoice->envlfo.viblfo.val;
    checkpoint->viblfo_incr = voice->rvoice->envlfo.viblfo.increment;
    checkpoint->phase = voice->rvoice->dsp.phase;
    checkpoint->filter_hist1 = voice->rvoice->resonant_filter.hist1;
    checkpoint->filter_hist2 = voice->rvoice->resonant_filter.hist2;
}

static void verify_voice_checkpoint(fluid_synth_t *synth,
                                    const voice_checkpoint_t *checkpoint)
{
    voice_checkpoint_t actual;

    capture_voice_checkpoint(synth, &actual);
    TEST_ASSERT(actual.volume == checkpoint->volume);
    TEST_ASSERT(actual.expression == checkpoint->expression);
    TEST_ASSERT(actual.pan == checkpoint->pan);
    TEST_ASSERT(actual.modulation == checkpoint->modulation);
    TEST_ASSERT(actual.pitch_bend == checkpoint->pitch_bend);
    TEST_ASSERT(actual.volenv_section == checkpoint->volenv_section);
    TEST_ASSERT(actual.volenv_count == checkpoint->volenv_count);
    TEST_ASSERT(actual.volenv_val == checkpoint->volenv_val);
    TEST_ASSERT(actual.modenv_section == checkpoint->modenv_section);
    TEST_ASSERT(actual.modenv_count == checkpoint->modenv_count);
    TEST_ASSERT(actual.modenv_val == checkpoint->modenv_val);
    TEST_ASSERT(actual.modlfo_val == checkpoint->modlfo_val);
    TEST_ASSERT(actual.modlfo_incr == checkpoint->modlfo_incr);
    TEST_ASSERT(actual.viblfo_val == checkpoint->viblfo_val);
    TEST_ASSERT(actual.viblfo_incr == checkpoint->viblfo_incr);
    TEST_ASSERT(actual.phase == checkpoint->phase);
    TEST_ASSERT(actual.filter_hist1 == checkpoint->filter_hist1);
    TEST_ASSERT(actual.filter_hist2 == checkpoint->filter_hist2);
}

static void render(fluid_synth_t *synth, int frames,
                   float *left, float *right)
{
    TEST_SUCCESS(fluid_synth_write_float(synth, frames,
                                         left, 0, 1, right, 0, 1));
}

static void compare_samples(const char *label, const char *engine,
                            const float *ref, const float *actual, int frames)
{
    float max_error = 0.0f;
    int mismatches = 0;
    int i;

    for(i = 0; i < frames; ++i)
    {
        float error;

        if(memcmp(&ref[i], &actual[i], sizeof(ref[i])) != 0)
        {
            mismatches++;
        }

        error = ref[i] > actual[i] ? ref[i] - actual[i] : actual[i] - ref[i];

        if(error > max_error)
        {
            max_error = error;
        }
    }

    printf("snapshot %s engine=%s frames=%d bit_mismatches=%d max_abs_error=%.9g\n",
           label, engine, frames, mismatches, (double) max_error);
    TEST_ASSERT(mismatches == 0);
}

static void set_test_controllers(fluid_synth_t *synth)
{
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 7, 90));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 11, 100));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 10, 32));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 1, 40));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 91, 80));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 93, 60));
    TEST_SUCCESS(fluid_synth_pitch_bend(synth, 0, 9000));
}

static void test_empty_snapshot_roundtrip(fluid_synth_t *synth)
{
    fluid_synth_snapshot_t *snapshot;
    float ref_left[RESET_FRAMES];
    float ref_right[RESET_FRAMES];
    float replay_left[RESET_FRAMES];
    float replay_right[RESET_FRAMES];

    TEST_SUCCESS(fluid_synth_snapshot_save(synth, &snapshot));
    TEST_ASSERT(fluid_synth_snapshot_get_position(snapshot) == 0);

    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    set_test_controllers(synth);
    render(synth, RESET_FRAMES, ref_left, ref_right);

    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    set_test_controllers(synth);
    render(synth, RESET_FRAMES, replay_left, replay_right);

    TEST_ASSERT(memcmp(ref_left, replay_left, sizeof(ref_left)) == 0);
    TEST_ASSERT(memcmp(ref_right, replay_right, sizeof(ref_right)) == 0);

    fluid_synth_snapshot_free(snapshot);
}

static void test_engine_roundtrip(const char *engine)
{
    fluid_settings_t *settings = new_fluid_settings();
    fluid_synth_t *synth;
    fluid_synth_snapshot_t *snapshot;
    float prefix_left[PREFIX_FRAMES];
    float prefix_right[PREFIX_FRAMES];
    float ref_left[TAIL_FRAMES];
    float ref_right[TAIL_FRAMES];
    float replay_left[TAIL_FRAMES];
    float replay_right[TAIL_FRAMES];

    TEST_ASSERT(settings != NULL);
    TEST_SUCCESS(fluid_settings_setnum(settings, "synth.sample-rate", 44100));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.reverb.active", 1));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.chorus.active", 1));
    TEST_SUCCESS(fluid_settings_setstr(settings, "synth.reverb.engine", engine));

    synth = new_fluid_synth(settings);
    TEST_ASSERT(synth != NULL);
    TEST_ASSERT(fluid_synth_sfload(synth, TEST_SOUNDFONT, 1) != FLUID_FAILED);

    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    TEST_SUCCESS(fluid_synth_noteon(synth, 1, 67, 110));
    set_test_controllers(synth);
    render(synth, PREFIX_FRAMES, prefix_left, prefix_right);
    TEST_SUCCESS(fluid_synth_snapshot_save(synth, &snapshot));

    render(synth, TAIL_FRAMES, ref_left, ref_right);
    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    render(synth, TAIL_FRAMES, replay_left, replay_right);
    compare_samples("left", engine, ref_left, replay_left, TAIL_FRAMES);
    compare_samples("right", engine, ref_right, replay_right, TAIL_FRAMES);

    fluid_synth_snapshot_free(snapshot);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
}

int main(void)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_synth_snapshot_t *snapshot;
    fluid_synth_snapshot_t *second_snapshot = NULL;
    voice_checkpoint_t checkpoint;
    float prefix_left[PREFIX_FRAMES];
    float prefix_right[PREFIX_FRAMES];
    float ref_left[TAIL_FRAMES];
    float ref_right[TAIL_FRAMES];
    float replay_left[TAIL_FRAMES];
    float replay_right[TAIL_FRAMES];
    short s16_ref[2 * TAIL_FRAMES];
    short s16_replay[2 * TAIL_FRAMES];
    int sfont_id;

    TEST_DISABLE_STDOUT_BUFFERING();

    test_engine_roundtrip("dat");
    test_engine_roundtrip("fdn");
    test_engine_roundtrip("free");
    test_engine_roundtrip("lex");

    settings = new_fluid_settings();
    TEST_ASSERT(settings != NULL);
    TEST_SUCCESS(fluid_settings_setnum(settings, "synth.sample-rate", 44100));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.polyphony", 64));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.reverb.active", 1));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.chorus.active", 1));
    TEST_SUCCESS(fluid_settings_setstr(settings, "synth.reverb.engine", "dat"));

    synth = new_fluid_synth(settings);
    TEST_ASSERT(synth != NULL);

    sfont_id = fluid_synth_sfload(synth, TEST_SOUNDFONT, 1);
    TEST_ASSERT(sfont_id != FLUID_FAILED);

    test_empty_snapshot_roundtrip(synth);

    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    TEST_SUCCESS(fluid_synth_noteon(synth, 1, 67, 110));
    set_test_controllers(synth);
    render(synth, PREFIX_FRAMES, prefix_left, prefix_right);

    TEST_SUCCESS(fluid_synth_snapshot_save(synth, &snapshot));
    TEST_ASSERT(fluid_synth_snapshot_get_position(snapshot)
                == RESET_FRAMES + PREFIX_FRAMES);

    {
        fluid_settings_t *other_settings = new_fluid_settings();
        fluid_synth_t *other_synth;

        TEST_ASSERT(other_settings != NULL);
        other_synth = new_fluid_synth(other_settings);
        TEST_ASSERT(other_synth != NULL);
        TEST_ASSERT(fluid_synth_snapshot_restore(other_synth, snapshot)
                    == FLUID_FAILED);
        delete_fluid_synth(other_synth);
        delete_fluid_settings(other_settings);
    }

    capture_voice_checkpoint(synth, &checkpoint);

    TEST_SUCCESS(fluid_synth_write_s16(synth, TAIL_FRAMES,
                                       s16_ref, 0, 2, s16_ref, 1, 2));
    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    TEST_SUCCESS(fluid_synth_write_s16(synth, TAIL_FRAMES,
                                       s16_replay, 0, 2, s16_replay, 1, 2));
    printf("snapshot s16 engine=dat samples=%d bit_mismatches=%d\n",
           (int) (2 * TAIL_FRAMES),
           memcmp(s16_ref, s16_replay, sizeof(s16_ref)) == 0 ? 0 : 1);
    TEST_ASSERT(memcmp(s16_ref, s16_replay, sizeof(s16_ref)) == 0);

    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    verify_voice_checkpoint(synth, &checkpoint);
    render(synth, TAIL_FRAMES, ref_left, ref_right);

    TEST_SUCCESS(fluid_synth_cc(synth, 0, 7, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 10, 127));
    TEST_SUCCESS(fluid_synth_pitch_bend(synth, 0, 0));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));

    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    verify_voice_checkpoint(synth, &checkpoint);
    render(synth, TAIL_FRAMES, replay_left, replay_right);

    compare_samples("left", "dat", ref_left, replay_left, TAIL_FRAMES);
    compare_samples("right", "dat", ref_right, replay_right, TAIL_FRAMES);

    /* The same snapshot can seek backwards repeatedly. */
    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, snapshot));
    render(synth, TAIL_FRAMES, replay_left, replay_right);
    compare_samples("repeat-left", "dat", ref_left, replay_left, TAIL_FRAMES);
    compare_samples("repeat-right", "dat", ref_right, replay_right, TAIL_FRAMES);

    TEST_ASSERT(fluid_synth_snapshot_restore(synth, NULL) == FLUID_FAILED);
    TEST_ASSERT(fluid_synth_snapshot_get_position(NULL) == -1);
    fluid_synth_snapshot_free(NULL);

    /* Capture release/sustain state, with a partial mixer block pending. */
    TEST_SUCCESS(fluid_synth_cc(synth, 0, 64, 127));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render(synth, 211, replay_left, replay_right);
    TEST_SUCCESS(fluid_synth_snapshot_save(synth, &second_snapshot));
    TEST_ASSERT(fluid_synth_snapshot_get_position(second_snapshot)
                == RESET_FRAMES + PREFIX_FRAMES + TAIL_FRAMES + 211);

    render(synth, TAIL_FRAMES, ref_left, ref_right);
    TEST_SUCCESS(fluid_synth_snapshot_restore(synth, second_snapshot));
    render(synth, TAIL_FRAMES, replay_left, replay_right);
    compare_samples("release-left", "dat", ref_left, replay_left, TAIL_FRAMES);
    compare_samples("release-right", "dat", ref_right, replay_right, TAIL_FRAMES);

    fluid_synth_snapshot_free(second_snapshot);
    second_snapshot = NULL;

    render(synth, 36, replay_left, replay_right);
    TEST_SUCCESS(fluid_synth_snapshot_save(synth, &second_snapshot));
    TEST_ASSERT(fluid_synth_snapshot_get_position(second_snapshot)
                == RESET_FRAMES + PREFIX_FRAMES + TAIL_FRAMES + 211
                + TAIL_FRAMES + 36);
    TEST_ASSERT(fluid_synth_snapshot_get_position(second_snapshot) % 64 == 0);
    fluid_synth_snapshot_free(second_snapshot);
    second_snapshot = NULL;

    fluid_synth_snapshot_free(snapshot);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    return EXIT_SUCCESS;
}
