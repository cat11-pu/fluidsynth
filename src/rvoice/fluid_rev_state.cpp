/*
 * FluidSynth - A Software Synthesizer
 *
 * Reverb engine state snapshots.
 */

#include "fluid_rev.h"

#include "fluid_rev_dattorro.h"
#include "fluid_rev_fdn.h"
#include "fluid_rev_freeverb.h"
#include "fluid_rev_lexverb.h"
#include "fluid_rev_state_codec.h"

#ifdef SIGNALSMITH_SUPPORT
#include "fluid_rev_signalsmith.h"
#endif

#include <new>

using fluid_reverb_state::Reader;
using fluid_reverb_state::Writer;

struct _fluid_revmodel_state_t
{
    std::vector<unsigned char> data;
};

namespace
{

enum engine_tag : uint32_t
{
    ENGINE_FDN = 0x46444e31,
    ENGINE_FREEVERB = 0x46525631,
    ENGINE_LEXVERB = 0x4c455831,
    ENGINE_DATTORRO = 0x44415431,
    ENGINE_SIGNALSMITH = 0x534d5431
};

void write_tag(Writer &writer, engine_tag tag)
{
    uint32_t value = tag;
    writer.pod(value);
}

bool read_tag(Reader &reader, engine_tag expected)
{
    uint32_t value;

    return reader.pod(value) && value == expected;
}

} // namespace

fluid_revmodel_state_t *
fluid_revmodel_state_save(const fluid_revmodel_t *rev)
{
    fluid_revmodel_state_t *state;

    fluid_return_val_if_fail(rev != NULL, NULL);

    state = new(std::nothrow) fluid_revmodel_state_t();

    if(state == NULL)
    {
        return NULL;
    }

    if(!rev->save_state(state->data))
    {
        delete state;
        return NULL;
    }

    return state;
}

int
fluid_revmodel_state_restore(fluid_revmodel_t *rev,
                             const fluid_revmodel_state_t *state)
{
    fluid_return_val_if_fail(rev != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(state != NULL, FLUID_FAILED);

    return rev->restore_state(state->data) ? FLUID_OK : FLUID_FAILED;
}

void
fluid_revmodel_state_free(fluid_revmodel_state_t *state)
{
    delete state;
}

bool
fluid_revmodel_fdn::save_state(std::vector<unsigned char> &data) const
{
    Writer writer;

    write_tag(writer, ENGINE_FDN);
    writer.pod(roomsize);
    writer.pod(damp);
    writer.pod(level);
    writer.pod(wet1);
    writer.pod(wet2);
    writer.pod(width);
    writer.pod(late.samplerate);
    writer.pod(late.sample_rate_max);
    writer.pod(late.tone_buffer);
    writer.pod(late.b1);
    writer.pod(late.b2);

    for(int i = 0; i < NBR_DELAYS; ++i)
    {
        const mod_delay_line &line = late.mod_delay_lines[i];
        fluid_reverb_state::write_delay_line(writer, line.dl);
        writer.pod(line.mod.a1);
        writer.pod(line.mod.buffer1);
        writer.pod(line.mod.buffer2);
        writer.pod(line.mod.reset_buffer2);
        writer.pod(line.center_pos_mod);
        writer.pod(line.mod_depth);
        writer.pod(line.index_rate);
        writer.pod(line.mod_rate);
        writer.pod(line.frac_pos_mod);
        writer.pod(line.buffer);
        writer.pod(late.out_left_gain[i]);
        writer.pod(late.out_right_gain[i]);
    }

    data.swap(writer.data);
    return true;
}

bool
fluid_revmodel_fdn::restore_state(const std::vector<unsigned char> &data)
{
    Reader reader(data);

    if(!read_tag(reader, ENGINE_FDN)
       || !reader.pod(roomsize)
       || !reader.pod(damp)
       || !reader.pod(level)
       || !reader.pod(wet1)
       || !reader.pod(wet2)
       || !reader.pod(width)
       || !reader.pod(late.samplerate)
       || !reader.pod(late.sample_rate_max)
       || !reader.pod(late.tone_buffer)
       || !reader.pod(late.b1)
       || !reader.pod(late.b2))
    {
        return false;
    }

    for(int i = 0; i < NBR_DELAYS; ++i)
    {
        mod_delay_line &line = late.mod_delay_lines[i];

        if(!fluid_reverb_state::read_delay_line(reader, line.dl)
           || !reader.pod(line.mod.a1)
           || !reader.pod(line.mod.buffer1)
           || !reader.pod(line.mod.buffer2)
           || !reader.pod(line.mod.reset_buffer2)
           || !reader.pod(line.center_pos_mod)
           || !reader.pod(line.mod_depth)
           || !reader.pod(line.index_rate)
           || !reader.pod(line.mod_rate)
           || !reader.pod(line.frac_pos_mod)
           || !reader.pod(line.buffer)
           || !reader.pod(late.out_left_gain[i])
           || !reader.pod(late.out_right_gain[i]))
        {
            return false;
        }
    }

    return reader.at_end();
}

bool
fluid_revmodel_freeverb::save_state(std::vector<unsigned char> &data) const
{
    Writer writer;

    write_tag(writer, ENGINE_FREEVERB);
    writer.pod(roomsize);
    writer.pod(damp);
    writer.pod(level);
    writer.pod(wet1);
    writer.pod(wet2);
    writer.pod(width);
    writer.pod(gain);

    for(int i = 0; i < numcombs; ++i)
    {
        fluid_reverb_state::write_comb(writer, combL[i]);
        fluid_reverb_state::write_comb(writer, combR[i]);
    }

    for(int i = 0; i < numallpasses; ++i)
    {
        fluid_reverb_state::write_allpass(writer, allpassL[i]);
        fluid_reverb_state::write_allpass(writer, allpassR[i]);
    }

    data.swap(writer.data);
    return true;
}

bool
fluid_revmodel_freeverb::restore_state(const std::vector<unsigned char> &data)
{
    Reader reader(data);

    if(!read_tag(reader, ENGINE_FREEVERB)
       || !reader.pod(roomsize)
       || !reader.pod(damp)
       || !reader.pod(level)
       || !reader.pod(wet1)
       || !reader.pod(wet2)
       || !reader.pod(width)
       || !reader.pod(gain))
    {
        return false;
    }

    for(int i = 0; i < numcombs; ++i)
    {
        if(!fluid_reverb_state::read_comb(reader, combL[i])
           || !fluid_reverb_state::read_comb(reader, combR[i]))
        {
            return false;
        }
    }

    for(int i = 0; i < numallpasses; ++i)
    {
        if(!fluid_reverb_state::read_allpass(reader, allpassL[i])
           || !fluid_reverb_state::read_allpass(reader, allpassR[i]))
        {
            return false;
        }
    }

    return reader.at_end();
}

bool
fluid_revmodel_lexverb::save_state(std::vector<unsigned char> &data) const
{
    Writer writer;

    write_tag(writer, ENGINE_LEXVERB);
    writer.pod(roomsize);
    writer.pod(damp);
    writer.pod(level);
    writer.pod(wet1);
    writer.pod(wet2);
    writer.pod(width);
    writer.pod(cached_sample_rate);

    for(int i = 0; i < NUM_OF_AP_SECTS; ++i)
    {
        fluid_reverb_state::write_allpass(writer, ap[i]);
    }

    for(int i = 0; i < NUM_OF_DELAY_SECTS; ++i)
    {
        fluid_reverb_state::write_delay_line(writer, dl[i]);
    }

    data.swap(writer.data);
    return true;
}

bool
fluid_revmodel_lexverb::restore_state(const std::vector<unsigned char> &data)
{
    Reader reader(data);

    if(!read_tag(reader, ENGINE_LEXVERB)
       || !reader.pod(roomsize)
       || !reader.pod(damp)
       || !reader.pod(level)
       || !reader.pod(wet1)
       || !reader.pod(wet2)
       || !reader.pod(width)
       || !reader.pod(cached_sample_rate))
    {
        return false;
    }

    for(int i = 0; i < NUM_OF_AP_SECTS; ++i)
    {
        if(!fluid_reverb_state::read_allpass(reader, ap[i]))
        {
            return false;
        }
    }

    for(int i = 0; i < NUM_OF_DELAY_SECTS; ++i)
    {
        if(!fluid_reverb_state::read_delay_line(reader, dl[i]))
        {
            return false;
        }
    }

    return reader.at_end();
}

bool
fluid_revmodel_dattorro::save_state(std::vector<unsigned char> &data) const
{
    Writer writer;

    write_tag(writer, ENGINE_DATTORRO);
    writer.pod(roomsize);
    writer.pod(damp);
    writer.pod(level);
    writer.pod(wet1);
    writer.pod(wet2);
    writer.pod(width);
    writer.pod(bandwidth);
    writer.pod(decay);
    writer.pod(cached_sample_rate);
    fluid_reverb_state::write_delay_line(writer, predelay);

    for(int i = 0; i < 4; ++i)
    {
        fluid_reverb_state::write_allpass(writer, input_ap[i]);
        fluid_reverb_state::write_allpass(writer, tank_ap[i]);
        fluid_reverb_state::write_delay_line(writer, tank_delay[i]);
    }

    for(size_t i = 0; i < taps.size(); ++i)
    {
        writer.pod(taps[i]);
    }

    data.swap(writer.data);
    return true;
}

bool
fluid_revmodel_dattorro::restore_state(const std::vector<unsigned char> &data)
{
    Reader reader(data);

    if(!read_tag(reader, ENGINE_DATTORRO)
       || !reader.pod(roomsize)
       || !reader.pod(damp)
       || !reader.pod(level)
       || !reader.pod(wet1)
       || !reader.pod(wet2)
       || !reader.pod(width)
       || !reader.pod(bandwidth)
       || !reader.pod(decay)
       || !reader.pod(cached_sample_rate)
       || !fluid_reverb_state::read_delay_line(reader, predelay))
    {
        return false;
    }

    for(int i = 0; i < 4; ++i)
    {
        if(!fluid_reverb_state::read_allpass(reader, input_ap[i])
           || !fluid_reverb_state::read_allpass(reader, tank_ap[i])
           || !fluid_reverb_state::read_delay_line(reader, tank_delay[i]))
        {
            return false;
        }
    }

    for(size_t i = 0; i < taps.size(); ++i)
    {
        if(!reader.pod(taps[i]))
        {
            return false;
        }
    }

    return reader.at_end();
}

#ifdef SIGNALSMITH_SUPPORT
bool
fluid_revmodel_signalsmith::save_state(std::vector<unsigned char> &) const
{
    return false;
}

bool
fluid_revmodel_signalsmith::restore_state(const std::vector<unsigned char> &)
{
    return false;
}
#endif
