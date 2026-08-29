#include "TifSplitter.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <io.h>

namespace {

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
    // Shared flags - apply to whichever mode (single-file or batch) ends up running.
    bool extractChannels = false;
    bool flipY = false;
    bool normalize = true;
    int tileSize = 0;
    std::string outputDir;
    bool outputExplicit = false;
    int maxThreads = 0;
    bool showHelp = false;

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
            tileSize = std::atoi(nextArg(arg.c_str()).c_str());
        } else if (arg == "--flip-y") {
            flipY = true;
        } else if (arg == "--no-normalize") {
            normalize = false;
        } else if (arg == "--threads") {
            maxThreads = std::atoi(nextArg(arg.c_str()).c_str());
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
        options.MaxThreads = maxThreads;

        size_t lastFailedCount = 0;
        TifConverter::ProcessBatch(options, [&](const BatchProgress& p) {
            lastFailedCount = p.FailedFiles;
            std::cout << "[Progress] " << p.CompletedFiles << "/" << p.TotalFiles
                       << " (failed: " << p.FailedFiles << ") - finished: " << p.CurrentFileName << "\n";
        });

        if (_isatty(_fileno(stdin))) {
            std::cout << "\nPress Enter to exit...";
            std::cin.get();
        }
        return lastFailedCount == 0 ? 0 : 1;
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
    // Skipped when input is redirected so this never hangs a script or CI run.
    if (_isatty(_fileno(stdin))) {
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
    }

    return ok ? 0 : 1;
}
