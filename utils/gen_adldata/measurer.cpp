#include "measurer.h"
#include "file_formats/common.h"
#include <cmath>

#ifdef GEN_ADLDATA_DEEP_DEBUG
#include "../midiplay/wave_writer.h"
#endif

#ifndef M_PI
#define M_PI    3.14159265358979323846
#endif

// Nuked OPL3 emulator, Most accurate, but requires the powerful CPU
#ifndef ADLMIDI_DISABLE_NUKED_EMULATOR
#   include "../../src/chips/nuked_opl3.h"
#   include "../../src/chips/nuked_opl3_v174.h"
#endif

// DosBox 0.74 OPL3 emulator, Well-accurate and fast
#ifndef ADLMIDI_DISABLE_DOSBOX_EMULATOR
#    include "../../src/chips/dosbox_opl3.h"
#endif

#define NUM_OF_CHANNELS                 23
#define NUM_OF_RM_CHANNELS              5

//! Per-channel and per-operator registers map
static const uint16_t g_operatorsMap[(NUM_OF_CHANNELS + NUM_OF_RM_CHANNELS) * 2] =
{
    // Channels 0-2
    0x000, 0x003, 0x001, 0x004, 0x002, 0x005, // operators  0, 3,  1, 4,  2, 5
    // Channels 3-5
    0x008, 0x00B, 0x009, 0x00C, 0x00A, 0x00D, // operators  6, 9,  7,10,  8,11
    // Channels 6-8
    0x010, 0x013, 0x011, 0x014, 0x012, 0x015, // operators 12,15, 13,16, 14,17
    // Same for second card
    0x100, 0x103, 0x101, 0x104, 0x102, 0x105, // operators 18,21, 19,22, 20,23
    0x108, 0x10B, 0x109, 0x10C, 0x10A, 0x10D, // operators 24,27, 25,28, 26,29
    0x110, 0x113, 0x111, 0x114, 0x112, 0x115, // operators 30,33, 31,34, 32,35

    //==For Rhythm-mode percussions
    // Channel 18
    0x010, 0x013,  // operators 12,15
    // Channel 19
    0xFFF, 0x014,  // operator 16
    // Channel 19
    0x012, 0xFFF,  // operator 14
    // Channel 19
    0xFFF, 0x015,  // operator 17
    // Channel 19
    0x011, 0xFFF,  // operator 13

    //==For Rhythm-mode percussions in CMF, snare and cymbal operators has inverted!
    0x010, 0x013,  // operators 12,15
    // Channel 19
    0x014, 0xFFF,  // operator 16
    // Channel 19
    0x012, 0xFFF,  // operator 14
    // Channel 19
    0x015, 0xFFF,  // operator 17
    // Channel 19
    0x011, 0xFFF   // operator 13
};

//! Channel map to regoster offsets
static const uint16_t g_channelsMap[NUM_OF_CHANNELS] =
{
    0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, // 0..8
    0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, // 9..17 (secondary set)
    0x006, 0x007, 0x008, 0x008, 0x008 // <- hw percussions, hihats and cymbals using tom-tom's channel as pitch source
};


template <class T>
class AudioHistory
{
    std::unique_ptr<T[]> m_data;
    size_t m_index = 0;  // points to the next write slot
    size_t m_length = 0;
    size_t m_capacity = 0;

public:
    size_t size() const { return m_length; }
    size_t capacity() const { return m_capacity; }
    const T *data() const { return &m_data[m_index + m_capacity - m_length]; }

    void reset(size_t capacity)
    {
        m_data.reset(new T[2 * capacity]());
        m_index = 0;
        m_length = 0;
        m_capacity = capacity;
    }

    void clear()
    {
        m_length = 0;
    }

    void add(const T &item)
    {
        T *data = m_data.get();
        const size_t capacity = m_capacity;
        size_t index = m_index;
        data[index] = item;
        data[index + capacity] = item;
        m_index = (index + 1 != capacity) ? (index + 1) : 0;
        size_t length = m_length + 1;
        m_length = (length < capacity) ? length : capacity;
    }
};

static void HannWindow(double *w, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        w[i] = 0.5 * (1.0 - std::cos(2 * M_PI * i / (n - 1)));
}

static double MeasureRMS(const double *signal, const double *window, unsigned length)
{
    double mean = 0;
#pragma omp simd reduction(+: mean)
    for(unsigned i = 0; i < length; ++i)
        mean += window[i] * signal[i];
    mean /= length;

    double rms = 0;
#pragma omp simd reduction(+: rms)
    for(unsigned i = 0; i < length; ++i)
    {
        double diff = window[i] * signal[i] - mean;
        rms += diff * diff;
    }
    rms = std::sqrt(rms / (length - 1));

    return rms;
}

static const unsigned g_outputRate = 49716;

struct TinySynth
{
    OPLChipBase *m_chip;
    unsigned m_notesNum;
    unsigned m_actualNotesNum;
    bool m_isReal4op;
    bool m_isPseudo4op;
    int m_playNoteNum;
    int8_t m_voice1Detune;
    int16_t m_noteOffsets[2];
    unsigned m_x[2];

    bool m_isSilentGuess;

    void resetChip()
    {
        static const short initdata[] =
        {
            0x004, 96, 0x004, 128,      // Pulse timer
            0x105, 0, 0x105, 1, 0x105, 0, // Pulse OPL3 enable, leave disabled
            0x001, 32, 0x0BD, 0         // Enable wave & melodic
        };

        m_chip->setRate(g_outputRate);

        for(size_t a = 0; a < 18; ++a)
            m_chip->writeReg(0xB0 + g_channelsMap[a], 0x00);
        for(unsigned a = 0; a < sizeof(initdata) / sizeof(*initdata); a += 2)
            m_chip->writeReg((uint16_t)initdata[a], (uint8_t)initdata[a + 1]);
    }

    void setInstrument(const ins &in)
    {
        insdata rawData[2];
        bool found[2] = {false, false};
        for(InstrumentDataTab::const_iterator j = insdatatab.begin();
            j != insdatatab.end();
            ++j)
        {
            if(j->second.first == in.insno1)
            {
                rawData[0] = j->first;
                found[0] = true;
                if(found[1]) break;
            }
            if(j->second.first == in.insno2)
            {
                rawData[1] = j->first;
                found[1] = true;
                if(found[0]) break;
            }
        }

        std::memset(m_x, 0, sizeof(m_x));
        m_playNoteNum = in.notenum >= 128 ? (in.notenum - 128) : in.notenum;
        m_isReal4op = in.real4op && !in.pseudo4op;
        m_isPseudo4op = in.pseudo4op;
        if(m_playNoteNum == 0)
            m_playNoteNum = 25;
        m_notesNum = in.insno1 == in.insno2 ? 1 : 2;
        m_actualNotesNum = (m_isReal4op ? 1 : m_notesNum);
        m_voice1Detune = 0;
        m_noteOffsets[0] = rawData[0].finetune;
        m_noteOffsets[1] = rawData[1].finetune;
        if(in.pseudo4op)
            m_voice1Detune = in.voice2_fine_tune;
        m_chip->writeReg(0x104, in.real4op ? (1 << 6) - 1 : 0x00);

        //For cleaner measurement, disable tremolo and vibrato
        rawData[0].data[0] &= 0x3F;
        rawData[0].data[1] &= 0x3F;
        rawData[1].data[0] &= 0x3F;
        rawData[1].data[1] &= 0x3F;

        for(unsigned n = 0; n < m_notesNum; ++n)
        {
            static const unsigned char patchdata[11] =
            {0x20, 0x23, 0x60, 0x63, 0x80, 0x83, 0xE0, 0xE3, 0x40, 0x43, 0xC0};
            for(unsigned a = 0; a < 10; ++a)
                m_chip->writeReg(patchdata[a] + n * 8, rawData[n].data[a]);
            m_chip->writeReg(patchdata[10] + n * 8, rawData[n].data[10] | 0x30);
        }
    }

    void setInstrument(const BanksDump &db, const BanksDump::InstrumentEntry &ins)
    {
        bool isPseudo4ops = ((ins.instFlags & BanksDump::InstrumentEntry::WOPL_Ins_Pseudo4op) != 0);
        bool is4ops =       ((ins.instFlags & BanksDump::InstrumentEntry::WOPL_Ins_4op) != 0) && !isPseudo4ops;
        size_t opsNum = (is4ops || isPseudo4ops) ? 4 : 2;
        BanksDump::Operator ops[4];
        assert(ins.ops[0] >= 0);
        assert(ins.ops[1] >= 0);
        ops[0] = db.operators[ins.ops[0]];
        ops[1] = db.operators[ins.ops[1]];
        if(opsNum > 2)
        {
            assert(ins.ops[2] >= 0);
            assert(ins.ops[3] >= 0);
            ops[2] = db.operators[ins.ops[2]];
            ops[3] = db.operators[ins.ops[3]];
        }

        std::memset(m_x, 0, sizeof(m_x));
        m_playNoteNum = ins.percussionKeyNumber >= 128 ? (ins.percussionKeyNumber - 128) : ins.percussionKeyNumber;
        m_isReal4op = is4ops;
        m_isPseudo4op = isPseudo4ops;
        if(m_playNoteNum == 0)
            m_playNoteNum = 60;
        m_notesNum = opsNum / 2;
        m_actualNotesNum = (m_isReal4op ? 1 : m_notesNum);
        m_voice1Detune = 0;
        m_noteOffsets[0] = ins.noteOffset1;
        m_noteOffsets[1] = ins.noteOffset2;
        if(isPseudo4ops)
            m_voice1Detune = ins.secondVoiceDetune;
        m_chip->writeReg(0x104, is4ops ? 0x3F : 0x00);

        //For cleaner measurement, disable tremolo and vibrato
        ops[0].d_E862 &= 0xFFFFFF3F;
        ops[1].d_E862 &= 0xFFFFFF3F;
        ops[2].d_E862 &= 0xFFFFFF3F;
        ops[3].d_E862 &= 0xFFFFFF3F;
//        rawData[0].data[0] &= 0x3F;
//        rawData[0].data[1] &= 0x3F;
//        rawData[1].data[0] &= 0x3F;
//        rawData[1].data[1] &= 0x3F;

        for(unsigned n = 0; n < m_notesNum; ++n)
        {
            static const uint8_t data[4] = {0x20, 0x60, 0x80, 0xE0};
            size_t opOffset = (n * 2);
            uint16_t o1 = g_operatorsMap[opOffset + 0];
            uint16_t o2 = g_operatorsMap[opOffset + 1];
            uint_fast32_t x1 = ops[opOffset + 0].d_E862, y1 = ops[opOffset + 1].d_E862;
            uint_fast8_t  x2 = ops[opOffset + 0].d_40,   y2 = ops[opOffset + 1].d_40;
            uint_fast8_t  fbConn = (ins.fbConn >> (n * 8)) & 0xFF;

            for(size_t a = 0; a < 4; ++a, x1 >>= 8, y1 >>= 8)
            {
                m_chip->writeReg(data[a] + o1, x1 & 0xFF);
                m_chip->writeReg(data[a] + o2, y1 & 0xFF);
            }
            m_chip->writeReg(0xC0 + (n * 8), fbConn | 0x30);
            m_chip->writeReg(0x40 + o1, x2 & 0xFF);
            m_chip->writeReg(0x40 + o2, y2 & 0xFF);
        }

//        for(unsigned n = 0; n < m_notesNum; ++n)
//        {
//            static const unsigned char patchdata[11] =
//            {0x20, 0x23, 0x60, 0x63, 0x80, 0x83, 0xE0, 0xE3, 0x40, 0x43, 0xC0};
//            for(unsigned a = 0; a < 10; ++a)
//                m_chip->writeReg(patchdata[a] + n * 8, rawData[n].data[a]);
//            m_chip->writeReg(patchdata[10] + n * 8, rawData[n].data[10] | 0x30);
//        }
    }

    void noteOn()
    {
        std::memset(m_x, 0, sizeof(m_x));
        for(unsigned n = 0; n < m_actualNotesNum; ++n)
        {
            double hertz = 172.00093 * std::exp(0.057762265 * (m_playNoteNum + m_noteOffsets[n]));
            if(hertz > 131071)
            {
                std::fprintf(stdout, "%s:%d:0: warning: Why does note %d + note-offset %d produce hertz %g?\n", __FILE__, __LINE__,
                             m_playNoteNum, m_noteOffsets[n], hertz);
                std::fflush(stdout);
                hertz = 131071;
            }
            m_x[n] = 0x2000u;
            while(hertz >= 1023.5)
            {
                hertz /= 2.0;    // Calculate octave
                m_x[n] += 0x400;
            }
            m_x[n] += (unsigned int)(hertz + 0.5);

            // Keyon the note
            m_chip->writeReg(0xA0 + (n * 3), m_x[n] & 0xFF);
            m_chip->writeReg(0xB0 + (n * 3), (m_x[n] >> 8) & 0xFF);
        }
    }

    void noteOff()
    {
        // Keyoff the note
        for(unsigned n = 0; n < m_actualNotesNum; ++n)
            m_chip->writeReg(0xB0 + (n * 3), (m_x[n] >> 8) & 0xDF);
    }

    void generate(int16_t *output, size_t frames)
    {
        m_chip->generate(output, frames);
    }
};


DurationInfo MeasureDurations(const ins &in, OPLChipBase *chip)
{
    AudioHistory<double> audioHistory;

    const unsigned interval             = 150;
    const unsigned samples_per_interval = g_outputRate / interval;

    const double historyLength = 0.1;  // maximum duration to memorize (seconds)
    audioHistory.reset(std::ceil(historyLength * g_outputRate));

    std::unique_ptr<double[]> window;
    window.reset(new double[audioHistory.capacity()]);
    unsigned winsize = 0;

    TinySynth synth;
    synth.m_chip = chip;
    synth.resetChip();
    synth.setInstrument(in);
    synth.noteOn();

    /* For capturing */
    const unsigned max_silent = 6;
    const unsigned max_on  = 40;
    const unsigned max_off = 60;

    unsigned max_period_on = max_on * interval;
    unsigned max_period_off = max_off * interval;

    const double min_coefficient_on = 0.008;
    const double min_coefficient_off = 0.2;

    unsigned windows_passed_on = 0;
    unsigned windows_passed_off = 0;

    /* For Analyze the results */
    double begin_amplitude        = 0;
    double peak_amplitude_value   = 0;
    size_t peak_amplitude_time    = 0;
    size_t quarter_amplitude_time = max_period_on;
    bool   quarter_amplitude_time_found = false;
    size_t keyoff_out_time        = 0;
    bool   keyoff_out_time_found  = false;

    const size_t audioBufferLength = 256;
    const size_t audioBufferSize = 2 * audioBufferLength;
    int16_t audioBuffer[audioBufferSize];

    // For up to 40 seconds, measure mean amplitude.
    double highest_sofar = 0;
    short sound_min = 0, sound_max = 0;

    for(unsigned period = 0; period < max_period_on; ++period, ++windows_passed_on)
    {
        for(unsigned i = 0; i < samples_per_interval;)
        {
            size_t blocksize = samples_per_interval - i;
            blocksize = (blocksize < audioBufferLength) ? blocksize : audioBufferLength;
            synth.generate(audioBuffer, blocksize);
            for (unsigned j = 0; j < blocksize; ++j)
            {
                int16_t s = audioBuffer[2 * j];
                audioHistory.add(s);
                if(sound_min > s) sound_min = s;
                if(sound_max < s) sound_max = s;
            }
            i += blocksize;
        }

        if(winsize != audioHistory.size())
        {
            winsize = audioHistory.size();
            HannWindow(window.get(), winsize);
        }

        double rms = MeasureRMS(audioHistory.data(), window.get(), winsize);
        /* ======== Peak time detection ======== */
        if(period == 0)
        {
            begin_amplitude = rms;
            peak_amplitude_value = rms;
            peak_amplitude_time = 0;
        }
        else if(rms > peak_amplitude_value)
        {
            peak_amplitude_value = rms;
            peak_amplitude_time  = period;
            // In next step, update the quater amplitude time
            quarter_amplitude_time_found = false;
        }
        else if(!quarter_amplitude_time_found && (rms <= peak_amplitude_value * min_coefficient_on))
        {
            quarter_amplitude_time = period;
            quarter_amplitude_time_found = true;
        }
        /* ======== Peak time detection =END==== */
        if(rms > highest_sofar)
            highest_sofar = rms;

        if((period > max_silent * interval) &&
           ( (rms < highest_sofar * min_coefficient_on) || (sound_min >= -1 && sound_max <= 1) )
        )
            break;
    }

    if(!quarter_amplitude_time_found)
        quarter_amplitude_time = windows_passed_on;

    if(windows_passed_on >= max_period_on)
    {
        // Just Keyoff the note
        synth.noteOff();
    }
    else
    {
        // Reset the emulator and re-run the "ON" simulation until reaching the peak time
        synth.resetChip();
        synth.setInstrument(in);
        synth.noteOn();

        audioHistory.reset(std::ceil(historyLength * g_outputRate));
        for(unsigned period = 0;
            ((period < peak_amplitude_time) || (period == 0)) && (period < max_period_on);
            ++period)
        {
            for(unsigned i = 0; i < samples_per_interval;)
            {
                size_t blocksize = samples_per_interval - i;
                blocksize = (blocksize < audioBufferLength) ? blocksize : audioBufferLength;
                synth.generate(audioBuffer, blocksize);
                for (unsigned j = 0; j < blocksize; ++j)
                    audioHistory.add(audioBuffer[2 * j]);
                i += blocksize;
            }
        }
        synth.noteOff();
    }

    // Now, for up to 60 seconds, measure mean amplitude.
    for(unsigned period = 0; period < max_period_off; ++period, ++windows_passed_off)
    {
        for(unsigned i = 0; i < samples_per_interval;)
        {
            size_t blocksize = samples_per_interval - i;
            blocksize = (blocksize < 256) ? blocksize : 256;
            synth.generate(audioBuffer, blocksize);
            for (unsigned j = 0; j < blocksize; ++j)
            {
                int16_t s = audioBuffer[2 * j];
                audioHistory.add(s);
                if(sound_min > s) sound_min = s;
                if(sound_max < s) sound_max = s;
            }
            i += blocksize;
        }

        if(winsize != audioHistory.size())
        {
            winsize = audioHistory.size();
            HannWindow(window.get(), winsize);
        }

        double rms = MeasureRMS(audioHistory.data(), window.get(), winsize);
        /* ======== Find Key Off time ======== */
        if(!keyoff_out_time_found && (rms <= peak_amplitude_value * min_coefficient_off))
        {
            keyoff_out_time = period;
            keyoff_out_time_found = true;
        }
        /* ======== Find Key Off time ==END=== */
        if(rms < highest_sofar * min_coefficient_off)
            break;

        if((period > max_silent * interval) && (sound_min >= -1 && sound_max <= 1))
            break;
    }

    DurationInfo result;
    result.peak_amplitude_time = peak_amplitude_time;
    result.peak_amplitude_value = peak_amplitude_value;
    result.begin_amplitude = begin_amplitude;
    result.quarter_amplitude_time = (double)quarter_amplitude_time;
    result.keyoff_out_time = (double)keyoff_out_time;

    result.ms_sound_kon  = (int64_t)(quarter_amplitude_time * 1000.0 / interval);
    result.ms_sound_koff = (int64_t)(keyoff_out_time        * 1000.0 / interval);
    result.nosound = (peak_amplitude_value < 0.5) || ((sound_min >= -1) && (sound_max <= 1));

    return result;
}


DurationInfo MeasureDurations(BanksDump &db, const BanksDump::InstrumentEntry &ins, OPLChipBase *chip)
{
    AudioHistory<double> audioHistory;

    const unsigned interval             = 150;
    const unsigned samples_per_interval = g_outputRate / interval;

    const double historyLength = 0.1;  // maximum duration to memorize (seconds)
    audioHistory.reset(std::ceil(historyLength * g_outputRate));

    std::unique_ptr<double[]> window;
    window.reset(new double[audioHistory.capacity()]);
    unsigned winsize = 0;

    TinySynth synth;
    synth.m_chip = chip;
    synth.resetChip();
    synth.setInstrument(db, ins);
    synth.noteOn();

#ifdef GEN_ADLDATA_DEEP_DEBUG
    /*****************DEBUG******************/
    char waveFileOut[80] = "";
    std::snprintf(waveFileOut, 80, "fm_banks/_deep_debug/%04lu_%s_%u_an_%u_no.wav",
                  ins.instId, synth.m_isPseudo4op ? "pseudo4op" :
                              synth.m_isReal4op ? "4op" : "2op",
                  synth.m_actualNotesNum,
                  synth.m_notesNum);
    void *waveCtx = ctx_wave_open(g_outputRate, waveFileOut);
    ctx_wave_enable_stereo(waveCtx);
    /*****************DEBUG******************/
#endif

    /* For capturing */
    const unsigned max_silent = 6;
    const unsigned max_on  = 40;
    const unsigned max_off = 60;

    unsigned max_period_on = max_on * interval;
    unsigned max_period_off = max_off * interval;

    const double min_coefficient_on = 0.008;
    const double min_coefficient_off = 0.2;

    unsigned windows_passed_on = 0;
    unsigned windows_passed_off = 0;

    /* For Analyze the results */
    double begin_amplitude        = 0;
    double peak_amplitude_value   = 0;
    size_t peak_amplitude_time    = 0;
    size_t quarter_amplitude_time = max_period_on;
    bool   quarter_amplitude_time_found = false;
    size_t keyoff_out_time        = 0;
    bool   keyoff_out_time_found  = false;

    const size_t audioBufferLength = 256;
    const size_t audioBufferSize = 2 * audioBufferLength;
    int16_t audioBuffer[audioBufferSize];

    // For up to 40 seconds, measure mean amplitude.
    double highest_sofar = 0;
    short sound_min = 0, sound_max = 0;

    for(unsigned period = 0; period < max_period_on; ++period, ++windows_passed_on)
    {
        for(unsigned i = 0; i < samples_per_interval;)
        {
            size_t blocksize = samples_per_interval - i;
            blocksize = (blocksize < audioBufferLength) ? blocksize : audioBufferLength;
            synth.generate(audioBuffer, blocksize);
#ifdef GEN_ADLDATA_DEEP_DEBUG
            /***************DEBUG******************/
            ctx_wave_write(waveCtx, audioBuffer, blocksize * 2);
            /***************DEBUG******************/
#endif
            for (unsigned j = 0; j < blocksize; ++j)
            {
                int16_t s = audioBuffer[2 * j];
                audioHistory.add(s);
                if(sound_min > s) sound_min = s;
                if(sound_max < s) sound_max = s;
            }
            i += blocksize;
        }

        if(winsize != audioHistory.size())
        {
            winsize = audioHistory.size();
            HannWindow(window.get(), winsize);
        }

        double rms = MeasureRMS(audioHistory.data(), window.get(), winsize);
        /* ======== Peak time detection ======== */
        if(period == 0)
        {
            begin_amplitude = rms;
            peak_amplitude_value = rms;
            peak_amplitude_time = 0;
        }
        else if(rms > peak_amplitude_value)
        {
            peak_amplitude_value = rms;
            peak_amplitude_time  = period;
            // In next step, update the quater amplitude time
            quarter_amplitude_time_found = false;
        }
        else if(!quarter_amplitude_time_found && (rms <= peak_amplitude_value * min_coefficient_on))
        {
            quarter_amplitude_time = period;
            quarter_amplitude_time_found = true;
        }
        /* ======== Peak time detection =END==== */
        if(rms > highest_sofar)
            highest_sofar = rms;

        if((period > max_silent * interval) &&
           ( (rms < highest_sofar * min_coefficient_on) || (sound_min >= -1 && sound_max <= 1) )
        )
            break;
    }

    if(!quarter_amplitude_time_found)
        quarter_amplitude_time = windows_passed_on;

    if(windows_passed_on >= max_period_on)
    {
        // Just Keyoff the note
        synth.noteOff();
    }
    else
    {
        // Reset the emulator and re-run the "ON" simulation until reaching the peak time
        synth.resetChip();
        synth.setInstrument(db, ins);
        synth.noteOn();

        audioHistory.reset(std::ceil(historyLength * g_outputRate));
        for(unsigned period = 0;
            ((period < peak_amplitude_time) || (period == 0)) && (period < max_period_on);
            ++period)
        {
            for(unsigned i = 0; i < samples_per_interval;)
            {
                size_t blocksize = samples_per_interval - i;
                blocksize = (blocksize < audioBufferLength) ? blocksize : audioBufferLength;
                synth.generate(audioBuffer, blocksize);
                for (unsigned j = 0; j < blocksize; ++j)
                    audioHistory.add(audioBuffer[2 * j]);
                i += blocksize;
            }
        }
        synth.noteOff();
    }

    // Now, for up to 60 seconds, measure mean amplitude.
    for(unsigned period = 0; period < max_period_off; ++period, ++windows_passed_off)
    {
        for(unsigned i = 0; i < samples_per_interval;)
        {
            size_t blocksize = samples_per_interval - i;
            blocksize = (blocksize < 256) ? blocksize : 256;
            synth.generate(audioBuffer, blocksize);
            for (unsigned j = 0; j < blocksize; ++j)
            {
                int16_t s = audioBuffer[2 * j];
                audioHistory.add(s);
                if(sound_min > s) sound_min = s;
                if(sound_max < s) sound_max = s;
            }
            i += blocksize;
        }

        if(winsize != audioHistory.size())
        {
            winsize = audioHistory.size();
            HannWindow(window.get(), winsize);
        }

        double rms = MeasureRMS(audioHistory.data(), window.get(), winsize);
        /* ======== Find Key Off time ======== */
        if(!keyoff_out_time_found && (rms <= peak_amplitude_value * min_coefficient_off))
        {
            keyoff_out_time = period;
            keyoff_out_time_found = true;
        }
        /* ======== Find Key Off time ==END=== */
        if(rms < highest_sofar * min_coefficient_off)
            break;

        if((period > max_silent * interval) && (sound_min >= -1 && sound_max <= 1))
            break;
    }

    DurationInfo result;
    result.peak_amplitude_time = peak_amplitude_time;
    result.peak_amplitude_value = peak_amplitude_value;
    result.begin_amplitude = begin_amplitude;
    result.quarter_amplitude_time = (double)quarter_amplitude_time;
    result.keyoff_out_time = (double)keyoff_out_time;

    result.ms_sound_kon  = (int64_t)(quarter_amplitude_time * 1000.0 / interval);
    result.ms_sound_koff = (int64_t)(keyoff_out_time        * 1000.0 / interval);
    result.nosound = (peak_amplitude_value < 0.5) || ((sound_min >= -19) && (sound_max <= 18));

    db.instruments[ins.instId].delay_on_ms = result.ms_sound_kon;
    db.instruments[ins.instId].delay_off_ms = result.ms_sound_koff;
    if(result.nosound)
        db.instruments[ins.instId].instFlags |= BanksDump::InstrumentEntry::WOPL_Ins_IsBlank;

#ifdef GEN_ADLDATA_DEEP_DEBUG
    /***************DEBUG******************/
    ctx_wave_close(waveCtx);
    /***************DEBUG******************/
#endif
    {
        bool silent1 = result.nosound;
        bool silent2 = BanksDump::isSilent(db, ins);
        if(silent1 != silent2)
        {
            std::fprintf(stdout,
                         "\n\n%04lu - %s  AN=%u NN=%u -- con1=%lu, con2=%lu\n%s computed - %s actual (%g peak, %d<%d)\n\n",
                         ins.instId, synth.m_isPseudo4op ? "pseudo4op" :
                                     synth.m_isReal4op ? "4op" : "2op",
                         synth.m_actualNotesNum,
                         synth.m_notesNum,
                         (ins.fbConn) & 0x01,
                         (ins.fbConn >> 8) & 0x01,
                         silent2 ? "silent" : "sound",
                         silent1 ? "silent" : "sound",
                         peak_amplitude_value,
                         sound_min,
                         sound_max);
            for(auto &sss : ins.instMetas)
                std::fprintf(stdout, "%s\n", sss.c_str());
            BanksDump::isSilent(db, ins, true);
            std::fprintf(stdout, "\n\n");
            std::fflush(stdout);
            assert(silent1 == silent2);
            exit(1);
        }
    }

    return result;
}


MeasureThreaded::MeasureThreaded() :
    m_semaphore(int(std::thread::hardware_concurrency()) * 2),
    m_done(0),
    m_cache_matches(0)
{
    DosBoxOPL3::globalPreInit();
}

void MeasureThreaded::LoadCache(const char *fileName)
{
    m_durationInfo.clear();

    FILE *in = std::fopen(fileName, "rb");
    if(!in)
    {
        std::printf("Failed to load cache: file is not exists.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    char magic[32];
    if(std::fread(magic, 1, 32, in) != 32)
    {
        std::fclose(in);
        std::printf("Failed to load cache: can't read magic.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    if(memcmp(magic, "ADLMIDI-DURATION-CACHE-FILE-V1.0", 32) != 0)
    {
        std::fclose(in);
        std::printf("Failed to load cache: magic missmatch.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    while(!std::feof(in))
    {
        DurationInfo info;
        ins inst;
        //got by instrument
        insdata id[2];
        size_t insNo[2] = {0, 0};
        bool found[2] = {false, false};
        //got from file
        insdata id_f[2];
        bool found_f[2] = {false, false};
        bool isMatches = false;

        memset(id, 0, sizeof(insdata) * 2);
        memset(id_f, 0, sizeof(insdata) * 2);
        memset(&info, 0, sizeof(DurationInfo));
        memset(&inst, 0, sizeof(ins));

        //Instrument
        uint64_t inval;
        if(std::fread(&inval, 1, sizeof(uint64_t), in) != sizeof(uint64_t))
            break;
        inst.insno1 = inval;
        if(std::fread(&inval, 1, sizeof(uint64_t), in) != sizeof(uint64_t))
            break;
        inst.insno2 = inval;
        if(std::fread(&inst.instCache1.data, 1, 11, in) != 11)
            break;
        if(std::fread(&inst.instCache1.finetune, 1, 1, in) != 1)
            break;
        if(std::fread(&inst.instCache1.diff, 1, sizeof(bool), in) != sizeof(bool))
            break;
        if(std::fread(&inst.instCache2.data, 1, 11, in) != 11)
            break;
        if(std::fread(&inst.instCache2.finetune, 1, 1, in) != 1)
            break;
        if(std::fread(&inst.instCache2.diff, 1, sizeof(bool), in) != sizeof(bool))
            break;

        if(std::fread(&inst.notenum, 1, 1, in) != 1)
            break;
        if(std::fread(&inst.real4op, 1, 1, in) != 1)
            break;
        if(std::fread(&inst.pseudo4op, 1, 1, in) != 1)
            break;
        int64_t voice2detune = 0;
        if(std::fread(&voice2detune, sizeof(int64_t), 1, in) != 1)
            break;
        inst.voice2_fine_tune = static_cast<double>(voice2detune) / 1000000.0;

        //Instrument data
        if(fread(found_f, 1, 2 * sizeof(bool), in) != sizeof(bool) * 2)
            break;
        for(size_t i = 0; i < 2; i++)
        {
            if(fread(id_f[i].data, 1, 11, in) != 11)
                break;
            if(fread(&id_f[i].finetune, 1, 1, in) != 1)
                break;
            if(fread(&id_f[i].diff, 1, sizeof(bool), in) != sizeof(bool))
                break;
        }

        if(found_f[0] || found_f[1])
        {
            for(InstrumentDataTab::const_iterator j = insdatatab.begin(); j != insdatatab.end(); ++j)
            {
                if(j->second.first == inst.insno1)
                {
                    id[0] = j->first;
                    found[0] = (id[0] == id_f[0]);
                    insNo[0] = inst.insno1;
                    if(found[1]) break;
                }
                if(j->second.first == inst.insno2)
                {
                    id[1] = j->first;
                    found[1] = (id[1] == id_f[1]);
                    insNo[1] = inst.insno2;
                    if(found[0]) break;
                }
            }

            //Find instrument entries are matching
            if((found[0] != found_f[0]) || (found[1] != found_f[1]))
            {
                for(InstrumentDataTab::const_iterator j = insdatatab.begin(); j != insdatatab.end(); ++j)
                {
                    if(found_f[0] && (j->first == id_f[0]))
                    {
                        found[0] = true;
                        insNo[0] = j->second.first;
                    }
                    if(found_f[1] && (j->first == id_f[1]))
                    {
                        found[1] = true;
                        insNo[1] = j->second.first;
                    }
                    if(found[0] && !found_f[1])
                    {
                        isMatches = true;
                        break;
                    }
                    if(found[0] && found[1])
                    {
                        isMatches = true;
                        break;
                    }
                }
            }
            else
            {
                isMatches = true;
            }

            //Then find instrument entry that uses found instruments
            if(isMatches)
            {
                inst.insno1 = insNo[0];
                inst.insno2 = insNo[1];
                InstrumentsData::iterator d = instab.find(inst);
                if(d == instab.end())
                    isMatches = false;
            }
        }

        //Duration data
        if(std::fread(&info.ms_sound_kon, 1, sizeof(int64_t), in) != sizeof(int64_t))
            break;
        if(std::fread(&info.ms_sound_koff, 1, sizeof(int64_t), in) != sizeof(int64_t))
            break;
        if(std::fread(&info.nosound, 1, sizeof(bool), in) != sizeof(bool))
            break;

        if(isMatches)//Store only if cached entry matches actual raw instrument data
            m_durationInfo.insert({inst, info});
    }

    std::printf("Cache loaded!\n");
    std::fflush(stdout);

    std::fclose(in);
}

void MeasureThreaded::SaveCache(const char *fileName)
{
    FILE *out = std::fopen(fileName, "wb");
    fprintf(out, "ADLMIDI-DURATION-CACHE-FILE-V1.0");
    for(DurationInfoCache::iterator it = m_durationInfo.begin(); it != m_durationInfo.end(); it++)
    {
        const ins &in = it->first;
        insdata id[2];
        bool found[2] = {false, false};
        memset(id, 0, sizeof(insdata) * 2);

        uint64_t outval;
        outval = in.insno1;
        fwrite(&outval, 1, sizeof(uint64_t), out);
        outval = in.insno2;
        fwrite(&outval, 1, sizeof(uint64_t), out);
        fwrite(&in.instCache1.data, 1, 11, out);
        fwrite(&in.instCache1.finetune, 1, 1, out);
        fwrite(&in.instCache1.diff, 1, sizeof(bool), out);
        fwrite(&in.instCache2.data, 1, 11, out);
        fwrite(&in.instCache2.finetune, 1, 1, out);
        fwrite(&in.instCache2.diff, 1, sizeof(bool), out);
        fwrite(&in.notenum, 1, 1, out);
        fwrite(&in.real4op, 1, 1, out);
        fwrite(&in.pseudo4op, 1, 1, out);
        int64_t voice2detune = static_cast<int64_t>(in.voice2_fine_tune * 1000000.0);
        fwrite(&voice2detune, sizeof(int64_t), 1, out);

        for(InstrumentDataTab::const_iterator j = insdatatab.begin(); j != insdatatab.end(); ++j)
        {
            if(j->second.first == in.insno1)
            {
                id[0] = j->first;
                found[0] = true;
                if(found[1]) break;
            }
            if(j->second.first == in.insno2)
            {
                id[1] = j->first;
                found[1] = true;
                if(found[0]) break;
            }
        }

        fwrite(found, 1, 2 * sizeof(bool), out);
        for(size_t i = 0; i < 2; i++)
        {
            fwrite(id[i].data, 1, 11, out);
            fwrite(&id[i].finetune, 1, 1, out);
            fwrite(&id[i].diff, 1, sizeof(bool), out);
        }

        fwrite(&it->second.ms_sound_kon, 1, sizeof(int64_t), out);
        fwrite(&it->second.ms_sound_koff, 1, sizeof(int64_t), out);
        fwrite(&it->second.nosound, 1, sizeof(bool), out);
    }
    std::fclose(out);
}

void MeasureThreaded::LoadCacheX(const char *fileName)
{
    m_durationInfoX.clear();

    FILE *in = std::fopen(fileName, "rb");
    if(!in)
    {
        std::printf("Failed to load CacheX: file is not exists.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    char magic[32];
    if(std::fread(magic, 1, 32, in) != 32)
    {
        std::fclose(in);
        std::printf("Failed to load CacheX: can't read magic.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    if(std::memcmp(magic, "ADLMIDI-DURATION-CACHE-FILE-V2.0", 32) != 0)
    {
        std::fclose(in);
        std::printf("Failed to load CacheX: magic missmatch.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    uint_fast32_t itemsCount;
    uint8_t itemsCountA[4];
    if(std::fread(itemsCountA, 1, 4, in) != 4)
    {
        std::fclose(in);
        std::printf("Failed to load CacheX: can't read cache size value.\n"
               "Complete data will be generated from scratch.\n");
        std::fflush(stdout);
        return;
    }

    itemsCount = static_cast<uint_fast32_t>(toUint32LE(itemsCountA));

    while(!std::feof(in) && itemsCount > 0)
    {
        OperatorsKey k;
        DurationInfo v;

        uint8_t data_k[5];

        for(auto &kv : k)
        {
            uint8_t data[4];
            auto ret = std::fread(data, 1, 4, in);
            if(ret != 4)
            {
                std::fclose(in);
                std::printf("Failed to load CacheX: unexpected end of file.\n"
                       "Complete data will be generated from scratch.\n");
                std::fflush(stdout);
                return;
            }
            kv = static_cast<int_fast32_t>(toSint32LE(data));
        }

        auto ret = std::fread(data_k, 1, 5, in);
        if(ret != 5)
        {
            std::fclose(in);
            std::printf("Failed to load CacheX: unexpected end of file.\n"
                   "Complete data will be generated from scratch.\n");
            std::fflush(stdout);
            return;
        }

        v.ms_sound_kon  = static_cast<int_fast64_t>(toUint16LE(data_k + 0));
        v.ms_sound_koff = static_cast<int_fast64_t>(toUint16LE(data_k + 2));
        v.nosound = (data_k[4] == 0x01);

        m_durationInfoX.insert({k, v});
        itemsCount--;
    }

    std::printf("CacheX loaded!\n");
    std::fflush(stdout);

    std::fclose(in);
}

void MeasureThreaded::SaveCacheX(const char *fileName)
{
    FILE *out = std::fopen(fileName, "wb");
    std::fprintf(out, "ADLMIDI-DURATION-CACHE-FILE-V2.0");

    uint_fast32_t itemsCount = static_cast<uint_fast32_t>(m_durationInfoX.size());
    uint8_t itemsCountA[4] =
    {
        static_cast<uint8_t>((itemsCount >>  0) & 0xFF),
        static_cast<uint8_t>((itemsCount >>  8) & 0xFF),
        static_cast<uint8_t>((itemsCount >> 16) & 0xFF),
        static_cast<uint8_t>((itemsCount >> 24) & 0xFF)
    };
    std::fwrite(itemsCountA, 1, 4, out);

    for(DurationInfoCacheX::iterator it = m_durationInfoX.begin(); it != m_durationInfoX.end(); it++)
    {
        const OperatorsKey &k = it->first;
        const DurationInfo &v = it->second;

        uint8_t data_k[5] =
        {
            static_cast<uint8_t>((v.ms_sound_kon >>  0) & 0xFF),
            static_cast<uint8_t>((v.ms_sound_kon >>  8) & 0xFF),
            static_cast<uint8_t>((v.ms_sound_koff >> 0) & 0xFF),
            static_cast<uint8_t>((v.ms_sound_koff >> 8) & 0xFF),
            static_cast<uint8_t>(v.nosound ? 0x01 : 0x00)
        };

        for(auto &kv : k)
        {
            uint8_t data[4] =
            {
                static_cast<uint8_t>((kv >>  0) & 0xFF),
                static_cast<uint8_t>((kv >>  8) & 0xFF),
                static_cast<uint8_t>((kv >> 16) & 0xFF),
                static_cast<uint8_t>((kv >> 24) & 0xFF)
            };
            std::fwrite(data, 1, 4, out);
        }
        std::fwrite(data_k, 1, 5, out);
    }
    std::fclose(out);
}

#ifdef ADL_GENDATA_PRINT_PROGRESS

static const char* spinner = "-\\|/";

void MeasureThreaded::printProgress()
{
    std::printf("Calculating measures... [%c %3u%% {%4u/%4u} Threads %3u, Matches %u]       \r",
            spinner[m_done.load() % 4],
            (unsigned int)(((double)m_done.load() / (double)(m_total)) * 100),
            (unsigned int)m_done.load(),
            (unsigned int)m_total,
            (unsigned int)m_threads.size(),
            (unsigned int)m_cache_matches
            );
    std::fflush(stdout);
}
#else
void MeasureThreaded::printProgress()
{
    //Do nothing
}
#endif

void MeasureThreaded::printFinal()
{
    std::printf("Calculating measures completed! [Total entries %4u with %u cache matches]\n",
            (unsigned int)m_total,
            (unsigned int)m_cache_matches);
    std::fflush(stdout);
}

void MeasureThreaded::run(InstrumentsData::const_iterator i)
{
    m_semaphore.wait();
    if(m_threads.size() > 0)
    {
        for(std::vector<destData *>::iterator it = m_threads.begin(); it != m_threads.end();)
        {
            if(!(*it)->m_works)
            {
                delete(*it);
                it = m_threads.erase(it);
            }
            else
                it++;
        }
    }

    destData *dd = new destData;
    dd->i = i;
    dd->bd = nullptr;
    dd->bd_ins = nullptr;
    dd->myself = this;
    dd->start();
    m_threads.push_back(dd);
#ifdef ADL_GENDATA_PRINT_PROGRESS
    printProgress();
#endif
}

void MeasureThreaded::run(BanksDump &bd, BanksDump::InstrumentEntry &e)
{
    m_semaphore.wait();
    if(m_threads.size() > 0)
    {
        for(std::vector<destData *>::iterator it = m_threads.begin(); it != m_threads.end();)
        {
            if(!(*it)->m_works)
            {
                delete(*it);
                it = m_threads.erase(it);
            }
            else
                it++;
        }
    }

    destData *dd = new destData;
    dd->bd = &bd;
    dd->bd_ins = &e;
    dd->myself = this;
    dd->start();
    m_threads.push_back(dd);
#ifdef ADL_GENDATA_PRINT_PROGRESS
    printProgress();
#endif
}

void MeasureThreaded::waitAll()
{
    for(auto &th : m_threads)
    {
#ifdef ADL_GENDATA_PRINT_PROGRESS
        printProgress();
#endif
        delete th;
    }
    m_threads.clear();
    printFinal();
}

void MeasureThreaded::destData::start()
{
    m_work = std::thread(&destData::callback, this);
}

void MeasureThreaded::destData::callback(void *myself)
{
    destData *s = reinterpret_cast<destData *>(myself);
    DurationInfo info;
    DosBoxOPL3 dosbox;
    // NukedOPL3 dosbox;

    if(s->bd)
    {
        OperatorsKey ok = {s->bd_ins->ops[0], s->bd_ins->ops[1], s->bd_ins->ops[2], s->bd_ins->ops[3],
                           static_cast<int_fast32_t>(s->bd_ins->fbConn),
                           s->bd_ins->noteOffset1, s->bd_ins->noteOffset2,
                           static_cast<int_fast32_t>(s->bd_ins->percussionKeyNumber),
                           static_cast<int_fast32_t>(s->bd_ins->instFlags),
                           static_cast<int_fast32_t>(s->bd_ins->secondVoiceDetune)};
        s->myself->m_durationInfo_mx.lock();
        DurationInfoCacheX::iterator cachedEntry = s->myself->m_durationInfoX.find(ok);
        bool atEnd = cachedEntry == s->myself->m_durationInfoX.end();
        s->myself->m_durationInfo_mx.unlock();

        if(!atEnd)
        {
            const DurationInfo &di = cachedEntry->second;
            s->bd_ins->delay_on_ms = di.ms_sound_kon;
            s->bd_ins->delay_off_ms = di.ms_sound_koff;
            if(di.nosound)
                s->bd_ins->instFlags |= BanksDump::InstrumentEntry::WOPL_Ins_IsBlank;
            s->myself->m_cache_matches++;
            goto endWork;
        }
        info = MeasureDurations(*s->bd, *s->bd_ins, &dosbox);
        s->myself->m_durationInfo_mx.lock();
        s->myself->m_durationInfoX.insert({ok, info});
        s->myself->m_durationInfo_mx.unlock();
    }
    else
    {
        const ins &ok = s->i->first;
        s->myself->m_durationInfo_mx.lock();
        DurationInfoCache::iterator cachedEntry = s->myself->m_durationInfo.find(ok);
        bool atEnd = cachedEntry == s->myself->m_durationInfo.end();
        s->myself->m_durationInfo_mx.unlock();

        if(!atEnd)
        {
            s->myself->m_cache_matches++;
            goto endWork;
        }

        info = MeasureDurations(ok, &dosbox);
        s->myself->m_durationInfo_mx.lock();
        s->myself->m_durationInfo.insert({ok, info});
        s->myself->m_durationInfo_mx.unlock();
    }

endWork:
    s->myself->m_semaphore.notify();
    s->myself->m_done++;
    s->m_works = false;
}
