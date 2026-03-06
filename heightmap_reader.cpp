#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <limits>

// Simple utility to load an unsigned 16-bit RAW heightmap (.r16) into memory
// and allow interactive querying of normalized height values (0..1).
//
// This is an early helper used while developing a roadmap generator that
// will later perform pathfinding (A*) over the terrain. The program expects
// a raw sequence of uint16_t values (row-major), optionally specifying that
// the file is big-endian via the `--big-endian` flag.

static bool isLittleEndianHost() {
    uint16_t v = 0x0001;
    // Write a 16-bit value with low byte 1 and test the first byte to
    // determine host byte order. If the first byte equals 1, the host is
    // little-endian.
    return *reinterpret_cast<uint8_t*>(&v) == 0x01;
}

// Byte-swap a 16-bit value. This is used when the file endianness differs
// from the host endianness to produce correct numeric values.
static inline constexpr uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

int main(int argc, char** argv) {
    // Argument parsing. Accept either command-line arguments or interactively
    // prompt the user for the required inputs. This makes it easier to run
    // the tool from the IDE without configuring command arguments.
    std::string path;
    int width = 0;
    int height = 0;
    bool fileIsBigEndian = false;

    if (argc >= 4) {
        // Use command-line arguments when provided.
        path = argv[1];
        width = std::stoi(argv[2]);
        height = std::stoi(argv[3]);
        for (int i = 4; i < argc; ++i) if (std::string(argv[i]) == "--big-endian") fileIsBigEndian = true;
    } else {
        // Interactive prompts when no sufficient command-line args are given.
        std::cout << "Please provide the file path for the r16 heightmap: ";
        if (!std::getline(std::cin, path) || path.empty()) {
            std::cerr << "Error: no path provided.\n";
            return 1;
        }

        // Default settings used for most files. Ask the user to confirm or
        // enter custom values. Default assumes little-endian and 1024x1024.
        width = 1024;
        height = 1024;
        fileIsBigEndian = false;

        std::cout << "Default settings: size=1024x1024, little-endian. Proceed? (Y/n): ";
        std::string resp;
        if (!std::getline(std::cin, resp)) {
            std::cerr << "Error: failed to read response.\n";
            return 1;
        }

        if (!resp.empty() && (resp[0] == 'n' || resp[0] == 'N')) {
            // User chooses to provide custom size and endianness.
            std::cout << "Enter width and height separated by space: ";
            std::string sizeLine;
            if (!std::getline(std::cin, sizeLine) || sizeLine.empty()) {
                std::cerr << "Error: no size provided.\n";
                return 1;
            }
            std::istringstream siss(sizeLine);
            int w = 0, h = 0;
            if (!(siss >> w >> h) || w <= 0 || h <= 0) {
                std::cerr << "Error: invalid size input.\n";
                return 1;
            }
            width = w;
            height = h;

            std::cout << "Is the file big-endian? (y/N): ";
            std::string be;
            if (std::getline(std::cin, be) && !be.empty() && (be[0] == 'y' || be[0] == 'Y')) fileIsBigEndian = true;
        }
    }

    // Validate dimensions to avoid nonsensical allocations later.
    if (width <= 0 || height <= 0) {
        std::cerr << "Error: width/height must be > 0.\n";
        return 1;
    }

    const bool hostLittle = isLittleEndianHost();
    const bool needSwap = (fileIsBigEndian && hostLittle) || (!fileIsBigEndian && !hostLittle);

    // Compute number of samples and required buffer size. Perform overflow
    // checks to avoid UB or unsuccessful allocations for very large inputs.
    const size_t sw = static_cast<size_t>(width);
    const size_t sh = static_cast<size_t>(height);
    if (sw != 0 && sh > (std::numeric_limits<size_t>::max() / sw)) {
        std::cerr << "Error: requested width*height is too large and would overflow." << "\n";
        return 1;
    }
    const size_t count = sw * sh;
    const size_t bytesNeeded = count * sizeof(uint16_t);

    // `std::istream::read` accepts a `std::streamsize` length. Ensure our
    // required byte count fits in that type.
    if (bytesNeeded > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        std::cerr << "Error: required buffer size too large for stream operations." << "\n";
        return 1;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return 1;
    }

    // Basic sanity check: confirm file has at least the expected number of
    // bytes. Prefer `std::filesystem::file_size` where available, but fall
    // back to seek/tell if it fails.
    std::error_code ec;
    const auto fsSize = std::filesystem::file_size(path, ec);
    if (!ec) {
        if (fsSize < bytesNeeded) {
            std::cerr << "Error: file too small. Expected at least " << bytesNeeded
                << " bytes, got " << fsSize << " bytes." << "\n";
            return 1;
        }
    } else {
        // Fallback to seek/tell if filesystem call failed
        in.seekg(0, std::ios::end);
        const std::streamoff fileSize = in.tellg();
        in.seekg(0, std::ios::beg);

        if (fileSize < 0 || static_cast<size_t>(fileSize) < bytesNeeded) {
            std::cerr << "Error: file too small. Expected at least " << bytesNeeded
                << " bytes, got " << fileSize << " bytes." << "\n";
            return 1;
        }
    }

    // Allocate and read all height samples into a contiguous vector.
    std::vector<uint16_t> heights(count);
    in.read(reinterpret_cast<char*>(heights.data()), static_cast<std::streamsize>(bytesNeeded));
    const std::streamsize actuallyRead = in.gcount();
    if (static_cast<size_t>(actuallyRead) != bytesNeeded) {
        std::cerr << "Error: failed to read height data. Expected " << bytesNeeded
            << " bytes, got " << actuallyRead << " bytes." << "\n";
        return 1;
    }

    // If file endianness differs from host, swap every 16-bit sample.
    if (needSwap) {
        for (auto& v : heights) v = bswap16(v);
    }

    // Compute and print the minimum and maximum height samples and their
    // coordinates (normalized to [0..1]). This is useful to quickly inspect
    // the range of the terrain before interactive sampling or A* prep.
    if (count > 0) {
        uint16_t minV = heights[0];
        uint16_t maxV = heights[0];
        size_t minIdx = 0;
        size_t maxIdx = 0;
        for (size_t i = 1; i < count; ++i) {
            const uint16_t v = heights[i];
            if (v < minV) { minV = v; minIdx = i; }
            if (v > maxV) { maxV = v; maxIdx = i; }
        }

        const size_t minX = minIdx % sw;
        const size_t minY = minIdx / sw;
        const size_t maxX = maxIdx % sw;
        const size_t maxY = maxIdx / sw;

        const float minN = static_cast<float>(minV) / 65535.0f;
        const float maxN = static_cast<float>(maxV) / 65535.0f;

        std::cout << "Min normalized height: " << minN
                  << " at (" << minX << ", " << minY << ") raw=" << minV << "\n";
        std::cout << "Max normalized height: " << maxN
                  << " at (" << maxX << ", " << maxY << ") raw=" << maxV << "\n";
    }

    // Helper that returns a normalized height value [0..1] for integer
    // coordinates. The caller is responsible for bounds checking.
    auto sample = [&](int x, int y) -> float {
        const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
        const uint16_t v = heights[idx];
        return static_cast<float>(v) / 65535.0f; // normalized 0..1
        };

    // Interactive prompt for querying single-pixel heights. This is useful
    // for manual inspection while developing the roadmap generator.
    std::cout << "Loaded " << path << " (" << width << "x" << height << ")\n";
    std::cout << "Enter coordinates as: x y   (or type 'q' to quit)\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "q" || line == "quit" || line == "exit") break;

        std::istringstream iss(line);
        int x = 0, y = 0;
        // Parse a pair of integers from the input line.
        if (!(iss >> x >> y)) {
            std::cout << "Please enter: x y  (example: 120 450)\n";
            continue;
        }

        // Bounds-check the coordinates before sampling.
        if (x < 0 || x >= width || y < 0 || y >= height) {
            std::cout << "Out of bounds. Valid x: [0.." << (width - 1)
                << "], y: [0.." << (height - 1) << "]\n";
            continue;
        }

        // Retrieve and print the normalized height.
        const float h = sample(x, y);
        std::cout << "Normalized height at (" << x << ", " << y << ") = " << h << "\n";
    }

    return 0;
}
