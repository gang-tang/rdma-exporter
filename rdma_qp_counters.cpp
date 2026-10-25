#include "rdma_qp_counters.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace
{
constexpr char kQpCommand[] = "rdma resource show qp 2>/dev/null";

struct qp_state_counts
{
    uint64_t total = 0;
    std::map<std::string, uint64_t> states;
};

// "link mrdma0/1 lqpn 1 type GSI state RTS sq-psn 0 comm ib_core"
void countQpLine(const std::string &line, std::map<std::string, qp_state_counts> &by_device)
{
    std::istringstream stream(line);
    std::string token, value, device, state;

    while (stream >> token)
    {
        if (token == "link" && (stream >> value))
        {
            auto slash = value.find('/');
            device = slash == std::string::npos ? value : value.substr(0, slash);
        }
        else if (token == "state" && (stream >> value))
        {
            state = value;
        }
        // stop before "comm", a process name may look like a field keyword
        if (!device.empty() && !state.empty())
        {
            break;
        }
    }

    if (device.empty() || state.empty())
    {
        return;
    }

    auto &counts = by_device[device];
    counts.total++;
    counts.states[state]++;
}
} // namespace

void exportRdmaQpCounters(RotatingFileWriter &logger)
{
    std::map<std::string, qp_state_counts> by_device;

    FILE *pipe = popen(kQpCommand, "r");
    if (pipe == nullptr)
    {
        return;
    }

    std::array<char, 1024> line{};
    bool in_oversized_line = false;
    while (fgets(line.data(), static_cast<int>(line.size()), pipe) != nullptr)
    {
        size_t len = strlen(line.data());
        bool line_complete = len > 0 && line[len - 1] == '\n';
        if (!in_oversized_line && line_complete)
        {
            countQpLine(line.data(), by_device);
        }
        in_oversized_line = !line_complete;
    }
    pclose(pipe);

    if (by_device.empty())
    {
        return;
    }

    auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

    for (const auto &entry : by_device)
    {
        std::ostringstream content;
        content << "ib_qp_counters,ib_device=" << entry.first << " total=" << entry.second.total;
        for (const auto &state : entry.second.states)
        {
            content << "," << state.first << "=" << state.second;
        }
        content << " " << ts_ns << "\n";

        logger.write(content.str());
    }
}
