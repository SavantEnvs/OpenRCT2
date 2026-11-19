/*
 * libFuzzer harnesses for OpenRCT2 (savantenvs mayhem layer).
 *
 * One source, one binary per FUZZ_MODE_* (selected with -DFUZZ_MODE_<name> by mayhem/build.sh).
 * Every target goes through the entry point the game itself uses for that kind of untrusted file:
 *
 *   park_file       ParkImporter::CreateParkFile()->LoadFromStream + LoadObjects + Import   (.park)
 *   s6              ParkImporter::CreateS6()      ->LoadFromStream + LoadObjects + Import   (.sv6/.sc6, RCT2)
 *   s4              ParkImporter::CreateS4()      ->LoadFromStream + LoadObjects + Import   (.sv4/.sc4, RCT1)
 *   td6             TrackImporter::CreateTD6()->LoadFromStream + Import                     (.td6 track design)
 *   td4             TrackImporter::CreateTD4()->LoadFromStream + Import                     (.td4 track design)
 *   object_dat      ObjectFactory::CreateObjectFromLegacyFile + Object::Load/Unload         (.dat object)
 *   object_json     ObjectFactory::CreateObjectFromJsonFile   + Object::Load/Unload         (object.json)
 *   object_parkobj  ObjectFactory::CreateObjectFromZipFile    + Object::Load/Unload         (.parkobj zip)
 *                   (json/parkobj: loadImages=false, as the headless game loads them — image references
 *                    resolve against the RCT2 g1 table, which a headless instance does not have)
 *   parkinfo        `openrct2-cli parkinfo objects <file>` via CommandLineRun               (CLI)
 *   simulate        IContext::LoadParkFromFile + 50 game ticks (`openrct2-cli simulate`)    (any park file)
 *   sea             IContext::LoadParkFromFile on an input named *.sea -> DecryptSea + park load
 *                   (RCT Classic encrypted scenario; the key derives from the file name, so seeds are
 *                    encrypted for "input.sea")
 *   sprite          `openrct2-cli sprite exportall <file> <dir>` via CommandLineRun         (G1 sprite file)
 *   language        LanguagePackFactory::FromText + GetString over every id                 (language .txt)
 *   config_ini      Config::OpenFromPath (the config.ini loader, IniReader)                 (config.ini)
 *   replay          IReplayManager::StartPlayback + up to 10 replayed ticks                 (.parkrep)
 *   title_sequence  Title::LoadTitleSequence + TitleSequenceGetParkHandle + park load       (.parkseq zip)
 *
 * State kept across inputs (on purpose):
 *  - One headless, no-graphics IContext per process (created in LLVMFuzzerInitialize), as the game and
 *    upstream's tests do. Each park/replay input replaces the whole game state and the loaded object set
 *    (ObjectManager::LoadObjects unloads what the new input does not need), so a result depends on the
 *    input, not on its predecessors. The object repository (read-only index of the shipped objects) is
 *    never modified by an input.
 *  - A private scratch directory under $TMPDIR (user-data dir + the input file); input files are rewritten
 *    for every input and the sprite output directory is removed after every input.
 * parkinfo creates (and destroys) its own context per input, exactly like the CLI command does.
 */
#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/ReplayManager.h>
#include <openrct2/TrackImporter.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/localisation/LanguagePack.h>
#include <openrct2/object/Object.h>
#include <openrct2/object/ObjectFactory.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/scenes/title/TitleSequence.h>
#include <openrct2/world/MapAnimation.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>

using namespace OpenRCT2;

namespace
{
    std::unique_ptr<IContext> gFuzzContext;
    std::string gScratchDir;
    std::string gInputPath;

    std::string ExeDir()
    {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec)
            return ".";
        return exe.parent_path().string();
    }

    bool WriteInput(const std::string& path, const uint8_t* data, size_t size)
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;
        bool ok = size == 0 || fwrite(data, 1, size, f) == size;
        ok = (fclose(f) == 0) && ok;
        return ok;
    }

    // Data dir: <exe dir>/../data (the repo's data/ with data/object -> the pinned object set; build.sh
    // lays it out). User dir: a private scratch dir seeded with the object index build.sh pre-built, so a
    // fresh process does not rescan ~2500 objects.
    void SetupPaths(const char* inputName)
    {
        const char* tmp = getenv("TMPDIR");
        std::string templ = std::string(tmp != nullptr && *tmp ? tmp : "/tmp") + "/openrct2-fuzz-XXXXXX";
        char* dir = mkdtemp(templ.data());
        if (dir == nullptr)
        {
            perror("mkdtemp");
            abort();
        }
        gScratchDir = dir;
        auto exeDir = ExeDir();
        std::error_code ec;
        std::filesystem::copy_file(
            exeDir + "/fuzz-userdata/objects.idx", gScratchDir + "/objects.idx",
            std::filesystem::copy_options::overwrite_existing, ec);
        gCustomOpenRCT2DataPath = std::filesystem::weakly_canonical(exeDir + "/../data", ec).string();
        gCustomUserDataPath = gScratchDir;
        gInputPath = gScratchDir + "/" + inputName;
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        // Diagnostics off for the whole run (set once, never changed by an input): malformed parks log one
        // line per bad tile/entity, and printing them dominated the run time.
        for (auto& level : _log_levels)
            level = false;
    }

    void InitContext()
    {
        gFuzzContext = CreateContext();
        if (!gFuzzContext->Initialise())
        {
            fprintf(stderr, "OpenRCT2 context failed to initialise\n");
            abort();
        }
    }

    void ImportPark(std::unique_ptr<IParkImporter> importer, const uint8_t* data, size_t size)
    {
        MemoryStream ms(data, size);
        auto result = importer->LoadFromStream(&ms, false, false, {});
        gFuzzContext->GetObjectManager().LoadObjects(result.RequiredObjects);
        MapAnimations::ClearAll();
        importer->Import(getGameState());
    }

    void ImportTrack(std::unique_ptr<ITrackImporter> importer, const uint8_t* data, size_t size)
    {
        MemoryStream ms(data, size);
        if (importer->LoadFromStream(&ms))
        {
            auto td = importer->Import();
            (void)td;
        }
    }

    void LoadAndUnload(std::unique_ptr<Object> obj)
    {
        if (obj == nullptr)
            return;
        obj->Load();
        obj->Unload();
    }
} // namespace

#if defined(FUZZ_MODE_PARK_FILE)
#define INPUT_NAME "input.park"
#elif defined(FUZZ_MODE_S6)
#define INPUT_NAME "input.sv6"
#elif defined(FUZZ_MODE_S4)
#define INPUT_NAME "input.sv4"
#elif defined(FUZZ_MODE_TD6)
#define INPUT_NAME "input.td6"
#elif defined(FUZZ_MODE_TD4)
#define INPUT_NAME "input.td4"
#elif defined(FUZZ_MODE_OBJECT_DAT)
#define INPUT_NAME "input.dat"
#elif defined(FUZZ_MODE_OBJECT_JSON)
#define INPUT_NAME "object.json"
#elif defined(FUZZ_MODE_OBJECT_PARKOBJ)
#define INPUT_NAME "input.parkobj"
#elif defined(FUZZ_MODE_PARKINFO)
#define INPUT_NAME "input.park"
#elif defined(FUZZ_MODE_SIMULATE)
#define INPUT_NAME "input.park"
#elif defined(FUZZ_MODE_SEA)
#define INPUT_NAME "input.sea"
#elif defined(FUZZ_MODE_SPRITE)
#define INPUT_NAME "input.dat"
#elif defined(FUZZ_MODE_LANGUAGE)
#define INPUT_NAME "input.txt"
#elif defined(FUZZ_MODE_CONFIG_INI)
#define INPUT_NAME "config.ini"
#elif defined(FUZZ_MODE_REPLAY)
#define INPUT_NAME "input.parkrep"
#elif defined(FUZZ_MODE_TITLE_SEQUENCE)
#define INPUT_NAME "input.parkseq"
#else
#error "define one FUZZ_MODE_*"
#endif

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
    SetupPaths(INPUT_NAME);
#if !defined(FUZZ_MODE_PARKINFO) && !defined(FUZZ_MODE_LANGUAGE) && !defined(FUZZ_MODE_SPRITE)
    InitContext();
#endif
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    try
    {
#if defined(FUZZ_MODE_PARK_FILE)
        ImportPark(ParkImporter::CreateParkFile(gFuzzContext->GetObjectRepository()), data, size);
#elif defined(FUZZ_MODE_S6)
        ImportPark(ParkImporter::CreateS6(gFuzzContext->GetObjectRepository()), data, size);
#elif defined(FUZZ_MODE_S4)
        ImportPark(ParkImporter::CreateS4(), data, size);
#elif defined(FUZZ_MODE_TD6)
        ImportTrack(TrackImporter::CreateTD6(), data, size);
#elif defined(FUZZ_MODE_TD4)
        ImportTrack(TrackImporter::CreateTD4(), data, size);
#elif defined(FUZZ_MODE_OBJECT_DAT)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        LoadAndUnload(ObjectFactory::CreateObjectFromLegacyFile(gInputPath.c_str(), true));
#elif defined(FUZZ_MODE_OBJECT_JSON)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        LoadAndUnload(ObjectFactory::CreateObjectFromJsonFile(gInputPath, false));
#elif defined(FUZZ_MODE_OBJECT_PARKOBJ)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        LoadAndUnload(ObjectFactory::CreateObjectFromZipFile(gInputPath, false));
#elif defined(FUZZ_MODE_PARKINFO)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        const char* argv[] = { "openrct2-cli", "parkinfo", "objects", gInputPath.c_str() };
        CommandLineRun(argv, 4);
#elif defined(FUZZ_MODE_SIMULATE)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        if (gFuzzContext->LoadParkFromFile(gInputPath))
        {
            for (int i = 0; i < 50; i++)
                gameStateUpdateLogic();
        }
#elif defined(FUZZ_MODE_SEA)
        // Context::LoadParkFromFile picks the .sea branch (DecryptSea, then the S6/park importer) by the
        // file extension, as when the game opens an RCT Classic scenario.
        if (!WriteInput(gInputPath, data, size))
            return 0;
        gFuzzContext->LoadParkFromFile(gInputPath);
#elif defined(FUZZ_MODE_SPRITE)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        std::string outDir = gScratchDir + "/export";
        const char* argv[] = { "openrct2-cli", "sprite", "exportall", gInputPath.c_str(), outDir.c_str() };
        CommandLineRun(argv, 5);
        std::error_code ec;
        std::filesystem::remove_all(outDir, ec);
#elif defined(FUZZ_MODE_LANGUAGE)
        std::string text(reinterpret_cast<const char*>(data), size);
        auto pack = LanguagePackFactory::FromText(0, text.c_str());
        if (pack != nullptr)
        {
            size_t total = 0;
            for (uint32_t id = 0; id < 0xFFFF; id++)
            {
                const utf8* s = pack->GetString(static_cast<StringId>(id));
                if (s != nullptr)
                    total += strlen(s);
            }
            (void)total;
        }
#elif defined(FUZZ_MODE_CONFIG_INI)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        Config::SetDefaults();
        Config::OpenFromPath(gInputPath);
#elif defined(FUZZ_MODE_REPLAY)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        auto* replayManager = gFuzzContext->GetReplayManager();
        try
        {
            replayManager->StartPlayback(gInputPath);
            // 10 ticks: enough to deserialise and execute the recorded game actions of the first ticks
            // while keeping the per-input cost dominated by the replay/park load, not the simulation.
            for (int i = 0; i < 10 && replayManager->IsReplaying(); i++)
            {
                gameStateUpdateLogic();
                if (replayManager->IsPlaybackStateMismatching())
                    break;
            }
        }
        catch (const std::exception&)
        {
        }
        if (replayManager->IsReplaying())
            replayManager->StopPlayback();
#elif defined(FUZZ_MODE_TITLE_SEQUENCE)
        if (!WriteInput(gInputPath, data, size))
            return 0;
        auto seq = Title::LoadTitleSequence(gInputPath);
        if (seq != nullptr)
        {
            for (size_t i = 0; i < seq->Saves.size(); i++)
            {
                auto handle = Title::TitleSequenceGetParkHandle(*seq, i);
                if (handle == nullptr || handle->Stream == nullptr)
                    continue;
                try
                {
                    auto importer = ParkImporter::Create(handle->HintPath);
                    importer->LoadFromStream(handle->Stream.get(), false, true, handle->HintPath);
                }
                catch (const std::exception&)
                {
                }
            }
        }
#endif
    }
    catch (const std::exception&)
    {
        // Parsers report malformed input by throwing (IOException, SawyerChunkException, json errors...).
    }
    return 0;
}
