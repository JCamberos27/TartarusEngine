#include "TifSplitter.h"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <io.h>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// #185: std::atoi turned garbage ("--tile-size big") into 0 silently. Whole-string parse with
// a range check; exits with a message instead.
int ParseIntArg(const std::string& flag, const std::string& value, int minValue, int maxValue) {
    int result = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    auto [ptr, ec] = std::from_chars(first, last, result);
    if (ec != std::errc() || ptr != last || result < minValue || result > maxValue) {
        std::cerr << "[Error] " << flag << " expects a whole number from " << minValue << " to "
                  << maxValue << ", got '" << value << "'.\n";
        std::exit(1);
    }
    return result;
}

void PrintUsage(const char* exeName) {
    std::cout <<
        "TifSplitter - converts TIFF texture sheets or heightmaps into engine-ready 8-bit PNGs\n\n"
        "Single-file usage:\n"
        "  " << exeName << " -i <input.tif> [-o <output_dir>] [options]\n\n"
        "Batch usage:\n"
        "  " << exeName << " -d <input_dir> [-o <output_dir>] [--recursive] [options]\n"
        "  " << exeName << " -f <a.tif> -f <b.tif> [-f <c.tif> ...] [-o <output_dir>] [options]\n\n"
        "Batch-only options:\n"
        "  -d, --dir <path>        Convert every .tif/.tiff directly inside this folder\n"
        "  --recursive             Also descend into -d's subfolders (output mirrors\n"
        "                          the same subfolder structure under -o)\n"
        "  -f, --files <path>      Add one file to the batch - repeat for more\n"
        "                          (-f a.tif -f b.tif -f c.tif)\n"
        "  --threads <N>           Max files converted concurrently (default: 0 =\n"
        "                          hardware concurrency)\n\n"
        "Shared options (apply to single-file and batch modes alike):\n"
        "  -o, --output <dir>      Output directory (default: ./Output, or next to\n"
        "                          the source file for a drag-and-dropped single file)\n"
        "  --split-channels        Also write Input_R/G/B/A.png (e.g. a packed\n"
        "                          Metallic/Roughness/AO/Smoothness sheet)\n"
        "  --tile-size <N>         Split into an N x N grid of PNGs instead of one\n"
        "                          image (e.g. --tile-size 1024 on an 8192x8192\n"
        "                          heightmap) - 0 (default) writes a single PNG\n"
        "  --flip-y                Invert the Green channel: DirectX -> OpenGL\n"
        "                          normal map convention\n"
        "  --no-normalize          Don't contrast-stretch split channels to fill\n"
        "                          0..255 (on by default, since heightmap data\n"
        "                          rarely uses its full source bit range)\n"
        "  --16bit                 Write split channels of a 16/32/64-bit source as\n"
        "                          16-bit PNGs (full heightmap precision, no\n"
        "                          terracing). Needs --split-channels\n"
        "  --pause                 Wait for Enter before exiting (default: only on a\n"
        "                          bare drag-and-drop launch, never for an explicit CLI run)\n"
        "  -h, --help              Show this message\n\n"
        "Examples:\n"
        "  " << exeName << " -i Terrain_Height.tif -o Output --tile-size 1024\n"
        "  " << exeName << " -d ./RawTextures -o ./ProcessedTextures --recursive\n"
        "  " << exeName << " -f Rock_A.tif -f Rock_B.tif -o Output --split-channels\n\n"
        "You can also just drag .tif file(s) onto this exe (or a shortcut to it) -\n"
        "output goes into an \"Output\" folder next to the source file(s). Dropping more\n"
        "than one file at once runs them as a batch.\n";
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // The embedded manifest sets the process code page to UTF-8 (#185 / #149), so argv, libtiff
    // and every std::string path are UTF-8; make the console print them that way too.
    SetConsoleOutputCP(CP_UTF8);
#endif
    // Shared flags - apply to whichever mode (single-file or batch) ends up running.
    bool extractChannels = false;
    bool flipY = false;
    bool normalize = true;
    bool output16 = false;
    int tileSize = 0;
    std::string outputDir;
    bool outputExplicit = false;
    int maxThreads = 0;
    bool showHelp = false;
    bool pauseExplicit = false; // --pause: force the "Press Enter" wait even for an explicit CLI run
    bool sawDashedArg = false;  // any -x / --x present => treat as an explicit CLI invocation, not a drop

    // Single-file mode inputs.
    std::string singleInput;
    std::vector<std::string> positionalInputs; // bare paths, e.g. files dropped onto this exe
                                                // (or a shortcut to it) in Explorer - Windows
                                                // passes each dropped file as its own argv entry,
                                                // so dropping several files means several of these

    // Batch mode inputs.
    std::vector<std::filesystem::path> batchFiles;
    std::string batchDir;
    bool recursive = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!arg.empty() && arg[0] == '-') sawDashedArg = true;
        auto nextArg = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[Error] " << flag << " requires a value.\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-i" || arg == "--input") {
            singleInput = nextArg(arg.c_str());
        } else if (arg == "-d" || arg == "--dir") {
            batchDir = nextArg(arg.c_str());
        } else if (arg == "-f" || arg == "--files") {
            batchFiles.push_back(nextArg(arg.c_str()));
        } else if (arg == "--recursive") {
            recursive = true;
        } else if (arg == "-o" || arg == "--output") {
            outputDir = nextArg(arg.c_str());
            outputExplicit = true;
        } else if (arg == "--split-channels") {
            extractChannels = true;
        } else if (arg == "--tile-size") {
            tileSize = ParseIntArg(arg, nextArg(arg.c_str()), 0, 1 << 20);
        } else if (arg == "--flip-y") {
            flipY = true;
        } else if (arg == "--no-normalize") {
            normalize = false;
        } else if (arg == "--16bit") {
            output16 = true;
        } else if (arg == "--pause") {
            pauseExplicit = true;
        } else if (arg == "--threads") {
            maxThreads = ParseIntArg(arg, nextArg(arg.c_str()), 0, 1024);
        } else if (arg == "-h" || arg == "--help") {
            showHelp = true;
        } else if (!arg.empty() && arg[0] != '-') {
            // Not a recognized flag and doesn't look like one - this is what Windows passes
            // when one or more files are dropped onto this exe or a shortcut to it (each
            // dropped file is its own bare argv entry, no -i).
            positionalInputs.push_back(arg);
        } else {
            std::cerr << "[Error] Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (showHelp) {
        PrintUsage(argv[0]);
        return 0;
    }
    if (output16 && !extractChannels) {
        std::cerr << "[Warn] --16bit only applies to --split-channels output; ignoring it.\n";
    }

    // "Press Enter to exit" is only useful when the window would otherwise vanish before the
    // result can be read — i.e. an Explorer drag-and-drop launch (bare paths, no flags). An
    // explicit CLI invocation should just return to the prompt. `--pause` forces it either way;
    // redirected stdin never pauses (would hang scripts/CI). (#126)
    const bool dragDropLaunch = !sawDashedArg && !positionalInputs.empty();
    const bool shouldPause = _isatty(_fileno(stdin)) && (pauseExplicit || dragDropLaunch);

    // Dropping several files onto the exe/shortcut at once (as opposed to just one) means
    // Windows handed us multiple bare positional args - route those into batch mode too, not
    // just the explicit -d/-f flags, otherwise every file after the first is silently ignored.
    bool droppedMultipleFiles = singleInput.empty() && positionalInputs.size() > 1;
    bool batchMode = !batchDir.empty() || !batchFiles.empty() || droppedMultipleFiles;

    if (batchMode) {
        if (droppedMultipleFiles) {
            for (const auto& p : positionalInputs) batchFiles.push_back(p);
        }

        BatchTifOptions options;
        options.InputFiles = batchFiles;
        options.InputDirectory = batchDir;
        if (outputExplicit) {
            options.OutputDirectory = outputDir;
        } else if (droppedMultipleFiles) {
            // Same reasoning as the single-file drag-drop default below: the process's working
            // directory is unpredictable when launched this way, so default next to the
            // dropped files instead of an unhelpful "./Output".
            options.OutputDirectory = (std::filesystem::path(positionalInputs.front()).parent_path() / "Output").string();
        } else {
            options.OutputDirectory = "./Output";
        }
        options.Recursive = recursive;
        options.ExtractIndividualChannels = extractChannels;
        options.FlipNormalY = flipY;
        options.NormalizeHeightmaps = normalize;
        options.TileSize = tileSize;
        options.Output16Bit = output16;
        options.MaxThreads = maxThreads;

        // #194 — the callback runs on worker threads; the exit code comes from the joined result.
        const BatchProgress result = TifConverter::ProcessBatch(options, [&](const BatchProgress& p) {
            std::cout << "[Progress] " << p.CompletedFiles << "/" << p.TotalFiles
                       << " (failed: " << p.FailedFiles << ") - finished: " << p.CurrentFileName << "\n";
        });

        if (shouldPause) {
            std::cout << "\nPress Enter to exit...";
            std::cin.get();
        }
        // #193 — converting nothing is a failure (exit 2), not success.
        if (result.TotalFiles == 0) return 2;
        return result.FailedFiles == 0 ? 0 : 1;
    }

    // Single-file mode.
    TifSplitterOptions options;
    options.InputFilePath = singleInput.empty()
        ? (positionalInputs.empty() ? std::string() : positionalInputs.front())
        : singleInput;
    options.ExtractIndividualChannels = extractChannels;
    options.FlipNormalY = flipY;
    options.NormalizeHeightmaps = normalize;
    options.TileSize = tileSize;
    options.Output16Bit = output16;

    if (outputExplicit) {
        options.OutputDirectory = outputDir;
    } else if (!options.InputFilePath.empty()) {
        // Defaults to a folder next to the SOURCE file rather than "./Output" relative to the
        // process's working directory - when launched by dragging a file onto a shortcut, that
        // working directory is unpredictable and "./Output" could land somewhere the user would
        // never think to look.
        std::filesystem::path inputPath(options.InputFilePath);
        options.OutputDirectory = (inputPath.parent_path() / "Output").string();
    }

    if (options.InputFilePath.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    bool ok = TifConverter::ProcessTif(options);

    // Only when running interactively (a real console, not piped/redirected input) - a drag-
    // and-drop launch would otherwise flash the result and close before it can be read.
    // Skipped for an explicit CLI run and for redirected stdin (see shouldPause above, #126).
    if (shouldPause) {
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
    }

    return ok ? 0 : 1;
}
