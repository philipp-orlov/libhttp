// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Stand-in for libFuzzer's main() for toolchains without it (GCC): runs every
// file named on the command line, then a number of mutations of the seeds
// and files. Deterministic for a given seed, so it doubles as a ctest smoke
// test; under -fsanitize=address,undefined it finds what libFuzzer would in
// the same first minutes.
//
//   fuzz_x [--iterations=N] [--seed=S] [file ...]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
// Defined by each target: inputs worth mutating.
std::vector<std::string> fuzzSeeds();

namespace {
void run(const std::string& input)
{
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::string mutate(const std::vector<std::string>& pool, std::mt19937_64& random)
{
    std::string input = pool[random() % pool.size()];
    const unsigned edits = 1 + random() % 6;
    for (unsigned edit = 0; edit < edits; ++edit) {
        switch (random() % 6) {
            case 0:  // flip a bit
                if (!input.empty())
                    input[random() % input.size()] ^= static_cast<char>(1u << (random() % 8));
                break;
            case 1:  // overwrite a byte
                if (!input.empty())
                    input[random() % input.size()] = static_cast<char>(random());
                break;
            case 2:  // insert bytes
                input.insert(random() % (input.size() + 1), 1 + random() % 4,
                             static_cast<char>(random()));
                break;
            case 3:  // delete a range
                if (!input.empty()) {
                    const size_t at = random() % input.size();
                    input.erase(at, 1 + random() % 8);
                }
                break;
            case 4:  // splice from another seed
                input.insert(random() % (input.size() + 1), pool[random() % pool.size()],
                             0, 1 + random() % 32);
                break;
            default:  // repeat a piece (long lines, many headers)
                if (!input.empty() && input.size() < 4096) {
                    const size_t at = random() % input.size();
                    const size_t length = 1 + random() % std::min<size_t>(16, input.size() - at);
                    const std::string piece = input.substr(at, length);
                    const unsigned times = 1 + random() % 64;
                    for (unsigned copy = 0; copy < times; ++copy)
                        input.insert(at, piece);
                }
                break;
        }
    }
    return input;
}
}  // namespace

int main(int argc, char** argv)
{
    size_t iterations = 3000;
    uint64_t seed = 1;
    std::vector<std::string> pool = fuzzSeeds();
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--iterations=", 0) == 0) {
            iterations = std::strtoull(argument.c_str() + 13, nullptr, 10);
        } else if (argument.rfind("--seed=", 0) == 0) {
            seed = std::strtoull(argument.c_str() + 7, nullptr, 10);
        } else {
            std::ifstream file(argument, std::ios::binary);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", argument.c_str());
                return 2;
            }
            pool.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            run(pool.back());
        }
    }
    if (pool.empty())
        pool.emplace_back();

    for (const std::string& input : pool)
        run(input);

    std::mt19937_64 random(seed);
    for (size_t iteration = 0; iteration < iterations; ++iteration)
        run(mutate(pool, random));

    std::printf("fuzz: %zu seeds, %zu mutations, no failure\n", pool.size(), iterations);
    return 0;
}
