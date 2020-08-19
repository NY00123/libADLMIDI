//#ifdef __MINGW32__
//typedef struct vswprintf {} swprintf;
//#endif
#include <cstdio>
#include <string>
#include <cstring>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "ini/ini_processing.h"

#include "progs_cache.h"
#include "measurer.h"

#include "midi_inst_list.h"

int main(int argc, char**argv)
{
    if(argc == 1)
    {
        std::printf("Usage:\n"
               "\n"
               "bin/gen_adldata src/adldata.cpp\n"
               "\n");
        return 1;
    }

    const char *outFile_s = argv[1];

#if 0
    FILE *outFile = std::fopen(outFile_s, "w");
    if(!outFile)
    {
        std::fprintf(stderr, "Can't open %s file for write!\n", outFile_s);
        return 1;
    }
#endif

    BanksDump db;

#if 0
    std::fprintf(outFile, "\
#include \"adldata.hh\"\n\
\n\
/* THIS OPL-3 FM INSTRUMENT DATA IS AUTOMATICALLY GENERATED\n\
 * FROM A NUMBER OF SOURCES, MOSTLY PC GAMES.\n\
 * PREPROCESSED, CONVERTED, AND POSTPROCESSED OFF-SCREEN.\n\
 */\n\
");
#endif


    {
        IniProcessing ini;
        if(!ini.open("banks.ini"))
        {
            std::fprintf(stderr, "Can't open banks.ini!\n");
            return 1;
        }

        uint32_t banks_count;
        ini.beginGroup("General");
        ini.read("banks", banks_count, 0);
        ini.endGroup();

        if(!banks_count)
        {
            std::fprintf(stderr, "Zero count of banks found in banks.ini!\n");
            return 1;
        }

        for(uint32_t bank = 0; bank < banks_count; bank++)
        {
            if(!ini.beginGroup(std::string("bank-") + std::to_string(bank)))
            {
                std::fprintf(stderr, "Failed to find bank %u!\n", bank);
                return 1;
            }
            std::string bank_name;
            std::string filepath;
            std::string filepath_d;
            std::string prefix;
            std::string prefix_d;
            std::string filter_m;
            std::string filter_p;
            std::string format;
            bool noRhythmMode = false;

            ini.read("name",     bank_name, "Untitled");
            ini.read("format",   format,    "Unknown");
            ini.read("file",     filepath,  "");
            ini.read("file-p",   filepath_d,  "");
            ini.read("prefix",   prefix, "");
            ini.read("prefix-p", prefix_d, "");
            ini.read("filter-m", filter_m, "");
            ini.read("filter-p", filter_p, "");
            ini.read("no-rhythm-mode", noRhythmMode, false);

            if(filepath.empty())
            {
                std::fprintf(stderr, "Failed to load bank %u, file is empty!\n", bank);
                return 1;
            }

//            banknames.push_back(bank_name);

            //printf("Loading %s...\n", filepath.c_str());

            if(format == "AIL")
            {
                if(!BankFormats::LoadMiles(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "Bisqwit")
            {
                if(!BankFormats::LoadBisqwit(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "WOPL")
            {
                if(!BankFormats::LoadWopl(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "OP2")
            {
                if(!BankFormats::LoadDoom(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "EA")
            {
                if(!BankFormats::LoadEA(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "TMB")
            {
                if(!BankFormats::LoadTMB(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "Junglevision")
            {
                if(!BankFormats::LoadJunglevision(db, filepath.c_str(), bank, bank_name, prefix.c_str()))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "AdLibGold")
            {
                if(!BankFormats::LoadBNK2(db, filepath.c_str(), bank, bank_name, prefix.c_str(), filter_m, filter_p))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
            }
            else
            if(format == "HMI")
            {
                if(!BankFormats::LoadBNK(db, filepath.c_str(),  bank, bank_name, prefix.c_str(),   false, false))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
                if(!filepath_d.empty())
                {
                    if(!BankFormats::LoadBNK(db, filepath_d.c_str(), bank, bank_name, prefix_d.c_str(), false, true))
                    {
                        std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                        return 1;
                    }
                }
            }
            else
            if(format == "IBK")
            {
                if(!BankFormats::LoadIBK(db, filepath.c_str(),  bank, bank_name, prefix.c_str(),   false))
                {
                    std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                    return 1;
                }
                if(!filepath_d.empty())
                {
                    //printf("Loading %s... \n", filepath_d.c_str());
                    if(!BankFormats::LoadIBK(db, filepath_d.c_str(),bank, bank_name, prefix_d.c_str(), true, noRhythmMode))
                    {
                        std::fprintf(stderr, "Failed to load bank %u, file %s!\n", bank, filepath.c_str());
                        return 1;
                    }
                }
            }
            else
            {
                std::fprintf(stderr, "Failed to load bank %u, file %s!\nUnknown format type %s\n",
                        bank,
                        filepath.c_str(),
                        format.c_str());
                return 1;
            }


            ini.endGroup();
        }

        std::printf("Loaded %u banks!\n", banks_count);
        std::fflush(stdout);
    }

    #if 0
    for(unsigned a = 0; a < 36 * 8; ++a)
    {
        if((1 << (a % 8)) > maxvalues[a / 8]) continue;

        const std::map<unsigned, unsigned> &data = Correlate[a];
        if(data.empty()) continue;
        std::vector< std::pair<unsigned, unsigned> > correlations;
        for(std::map<unsigned, unsigned>::const_iterator
            i = data.begin();
            i != data.end();
            ++i)
        {
            correlations.push_back(std::make_pair(i->second, i->first));
        }
        std::sort(correlations.begin(), correlations.end());
        std::fprintf(outFile, "Byte %2u bit %u=mask %02X:\n", a / 8, a % 8, 1 << (a % 8));
        for(size_t c = 0; c < correlations.size() && c < 10; ++c)
        {
            unsigned count = correlations[correlations.size() - 1 - c ].first;
            unsigned index = correlations[correlations.size() - 1 - c ].second;
            std::fprintf(outFile, "\tAdldata index %u, bit %u=mask %02X (%u matches)\n",
                    index / 8, index % 8, 1 << (index % 8), count);
        }
    }
    #endif

//    std::printf("Writing raw instrument data...\n");
//    std::fflush(stdout);

    MeasureThreaded measureCounter;
//#ifndef GEN_ADLDATA_DEEP_DEBUG // Skip slowest place to work with a debug
//    {
//        std::printf("Beginning to generate measures data... (hardware concurrency of %d)\n", std::thread::hardware_concurrency());
//        std::fflush(stdout);
//        measureCounter.LoadCache("fm_banks/adldata-cache.dat");
//        measureCounter.m_total = instab.size();
//        for(size_t b = instab.size(), c = 0; c < b; ++c)
//        {
//            for(std::map<ins, std::pair<size_t, std::set<std::string> > >::const_iterator i =  instab.begin(); i != instab.end(); ++i)
//            {
//                if(i->second.first != c) continue;
//                measureCounter.run(i);
//            }
//        }
//        std::fflush(stdout);
//        measureCounter.waitAll();
//        measureCounter.SaveCache("fm_banks/adldata-cache.dat");
//    }
//#endif

//    std::printf("Writing generated measure data...\n");
//    std::fflush(stdout);

#if 0
    std::vector<unsigned> adlins_flags;

    for(size_t b = instab.size(), c = 0; c < b; ++c)
        for(std::map<ins, std::pair<size_t, std::set<std::string> > >
            ::const_iterator
            i = instab.begin();
            i != instab.end();
            ++i)
        {
            if(i->second.first != c) continue;
            //DurationInfo info = MeasureDurations(i->first);
            MeasureThreaded::DurationInfoCache::iterator indo_i = measureCounter.m_durationInfo.find(i->first);
            DurationInfo info = indo_i->second;
#ifdef ADLDATA_WITH_COMMENTS
            {
                if(info.peak_amplitude_time == 0)
                {
                    std::fprintf(outFile,
                        "    // Amplitude begins at %6.1f,\n"
                        "    // fades to 20%% at %.1fs, keyoff fades to 20%% in %.1fs.\n",
                        info.begin_amplitude,
                        info.quarter_amplitude_time / double(info.interval),
                        info.keyoff_out_time / double(info.interval));
                }
                else
                {
                    std::fprintf(outFile,
                        "    // Amplitude begins at %6.1f, peaks %6.1f at %.1fs,\n"
                        "    // fades to 20%% at %.1fs, keyoff fades to 20%% in %.1fs.\n",
                        info.begin_amplitude,
                        info.peak_amplitude_value,
                        info.peak_amplitude_time / double(info.interval),
                        info.quarter_amplitude_time / double(info.interval),
                        info.keyoff_out_time / double(info.interval));
                }
            }
#endif

            unsigned flags = (i->first.pseudo4op ? ins::Flag_Pseudo4op : 0)|
                             (i->first.real4op ? ins::Flag_Real4op : 0) |
                             (info.nosound ? ins::Flag_NoSound : 0) | i->first.rhythmModeDrum;

            std::fprintf(outFile, "    {");
            std::fprintf(outFile, "%4d,%4d,%3d, %d, %6" PRId64 ",%6" PRId64 ", %6d, %g",
                    (unsigned) i->first.insno1,
                    (unsigned) i->first.insno2,
                    (int)(i->first.notenum),
                    flags,
                    info.ms_sound_kon,
                    info.ms_sound_koff,
                    i->first.midi_velocity_offset,
                    i->first.voice2_fine_tune);
            std::string names;
            for(std::set<std::string>::const_iterator
                j = i->second.second.begin();
                j != i->second.second.end();
                ++j)
            {
                if(!names.empty()) names += "; ";
                if((*j)[0] == '\377')
                    names += j->substr(1);
                else
                    names += *j;
            }
#ifdef ADLDATA_WITH_COMMENTS
            std::fprintf(outFile, " }, // %u: %s\n\n", (unsigned)c, names.c_str());
#else
            std::fprintf(outFile, " },\n");
#endif
            std::fflush(outFile);
            adlins_flags.push_back(flags);
        }
    std::fprintf(outFile, "};\n\n");


    std::printf("Writing banks data...\n");
    std::fflush(stdout);

    //fprintf(outFile, "static const unsigned short banks[][256] =\n");
#ifdef HARD_BANKS
    const unsigned bankcount = sizeof(banknames) / sizeof(*banknames);
#else
    const size_t bankcount = banknames.size();
#endif

    size_t nosound = InsertNoSoundIns();

    std::map<size_t, std::vector<size_t> > bank_data;
    for(size_t bank = 0; bank < bankcount; ++bank)
    {
        //bool redundant = true;
        std::vector<size_t> data(256);
        for(size_t p = 0; p < 256; ++p)
        {
            size_t v = progs[bank][p];
            if(v == 0 || (adlins_flags[v - 1] & 2))
                v = nosound; // Blank.in
            else
                v -= 1;
            data[p] = v;
        }
        bank_data[bank] = data;
    }
    std::set<size_t> listed;


    std::fprintf(outFile,
            "\n\n//Returns total number of generated banks\n"
            "int  maxAdlBanks()\n"
            "{\n"
            "   return %u;\n"
            "}\n\n"
            "const char* const banknames[%u] =\n",
                 (unsigned int)bankcount,
                 (unsigned int)(bankcount + 1));
    std::fprintf(outFile, "{\n");
    for(size_t bank = 0; bank < bankcount; ++bank)
        std::fprintf(outFile, "    \"%s\",\n", banknames[bank].c_str());
    std::fprintf(outFile, "    NULL\n};\n");

    std::fprintf(outFile, "const unsigned short banks[%u][256] =\n", (unsigned int)bankcount);
    std::fprintf(outFile, "{\n");
    for(size_t bank = 0; bank < bankcount; ++bank)
    {
#ifdef ADLDATA_WITH_COMMENTS
        std::fprintf(outFile, "    { // bank %u, %s\n", bank, banknames[bank].c_str());
#else
        std::fprintf(outFile, "    {\n");
        #endif
#ifdef ADLDATA_WITH_COMMENTS
        bool redundant = true;
#endif
        for(size_t p = 0; p < 256; ++p)
        {
            size_t v = bank_data[bank][p];
            if(listed.find(v) == listed.end())
            {
                listed.insert(v);
#ifdef ADLDATA_WITH_COMMENTS
                redundant = false;
#endif
            }
            std::fprintf(outFile, "%4d,", (unsigned int)v);
            if(p % 16 == 15) fprintf(outFile, "\n");
        }
        std::fprintf(outFile, "    },\n");
#ifdef ADLDATA_WITH_COMMENTS
        if(redundant)
        {
            std::fprintf(outFile, "    // Bank %u defines nothing new.\n", bank);
            for(unsigned refbank = 0; refbank < bank; ++refbank)
            {
                bool match = true;
                for(unsigned p = 0; p < 256; ++p)
                    if(bank_data[bank][p] != nosound
                       && bank_data[bank][p] != bank_data[refbank][p])
                    {
                        match = false;
                        break;
                    }
                if(match)
                    std::fprintf(outFile, "    // Bank %u is just a subset of bank %u!\n",
                            bank, refbank);
            }
        }
#endif
    }

#endif


#if 0
    std::fprintf(outFile, "};\n\n");
    std::fflush(outFile);

    std::fprintf(outFile, "const AdlBankSetup adlbanksetup[%u] =\n", (unsigned)banksetup.size());
    std::fprintf(outFile, "{\n");
    {
        BankSetupData::iterator last = banksetup.end();
        last--;
        for(BankSetupData::iterator it = banksetup.begin(); it != banksetup.end(); it++)
        {
            AdlBankSetup &setup = it->second;
            std::fprintf(outFile, "    {%d, %d, %d, %d}",
                         setup.volumeModel,
                         setup.deepTremolo,
                         setup.deepVibrato,
                         setup.scaleModulators);
            if(it != last)
                std::fprintf(outFile, ", //Bank %u, %s\n", (unsigned)it->first, banknames[it->first].c_str());
            else
                std::fprintf(outFile, "  //Bank %u, %s\n", (unsigned)it->first, banknames[it->first].c_str());
        }
    }
    std::fprintf(outFile, "};\n");
    std::fflush(outFile);

    std::fclose(outFile);
#endif

    {
        measureCounter.LoadCacheX("fm_banks/adldata-cache.dat");
        measureCounter.m_durationInfo.clear();
        measureCounter.m_cache_matches = 0;
        measureCounter.m_done = 0;
        measureCounter.m_total = db.instruments.size();
        std::printf("Beginning to generate measures data... (hardware concurrency of %d)\n", std::thread::hardware_concurrency());
        std::fflush(stdout);
        for(size_t b = 0; b < db.instruments.size(); ++b)
        {
            assert(db.instruments[b].instId == b);
            measureCounter.run(db, db.instruments[b]);
        }
        std::fflush(stdout);
        measureCounter.waitAll();
        measureCounter.SaveCacheX("fm_banks/adldata-cache.dat");
    }

    db.exportBanks(std::string(outFile_s));

    std::printf("Generation of ADLMIDI data has been completed!\n");
    std::fflush(stdout);
}
