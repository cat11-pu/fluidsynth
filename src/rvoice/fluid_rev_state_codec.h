/*
 * FluidSynth - A Software Synthesizer
 *
 * Internal binary state codec used by reverb engine snapshots.
 */

#pragma once

#include "fluid_rev_filters.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace fluid_reverb_state
{

class Writer
{
public:
    std::vector<unsigned char> data;

    void bytes(const void *src, size_t size)
    {
        const unsigned char *p = static_cast<const unsigned char *>(src);
        data.insert(data.end(), p, p + size);
    }

    template<typename T>
    void pod(const T &value)
    {
        bytes(&value, sizeof(value));
    }

    void count(size_t value)
    {
        uint32_t value32 = static_cast<uint32_t>(value);
        pod(value32);
    }
};

class Reader
{
public:
    Reader(const std::vector<unsigned char> &source)
        : data(source), pos(0)
    {
    }

    bool bytes(void *dst, size_t size)
    {
        if(size > data.size() - pos)
        {
            return false;
        }

        std::memcpy(dst, data.data() + pos, size);
        pos += size;
        return true;
    }

    template<typename T>
    bool pod(T &value)
    {
        return bytes(&value, sizeof(value));
    }

    bool count(size_t &value, size_t maximum)
    {
        uint32_t value32;

        if(!pod(value32) || value32 > maximum)
        {
            return false;
        }

        value = value32;
        return true;
    }

    bool at_end() const
    {
        return pos == data.size();
    }

private:
    const std::vector<unsigned char> &data;
    size_t pos;
};

template<typename SampleT, typename DampingT>
void write_delay_line(Writer &writer,
                      const fluid_reverb_delay_line<SampleT, DampingT> &delay)
{
    writer.count(delay.line.size());

    if(!delay.line.empty())
    {
        writer.bytes(delay.line.data(), delay.line.size() * sizeof(SampleT));
    }

    writer.pod(delay.line_in);
    writer.pod(delay.line_out);
    writer.pod(delay.damping.buffer);
    writer.pod(delay.damping.b0);
    writer.pod(delay.damping.a1);
    writer.pod(delay.coefficient);
    writer.pod(delay.last_output);
}

template<typename SampleT, typename DampingT>
bool read_delay_line(Reader &reader,
                     fluid_reverb_delay_line<SampleT, DampingT> &delay)
{
    size_t count;

    if(!reader.count(count, 1u << 22))
    {
        return false;
    }

    delay.line.resize(count);

    if(count > 0 && !reader.bytes(delay.line.data(), count * sizeof(SampleT)))
    {
        return false;
    }

    return reader.pod(delay.line_in)
           && reader.pod(delay.line_out)
           && reader.pod(delay.damping.buffer)
           && reader.pod(delay.damping.b0)
           && reader.pod(delay.damping.a1)
           && reader.pod(delay.coefficient)
           && reader.pod(delay.last_output);
}

template<typename SampleT, enum fluid_reverb_allpass_mode Mode>
void write_allpass(Writer &writer,
                   const fluid_reverb_allpass<SampleT, Mode> &allpass)
{
    writer.pod(allpass.feedback);
    write_delay_line(writer, allpass.delay);
    writer.pod(allpass.last_output);
}

template<typename SampleT, enum fluid_reverb_allpass_mode Mode>
bool read_allpass(Reader &reader,
                  fluid_reverb_allpass<SampleT, Mode> &allpass)
{
    return reader.pod(allpass.feedback)
           && read_delay_line(reader, allpass.delay)
           && reader.pod(allpass.last_output);
}

template<typename SampleT>
void write_comb(Writer &writer, const fluid_reverb_comb<SampleT> &comb)
{
    writer.pod(comb.feedback);
    writer.pod(comb.filterstore);
    writer.pod(comb.damp1);
    writer.pod(comb.damp2);
    write_delay_line(writer, comb.delay);
}

template<typename SampleT>
bool read_comb(Reader &reader, fluid_reverb_comb<SampleT> &comb)
{
    return reader.pod(comb.feedback)
           && reader.pod(comb.filterstore)
           && reader.pod(comb.damp1)
           && reader.pod(comb.damp2)
           && read_delay_line(reader, comb.delay);
}

} // namespace fluid_reverb_state
