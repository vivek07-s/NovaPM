// Test helper: burns about <duty> percent of one core (works in 100 ms periods).  Usage: load_gen DUTY [SECONDS]
#include <chrono>
#include <cstdlib>
#include <thread>
using namespace std::chrono;
int main(int argc, char** argv) {
    const double duty = argc > 1 ? std::atof(argv[1]) : 100.0;
    const double secs = argc > 2 ? std::atof(argv[2]) : 120.0;
    const auto end = steady_clock::now() + duration<double>(secs);
    volatile unsigned long long sink = 0;
    while (steady_clock::now() < end) {
        const auto t0 = steady_clock::now();
        const auto busy = duration<double, std::milli>(duty);          // duty % of a 100 ms period
        while (steady_clock::now() - t0 < busy) sink = sink + 1;
        if (duty < 100.0) std::this_thread::sleep_for(duration<double, std::milli>(100.0 - duty));
    }
    return 0;
}
