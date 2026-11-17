/*
 * FluidSynth - A Software Synthesizer
 *
 * Sample-accurate synthesizer state snapshots.
 */

#include "fluid_synth.h"
#include "fluid_chan.h"
#include "fluid_tuning.h"
#include "fluid_rvoice_mixer.h"
#include "fluid_chorus.h"
#include "fluid_rev.h"

typedef struct
{
    fluid_voice_t voice;
    fluid_rvoice_t rvoice;
    fluid_rvoice_t overflow_rvoice;
    int channel_index;
} fluid_voice_snapshot_t;

struct _fluid_synth_snapshot_t
{
    fluid_synth_t *owner;
    fluid_long_long_t position;

    int midi_channels;
    int nvoice;
    int polyphony;
    int audio_channels;
    int audio_groups;
    int effects_channels;
    int effects_groups;
    double sample_rate;

    unsigned int ticks;
    int state;
    int active_voice_count;
    unsigned int noteid;
    unsigned int storeid;
    int fromkey_portamento;
    int cur;
    int curmax;
    int dither_index;
    int bank_select;
    int with_reverb;
    int with_chorus;
    int reverb_type;
    int custom_filter_type;
    int custom_filter_flags;
    int msgs_note_cut_mode;
    int portamento_time_mode;
    int portamento_time_has_seen_lsb;
    float gain;

    fluid_channel_t *channels;
    fluid_voice_snapshot_t *voices;
    fluid_rvoice_mixer_state_t *mixer;
};

static void fluid_synth_snapshot_clear(fluid_synth_snapshot_t *snapshot)
{
    int i;

    if(snapshot == NULL)
    {
        return;
    }

    fluid_rvoice_mixer_state_free(snapshot->mixer);

    if(snapshot->voices != NULL)
    {
        for(i = 0; i < snapshot->nvoice; ++i)
        {
            fluid_sample_t *sample = snapshot->voices[i].voice.sample;
            fluid_sample_t *overflow_sample =
                snapshot->voices[i].voice.overflow_sample;

            if(sample != NULL)
            {
                fluid_sample_decr_ref(sample);
            }

            if(overflow_sample != NULL)
            {
                fluid_sample_decr_ref(overflow_sample);
            }
        }
    }

    if(snapshot->channels != NULL)
    {
        for(i = 0; i < snapshot->midi_channels; ++i)
        {
            if(snapshot->channels[i].tuning != NULL)
            {
                fluid_tuning_unref(snapshot->channels[i].tuning, 1);
            }
        }
    }

    FLUID_FREE(snapshot->channels);
    FLUID_FREE(snapshot->voices);
    FLUID_FREE(snapshot);
}

static void fluid_synth_snapshot_commit_events(fluid_synth_t *synth)
{
    fluid_rvoice_eventhandler_flush(synth->eventhandler);
    fluid_rvoice_eventhandler_dispatch_all(synth->eventhandler);
    fluid_synth_check_finished_voices(synth);
}

static int fluid_synth_snapshot_find_channel(const fluid_synth_t *synth,
        const fluid_voice_t *voice)
{
    int i;

    for(i = 0; i < synth->midi_channels; ++i)
    {
        if(voice->channel == synth->channel[i])
        {
            return i;
        }
    }

    if(voice->chan < synth->midi_channels)
    {
        return voice->chan;
    }

    return -1;
}

int
fluid_synth_snapshot_save(fluid_synth_t *synth,
                          fluid_synth_snapshot_t **snapshot)
{
    fluid_synth_snapshot_t *result;
    int i;
    unsigned int ticks;
    int pending;

    fluid_return_val_if_fail(synth != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(snapshot != NULL, FLUID_FAILED);
    *snapshot = NULL;

    fluid_synth_snapshot_commit_events(synth);

    result = FLUID_NEW(fluid_synth_snapshot_t);

    if(result == NULL)
    {
        return FLUID_FAILED;
    }

    FLUID_MEMSET(result, 0, sizeof(*result));
    result->owner = synth;
    result->midi_channels = synth->midi_channels;
    result->nvoice = synth->nvoice;
    result->polyphony = synth->polyphony;
    result->audio_channels = synth->audio_channels;
    result->audio_groups = synth->audio_groups;
    result->effects_channels = synth->effects_channels;
    result->effects_groups = synth->effects_groups;
    result->sample_rate = synth->sample_rate;

    ticks = fluid_synth_get_ticks(synth);
    pending = synth->curmax > synth->cur ? synth->curmax - synth->cur : 0;
    result->position = ticks >= (unsigned int) pending
                       ? (fluid_long_long_t) (ticks - (unsigned int) pending) : 0;
    result->ticks = ticks;
    result->state = synth->state;
    result->active_voice_count = synth->active_voice_count;
    result->noteid = synth->noteid;
    result->storeid = synth->storeid;
    result->fromkey_portamento = synth->fromkey_portamento;
    result->cur = synth->cur;
    result->curmax = synth->curmax;
    result->dither_index = synth->dither_index;
    result->bank_select = synth->bank_select;
    result->with_reverb = synth->with_reverb;
    result->with_chorus = synth->with_chorus;
    result->reverb_type = synth->reverb_type;
    result->custom_filter_type = synth->custom_filter_type;
    result->custom_filter_flags = synth->custom_filter_flags;
    result->msgs_note_cut_mode = synth->msgs_note_cut_mode;
    result->portamento_time_mode = synth->portamento_time_mode;
    result->portamento_time_has_seen_lsb = synth->portamento_time_has_seen_lsb;
    result->gain = synth->gain;

    result->channels = FLUID_ARRAY(fluid_channel_t, synth->midi_channels);
    result->voices = FLUID_ARRAY(fluid_voice_snapshot_t, synth->nvoice);

    if(result->channels == NULL || result->voices == NULL)
    {
        fluid_synth_snapshot_clear(result);
        return FLUID_FAILED;
    }

    for(i = 0; i < synth->midi_channels; ++i)
    {
        FLUID_MEMCPY(&result->channels[i], synth->channel[i],
                     sizeof(fluid_channel_t));

        if(result->channels[i].tuning != NULL)
        {
            fluid_tuning_ref(result->channels[i].tuning);
        }
    }

    for(i = 0; i < synth->nvoice; ++i)
    {
        fluid_voice_snapshot_t *voice_state = &result->voices[i];
        fluid_voice_t *voice = synth->voice[i];

        FLUID_MEMCPY(&voice_state->voice, voice, sizeof(*voice));
        FLUID_MEMCPY(&voice_state->rvoice, voice->rvoice,
                     sizeof(*voice->rvoice));
        FLUID_MEMCPY(&voice_state->overflow_rvoice, voice->overflow_rvoice,
                     sizeof(*voice->overflow_rvoice));
        voice_state->channel_index =
            fluid_synth_snapshot_find_channel(synth, voice);

        if(voice_state->voice.sample != NULL)
        {
            fluid_sample_incr_ref(voice_state->voice.sample);
        }

        if(voice_state->voice.overflow_sample != NULL)
        {
            fluid_sample_incr_ref(voice_state->voice.overflow_sample);
        }
    }

    result->mixer = fluid_rvoice_mixer_state_save(synth->eventhandler->mixer,
                    synth->voice, synth->nvoice);

    if(result->mixer == NULL)
    {
        fluid_synth_snapshot_clear(result);
        return FLUID_FAILED;
    }

    *snapshot = result;
    return FLUID_OK;
}

int
fluid_synth_snapshot_restore(fluid_synth_t *synth,
                             const fluid_synth_snapshot_t *snapshot)
{
    fluid_tuning_t *old_tunings[128];
    fluid_tuning_t **tunings = NULL;
    int i;

    fluid_return_val_if_fail(synth != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(snapshot != NULL, FLUID_FAILED);

    if(snapshot->owner != synth
       || snapshot->midi_channels != synth->midi_channels
       || snapshot->nvoice != synth->nvoice
       || snapshot->polyphony != synth->polyphony
       || snapshot->audio_channels != synth->audio_channels
       || snapshot->audio_groups != synth->audio_groups
       || snapshot->effects_channels != synth->effects_channels
       || snapshot->effects_groups != synth->effects_groups
       || snapshot->sample_rate != synth->sample_rate
       || snapshot->reverb_type != synth->reverb_type)
    {
        return FLUID_FAILED;
    }

    if(snapshot->midi_channels <= 128)
    {
        tunings = old_tunings;
    }
    else
    {
        tunings = FLUID_ARRAY(fluid_tuning_t *, snapshot->midi_channels);
    }

    if(tunings == NULL)
    {
        return FLUID_FAILED;
    }

    fluid_synth_snapshot_commit_events(synth);

    for(i = 0; i < snapshot->midi_channels; ++i)
    {
        fluid_channel_t *channel = synth->channel[i];
        fluid_channel_t *saved = &snapshot->channels[i];

        if(saved->tuning != NULL)
        {
            fluid_tuning_ref(saved->tuning);
        }

        old_tunings[i] = channel->tuning;
        fluid_channel_set_preset(channel, saved->preset);
        FLUID_MEMCPY(channel, saved, sizeof(*channel));
        channel->synth = synth;
        channel->channum = i;

        if(old_tunings[i] != NULL)
        {
            fluid_tuning_unref(old_tunings[i], 1);
        }
    }

    for(i = 0; i < snapshot->nvoice; ++i)
    {
        const fluid_voice_snapshot_t *saved = &snapshot->voices[i];
        fluid_voice_t *voice = synth->voice[i];
        fluid_rvoice_t *rvoice = voice->rvoice;
        fluid_rvoice_t *overflow_rvoice = voice->overflow_rvoice;
        fluid_sample_t *old_sample;
        fluid_sample_t *old_overflow_sample;

        if(saved->voice.sample != NULL)
        {
            fluid_sample_incr_ref(saved->voice.sample);
        }

        if(saved->voice.overflow_sample != NULL)
        {
            fluid_sample_incr_ref(saved->voice.overflow_sample);
        }

        old_sample = voice->sample;
        old_overflow_sample = voice->overflow_sample;

        FLUID_MEMCPY(voice, &saved->voice, sizeof(*voice));
        voice->rvoice = rvoice;
        voice->overflow_rvoice = overflow_rvoice;
        FLUID_MEMCPY(rvoice, &saved->rvoice, sizeof(*rvoice));
        FLUID_MEMCPY(overflow_rvoice, &saved->overflow_rvoice,
                     sizeof(*overflow_rvoice));

        voice->channel = saved->channel_index >= 0
                         ? synth->channel[saved->channel_index] : NULL;
        voice->eventhandler = synth->eventhandler;
        voice->rvoice->dsp.sample = voice->sample;
        voice->rvoice->resonant_filter.sincos_table =
            &synth->iir_sincos_table[0];
        voice->rvoice->resonant_custom_filter.sincos_table =
            &synth->iir_sincos_table[0];

        if(old_sample != NULL)
        {
            fluid_sample_decr_ref(old_sample);
        }

        if(old_overflow_sample != NULL)
        {
            fluid_sample_decr_ref(old_overflow_sample);
        }
    }

    if(fluid_rvoice_mixer_state_restore(synth->eventhandler->mixer,
                                        snapshot->mixer, synth->voice,
                                        synth->nvoice) != FLUID_OK)
    {
        if(tunings != old_tunings)
        {
            FLUID_FREE(tunings);
        }

        return FLUID_FAILED;
    }

    fluid_atomic_int_set((fluid_atomic_int_t *) &synth->ticks_since_start,
                         (int) snapshot->ticks);
    synth->state = snapshot->state;
    synth->active_voice_count = snapshot->active_voice_count;
    synth->noteid = snapshot->noteid;
    synth->storeid = snapshot->storeid;
    synth->fromkey_portamento = snapshot->fromkey_portamento;
    synth->cur = snapshot->cur;
    synth->curmax = snapshot->curmax;
    synth->dither_index = snapshot->dither_index;
    synth->bank_select = snapshot->bank_select;
    synth->with_reverb = snapshot->with_reverb;
    synth->with_chorus = snapshot->with_chorus;
    synth->custom_filter_type = snapshot->custom_filter_type;
    synth->custom_filter_flags = snapshot->custom_filter_flags;
    synth->msgs_note_cut_mode = snapshot->msgs_note_cut_mode;
    synth->portamento_time_mode = snapshot->portamento_time_mode;
    synth->portamento_time_has_seen_lsb =
        snapshot->portamento_time_has_seen_lsb;
    synth->gain = snapshot->gain;

    if(tunings != old_tunings)
    {
        FLUID_FREE(tunings);
    }

    return FLUID_OK;
}

void
fluid_synth_snapshot_free(fluid_synth_snapshot_t *snapshot)
{
    fluid_synth_snapshot_clear(snapshot);
}

fluid_long_long_t
fluid_synth_snapshot_get_position(const fluid_synth_snapshot_t *snapshot)
{
    fluid_return_val_if_fail(snapshot != NULL, -1);

    return snapshot->position;
}
