#include "test.h"
#include "fluidsynth.h"
#include "fluid_midi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define FLUID_TEST_HAS_ASAN 1
#else
#define FLUID_TEST_HAS_ASAN 0
#endif

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum
{
    CASE_COUNT = 200,
    RENDER_FRAMES = 256,
    RENDER_BLOCKS = 4,
    CHILD_NONFINITE = 20,
    CHILD_SETUP_ERROR = 21,
    ASAN_ERROR = 97,
    LSAN_ERROR = 98
};

enum class Anomaly
{
    SETUP,
    TRUNCATION,
    INVALID_CONTROLLER,
    POLYPHONY,
    CYCLE_REFERENCE
};

enum class OpKind
{
    MIDI_BYTES,
    DIRECT_CC,
    SET_POLYPHONY
};

struct Op
{
    OpKind kind;
    Anomaly anomaly;
    std::vector<unsigned char> bytes;
    int arg0;
    int arg1;
    int arg2;

    Op() : kind(OpKind::MIDI_BYTES), anomaly(Anomaly::SETUP), arg0(0), arg1(0), arg2(0) {}
};

struct Rng
{
    std::uint32_t state;

    explicit Rng(std::uint32_t seed) : state(seed ? seed : 0x6d2b79f5u) {}

    std::uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    int range(int limit)
    {
        return limit > 0 ? static_cast<int>(next() % static_cast<std::uint32_t>(limit)) : 0;
    }
};

enum class Outcome
{
    PASS,
    CRASH,
    NONFINITE,
    LEAK,
    HARNESS_ERROR
};

struct RunResult
{
    Outcome outcome;
    int raw_status;
    std::string diagnostic;

    RunResult() : outcome(Outcome::PASS), raw_status(0) {}
};

struct Counterexample
{
    int case_id;
    std::vector<Op> original;
    std::vector<Op> minimal;
    RunResult result;
};

static Op midi_op(Anomaly anomaly, const std::vector<unsigned char> &bytes)
{
    Op op;
    op.kind = OpKind::MIDI_BYTES;
    op.anomaly = anomaly;
    op.bytes = bytes;
    return op;
}

static Op midi_op(Anomaly anomaly, unsigned char b0)
{
    return midi_op(anomaly, std::vector<unsigned char>(1, b0));
}

static Op midi_op(Anomaly anomaly, unsigned char b0, unsigned char b1)
{
    std::vector<unsigned char> bytes;
    bytes.push_back(b0);
    bytes.push_back(b1);
    return midi_op(anomaly, bytes);
}

static Op midi_op(Anomaly anomaly, unsigned char b0, unsigned char b1, unsigned char b2)
{
    std::vector<unsigned char> bytes;
    bytes.push_back(b0);
    bytes.push_back(b1);
    bytes.push_back(b2);
    return midi_op(anomaly, bytes);
}

static Op direct_cc_op(int channel, int control, int value)
{
    Op op;
    op.kind = OpKind::DIRECT_CC;
    op.anomaly = Anomaly::INVALID_CONTROLLER;
    op.arg0 = channel;
    op.arg1 = control;
    op.arg2 = value;
    return op;
}

static Op polyphony_op(int value)
{
    Op op;
    op.kind = OpKind::SET_POLYPHONY;
    op.anomaly = Anomaly::POLYPHONY;
    op.arg0 = value;
    return op;
}

static Op make_truncated(Rng &rng, int channel)
{
    const unsigned char status = static_cast<unsigned char>(0x80 | (channel & 0x0f));
    switch(rng.range(7))
    {
    case 0:
        return midi_op(Anomaly::TRUNCATION, static_cast<unsigned char>(status | 0x10));

    case 1:
        return midi_op(Anomaly::TRUNCATION,
                       static_cast<unsigned char>(status | 0x10),
                       static_cast<unsigned char>(rng.range(128)));

    case 2:
        return midi_op(Anomaly::TRUNCATION, static_cast<unsigned char>(status | 0x20));

    case 3:
        return midi_op(Anomaly::TRUNCATION,
                       static_cast<unsigned char>(status | 0x20),
                       static_cast<unsigned char>(rng.range(128)));

    case 4:
        return midi_op(Anomaly::TRUNCATION, static_cast<unsigned char>(status | 0x30));

    case 5:
        return midi_op(Anomaly::TRUNCATION, static_cast<unsigned char>(status | 0x40));

    default:
    {
        std::vector<unsigned char> bytes;
        bytes.push_back(0xf0);
        const int length = 1 + rng.range(5);
        for(int i = 0; i < length; ++i)
        {
            bytes.push_back(static_cast<unsigned char>(rng.range(128)));
        }
        return midi_op(Anomaly::TRUNCATION, bytes);
    }
    }
}

static Op make_invalid_cc(Rng &rng, int channel)
{
    if(rng.range(4) == 0)
    {
        static const int invalid_controls[] = {-1, 128, 255, 1024, 0x7fffffff};
        static const int invalid_values[] = {-1, 128, 255, 0x7fffffff};
        const int control = invalid_controls[rng.range(static_cast<int>(FLUID_N_ELEMENTS(invalid_controls)))];
        const int value = invalid_values[rng.range(static_cast<int>(FLUID_N_ELEMENTS(invalid_values)))];
        return direct_cc_op(channel, control, value);
    }

    const unsigned char status = static_cast<unsigned char>(0xb0 | (channel & 0x0f));
    switch(rng.range(6))
    {
    case 0:
        return midi_op(Anomaly::INVALID_CONTROLLER, status, 121, 127);

    case 1:
        return midi_op(Anomaly::INVALID_CONTROLLER, status, 123, 0);

    case 2:
        return midi_op(Anomaly::INVALID_CONTROLLER, status, 126, 127);

    case 3:
        return midi_op(Anomaly::INVALID_CONTROLLER, status, 127, 127);

    case 4:
    {
        std::vector<unsigned char> bytes;
        bytes.push_back(status);
        bytes.push_back(99);
        bytes.push_back(static_cast<unsigned char>(rng.range(128)));
        bytes.push_back(status);
        bytes.push_back(98);
        bytes.push_back(static_cast<unsigned char>(rng.range(128)));
        bytes.push_back(status);
        bytes.push_back(6);
        bytes.push_back(static_cast<unsigned char>(rng.range(128)));
        bytes.push_back(status);
        bytes.push_back(38);
        bytes.push_back(static_cast<unsigned char>(rng.range(128)));
        return midi_op(Anomaly::INVALID_CONTROLLER, bytes);
    }

    default:
        return midi_op(Anomaly::INVALID_CONTROLLER, status,
                       static_cast<unsigned char>(120 + rng.range(8)),
                       static_cast<unsigned char>(rng.range(128)));
    }
}

static void append_polyphony_burst(std::vector<Op> &ops, Rng &rng, int case_id, int round, int channel)
{
    const int key_bias = rng.range(128);
    int count = 2 + rng.range(6);

    if(round == 0 && case_id % 64 == 0)
    {
        ops.push_back(polyphony_op(65535));
        count = 128;
    }
    else if(round == 0 && case_id % 16 == 0)
    {
        count = 32;
    }

    for(int i = 0; i < count; ++i)
    {
        const int note_channel = (channel + i) % 16;
        const int key = (key_bias + i * 37) & 0x7f;
        const int velocity = 1 + rng.range(127);
        ops.push_back(midi_op(Anomaly::POLYPHONY,
                              static_cast<unsigned char>(0x90 | note_channel),
                              static_cast<unsigned char>(key),
                              static_cast<unsigned char>(velocity)));
    }

    if(round == 0 && case_id % 64 == 0)
    {
        ops.push_back(polyphony_op(4));
    }
}

static void append_cycle_reference(std::vector<Op> &ops, Rng &rng, int channel)
{
    const int key = 12 + rng.range(108);
    const int other_key = 12 + rng.range(108);
    const unsigned char note_on = static_cast<unsigned char>(0x90 | (channel & 0x0f));
    const unsigned char note_off = static_cast<unsigned char>(0x80 | (channel & 0x0f));

    ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE, note_on,
                          static_cast<unsigned char>(key),
                          static_cast<unsigned char>(1 + rng.range(127))));

    for(int i = 0; i < 5; ++i)
    {
        const int cycle_key = (i & 1) ? key : other_key;
        ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE,
                              static_cast<unsigned char>(cycle_key),
                              static_cast<unsigned char>(1 + rng.range(127))));
    }

    ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE, note_off,
                          static_cast<unsigned char>(key), 0));

    for(int i = 0; i < 3; ++i)
    {
        ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE,
                              static_cast<unsigned char>(key), 0));
    }

    if(rng.range(5) == 0)
    {
        ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE, 0xf0, 0x7e));
        ops.push_back(midi_op(Anomaly::CYCLE_REFERENCE, 0xff));
    }
}

static std::vector<Op> generate_case(int case_id)
{
    Rng rng(0x9e3779b9u ^ (static_cast<std::uint32_t>(case_id) * 0x85ebca6bu));
    const int channel = case_id % 16;
    std::vector<Op> ops;

    if(case_id % 64 == 0)
    {
        ops.push_back(polyphony_op(65535));
    }
    else if(case_id % 32 == 0)
    {
        ops.push_back(polyphony_op(0));
    }
    else if(case_id % 32 == 1)
    {
        ops.push_back(polyphony_op(65536));
    }
    else
    {
        ops.push_back(polyphony_op(8 + case_id % 17));
    }

    for(int round = 0; round < 6; ++round)
    {
        const int rotation = (case_id + round) % 4;
        for(int step = 0; step < 4; ++step)
        {
            switch((rotation + step) % 4)
            {
            case 0:
                ops.push_back(make_truncated(rng, channel));
                if(rng.range(3) == 0)
                {
                    ops.push_back(make_truncated(rng, (channel + 1) % 16));
                }
                break;

            case 1:
                ops.push_back(make_invalid_cc(rng, channel));
                if(rng.range(3) == 0)
                {
                    ops.push_back(direct_cc_op(16, 1, 64));
                }
                break;

            case 2:
                append_polyphony_burst(ops, rng, case_id, round, channel);
                break;

            default:
                append_cycle_reference(ops, rng, channel);
                break;
            }
        }
    }

    return ops;
}

static const char *outcome_name(Outcome outcome)
{
    switch(outcome)
    {
    case Outcome::PASS:
        return "pass";
    case Outcome::CRASH:
        return "crash/sanitizer";
    case Outcome::NONFINITE:
        return "non-finite";
    case Outcome::LEAK:
        return "leak";
    default:
        return "harness-error";
    }
}

static const char *anomaly_name(Anomaly anomaly)
{
    switch(anomaly)
    {
    case Anomaly::SETUP:
        return "setup";
    case Anomaly::TRUNCATION:
        return "truncation";
    case Anomaly::INVALID_CONTROLLER:
        return "invalid-controller";
    case Anomaly::POLYPHONY:
        return "huge-polyphony";
    default:
        return "cycle-reference";
    }
}

static bool render_and_check(fluid_synth_t *synth, int block, std::string &detail)
{
    float left[RENDER_FRAMES];
    float right[RENDER_FRAMES];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));

    if(fluid_synth_write_float(synth, RENDER_FRAMES, left, 0, 1, right, 0, 1) != FLUID_OK)
    {
        detail = "fluid_synth_write_float failed";
        return false;
    }

    for(int i = 0; i < RENDER_FRAMES; ++i)
    {
        if(!std::isfinite(left[i]) || !std::isfinite(right[i]))
        {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer),
                          "non-finite sample block=%d frame=%d left=%.9g right=%.9g",
                          block, i, static_cast<double>(left[i]), static_cast<double>(right[i]));
            detail = buffer;
            return false;
        }
    }

    return true;
}

static int evaluate_case(const std::vector<Op> &ops)
{
    fluid_settings_t *settings = new_fluid_settings();
    if(settings == NULL)
    {
        std::fprintf(stderr, "new_fluid_settings failed\n");
        return CHILD_SETUP_ERROR;
    }

    fluid_settings_setint(settings, "synth.reverb.active", 0);
    fluid_settings_setint(settings, "synth.chorus.active", 0);
    fluid_settings_setint(settings, "synth.polyphony", 64);

    fluid_synth_t *synth = new_fluid_synth(settings);
    if(synth == NULL)
    {
        delete_fluid_settings(settings);
        std::fprintf(stderr, "new_fluid_synth failed\n");
        return CHILD_SETUP_ERROR;
    }

    if(fluid_synth_sfload(synth, TEST_SOUNDFONT, 1) == FLUID_FAILED)
    {
        delete_fluid_synth(synth);
        delete_fluid_settings(settings);
        std::fprintf(stderr, "fluid_synth_sfload(%s) failed\n", TEST_SOUNDFONT);
        return CHILD_SETUP_ERROR;
    }

    fluid_midi_parser_t *parser = new_fluid_midi_parser();
    if(parser == NULL)
    {
        delete_fluid_synth(synth);
        delete_fluid_settings(settings);
        std::fprintf(stderr, "new_fluid_midi_parser failed\n");
        return CHILD_SETUP_ERROR;
    }

    std::string detail;
    int render_block = 0;
    int status = 0;

    for(std::size_t i = 0; i < ops.size(); ++i)
    {
        const Op &op = ops[i];
        switch(op.kind)
        {
        case OpKind::MIDI_BYTES:
            for(std::size_t j = 0; j < op.bytes.size(); ++j)
            {
                fluid_midi_event_t *event = fluid_midi_parser_parse(parser, op.bytes[j]);
                if(event != NULL)
                {
                    fluid_synth_handle_midi_event(synth, event);
                }
            }
            break;

        case OpKind::DIRECT_CC:
            fluid_synth_cc(synth, op.arg0, op.arg1, op.arg2);
            break;

        case OpKind::SET_POLYPHONY:
            fluid_synth_set_polyphony(synth, op.arg0);
            break;
        }

        if((i + 1) % 12 == 0)
        {
            if(!render_and_check(synth, render_block++, detail))
            {
                status = CHILD_NONFINITE;
                break;
            }
        }
    }

    if(status == 0)
    {
        for(int i = 0; i < RENDER_BLOCKS; ++i)
        {
            if(!render_and_check(synth, render_block++, detail))
            {
                status = CHILD_NONFINITE;
                break;
            }
        }
    }

    if(status != 0)
    {
        std::fprintf(stderr, "%s\n", detail.c_str());
    }

    delete_fluid_midi_parser(parser);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    return status;
}

#ifndef _WIN32
static std::string read_all(int fd)
{
    std::string result;
    char buffer[4096];
    for(;;)
    {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if(count > 0)
        {
            result.append(buffer, static_cast<std::size_t>(count));
        }
        else if(count == 0)
        {
            break;
        }
        else if(errno != EINTR)
        {
            break;
        }
    }
    return result;
}
#endif

#ifndef _WIN32
static RunResult classify_child(int status, const std::string &diagnostic)
{
    RunResult result;
    result.raw_status = status;
    result.diagnostic = diagnostic;

    if(diagnostic.find("LeakSanitizer") != std::string::npos)
    {
        result.outcome = Outcome::LEAK;
        return result;
    }

    if(WIFSIGNALED(status))
    {
        result.outcome = Outcome::CRASH;
        return result;
    }

    if(!WIFEXITED(status))
    {
        result.outcome = Outcome::HARNESS_ERROR;
        return result;
    }

    switch(WEXITSTATUS(status))
    {
    case 0:
        result.outcome = Outcome::PASS;
        break;

    case CHILD_NONFINITE:
        result.outcome = Outcome::NONFINITE;
        break;

    case CHILD_SETUP_ERROR:
        result.outcome = Outcome::HARNESS_ERROR;
        break;

    case LSAN_ERROR:
        result.outcome = Outcome::LEAK;
        break;

    case ASAN_ERROR:
        result.outcome = Outcome::CRASH;
        break;

    default:
        result.outcome = Outcome::CRASH;
        break;
    }

    return result;
}
#endif

static RunResult run_isolated(const std::vector<Op> &ops)
{
#ifndef _WIN32
    char path[] = "/tmp/fluid-malformed-midi-XXXXXX";
    const int fd = mkstemp(path);
    if(fd < 0)
    {
        RunResult result;
        result.outcome = Outcome::HARNESS_ERROR;
        result.raw_status = -1;
        result.diagnostic = "mkstemp failed";
        return result;
    }

    const pid_t pid = fork();
    if(pid < 0)
    {
        close(fd);
        unlink(path);
        RunResult result;
        result.outcome = Outcome::HARNESS_ERROR;
        result.raw_status = -1;
        result.diagnostic = "fork failed";
        return result;
    }

    if(pid == 0)
    {
        if(dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
        {
            _exit(CHILD_SETUP_ERROR);
        }
        close(fd);
        const int status = evaluate_case(ops);
        std::fflush(NULL);
        std::exit(status);
    }

    close(fd);

    int status = 0;
    pid_t waited;
    do
    {
        waited = waitpid(pid, &status, 0);
    }
    while(waited < 0 && errno == EINTR);

    if(waited < 0)
    {
        unlink(path);
        RunResult result;
        result.outcome = Outcome::HARNESS_ERROR;
        result.raw_status = -1;
        result.diagnostic = "waitpid failed";
        return result;
    }

    const int output_fd = open(path, O_RDONLY);
    std::string diagnostic;
    if(output_fd >= 0)
    {
        diagnostic = read_all(output_fd);
        close(output_fd);
    }
    unlink(path);
    return classify_child(status, diagnostic);
#else
    const int status = evaluate_case(ops);
    RunResult result;
    result.raw_status = status;
    result.outcome = status == 0 ? Outcome::PASS
                     : status == CHILD_NONFINITE ? Outcome::NONFINITE
                     : Outcome::HARNESS_ERROR;
    return result;
#endif
}

static bool same_failure(const RunResult &a, const RunResult &b)
{
    return a.outcome != Outcome::PASS && a.outcome == b.outcome;
}

static std::vector<Op> without_range(const std::vector<Op> &ops, std::size_t begin, std::size_t end)
{
    std::vector<Op> result;
    result.reserve(ops.size() - (end - begin));
    result.insert(result.end(), ops.begin(), ops.begin() + begin);
    result.insert(result.end(), ops.begin() + end, ops.end());
    return result;
}

static std::vector<Op> minimize(const std::vector<Op> &original, const RunResult &failure)
{
    std::vector<Op> current = original;

    while(current.size() >= 2)
    {
        std::size_t chunks = 2;
        bool reduced = false;

        while(chunks <= current.size())
        {
            const std::size_t chunk_size = (current.size() + chunks - 1) / chunks;
            for(std::size_t begin = 0; begin < current.size(); begin += chunk_size)
            {
                const std::size_t end = std::min(begin + chunk_size, current.size());
                std::vector<Op> candidate = without_range(current, begin, end);
                if(same_failure(failure, run_isolated(candidate)))
                {
                    current.swap(candidate);
                    reduced = true;
                    break;
                }
            }

            if(reduced)
            {
                break;
            }
            if(chunks == current.size())
            {
                break;
            }
            chunks = std::min(current.size(), chunks * 2);
        }

        if(!reduced)
        {
            break;
        }
    }

    for(std::size_t i = 0; i < current.size();)
    {
        std::vector<Op> candidate = without_range(current, i, i + 1);
        if(same_failure(failure, run_isolated(candidate)))
        {
            current.swap(candidate);
        }
        else
        {
            ++i;
        }
    }

    return current;
}

static void print_sequence(const std::vector<Op> &ops)
{
    if(ops.empty())
    {
        std::printf("  <empty sequence>\n");
        return;
    }

    for(std::size_t i = 0; i < ops.size(); ++i)
    {
        const Op &op = ops[i];
        std::printf("  [%03zu] %-18s ", i, anomaly_name(op.anomaly));
        if(op.kind == OpKind::MIDI_BYTES)
        {
            std::printf("midi");
            for(std::size_t j = 0; j < op.bytes.size(); ++j)
            {
                std::printf(" %02x", op.bytes[j]);
            }
            std::printf("\n");
        }
        else if(op.kind == OpKind::DIRECT_CC)
        {
            std::printf("cc channel=%d control=%d value=%d\n", op.arg0, op.arg1, op.arg2);
        }
        else
        {
            std::printf("polyphony=%d\n", op.arg0);
        }
    }
}

static bool duplicate_counterexample(const std::vector<Counterexample> &counterexamples,
                                     const Counterexample &candidate)
{
    for(std::size_t i = 0; i < counterexamples.size(); ++i)
    {
        const Counterexample &other = counterexamples[i];
        if(other.result.outcome != candidate.result.outcome)
        {
            continue;
        }

        if(other.minimal.size() != candidate.minimal.size())
        {
            continue;
        }

        bool equal = true;
        for(std::size_t j = 0; j < candidate.minimal.size(); ++j)
        {
            const Op &a = other.minimal[j];
            const Op &b = candidate.minimal[j];
            if(a.kind != b.kind || a.arg0 != b.arg0 || a.arg1 != b.arg1 ||
               a.arg2 != b.arg2 || a.bytes != b.bytes)
            {
                equal = false;
                break;
            }
        }

        if(equal)
        {
            return true;
        }
    }
    return false;
}

int main(void)
{
    TEST_DISABLE_STDOUT_BUFFERING();

    int counts[5] = {0, 0, 0, 0, 0};
    std::size_t total_ops = 0;
    std::vector<Counterexample> counterexamples;

    std::printf("malformed MIDI property test: %d cases\n", CASE_COUNT);

    for(int case_id = 0; case_id < CASE_COUNT; ++case_id)
    {
        std::vector<Op> ops = generate_case(case_id);
        total_ops += ops.size();
        RunResult result = run_isolated(ops);
        counts[static_cast<int>(result.outcome)]++;

        if(result.outcome != Outcome::PASS)
        {
            Counterexample example;
            example.case_id = case_id;
            example.original = ops;
            example.minimal = minimize(ops, result);
            example.result = run_isolated(example.minimal);

            std::printf("FAIL case=%d outcome=%s original_ops=%zu minimal_ops=%zu\n",
                        case_id, outcome_name(example.result.outcome),
                        example.original.size(), example.minimal.size());
            print_sequence(example.minimal);
            if(!example.result.diagnostic.empty())
            {
                std::printf("  diagnostic:\n%s\n", example.result.diagnostic.c_str());
            }

            if(!duplicate_counterexample(counterexamples, example))
            {
                counterexamples.push_back(example);
            }
        }
    }

    std::printf("summary: cases=%d operations=%zu pass=%d crash=%d nonfinite=%d leak=%d harness=%d\n",
                CASE_COUNT, total_ops,
                counts[static_cast<int>(Outcome::PASS)],
                counts[static_cast<int>(Outcome::CRASH)],
                counts[static_cast<int>(Outcome::NONFINITE)],
                counts[static_cast<int>(Outcome::LEAK)],
                counts[static_cast<int>(Outcome::HARNESS_ERROR)]);

#if FLUID_TEST_HAS_ASAN
    std::printf("leak-check: AddressSanitizer/LeakSanitizer enabled per case\n");
#else
    std::printf("leak-check: unavailable in this build; use -fsanitize=address for LSan\n");
#endif

    std::printf("counterexamples: %zu unique\n", counterexamples.size());
    if(!counterexamples.empty())
    {
        for(std::size_t i = 0; i < counterexamples.size(); ++i)
        {
            const Counterexample &example = counterexamples[i];
            std::printf("counterexample %zu: case=%d outcome=%s\n",
                        i + 1, example.case_id, outcome_name(example.result.outcome));
            print_sequence(example.minimal);
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
