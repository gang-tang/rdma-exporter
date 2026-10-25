#include <iostream>
#include <vector>
#include <string>
#include <filesystem> // C++17
#include <system_error>
#include <map>
#include <fstream>
#include <chrono>
#include <thread>
#include <libgen.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <cstdint>
#include <thread>
#include <cstring>
#include <sys/file.h>
#include "raw_rotating_logger.h"
#include "rdma_qp_counters.h"

namespace fs = std::filesystem;
constexpr uint32_t HIGH_RES_US = 100000;     // microsecond
constexpr uint32_t MAX_STAGING_SEC = 5;      // seconds
constexpr uint32_t LOW_RES_SEC = 1;          // seconds
constexpr uint32_t MAX_AGG_DUR_US = 4998000; // microsecond
constexpr uint32_t METRIC_EXPORT_SEC = 5;    // seconds
constexpr int MAX_RECORDS = 1000;
constexpr int MAX_AGG_COUNT = 1000;
constexpr float MAX_DEVIATION = 0.1;
constexpr float MAX_RATE_MARKER = 200;
constexpr float MIN_RATE_MARKER = 0.01;
constexpr int MAX_EXCEPTION_COUNT = 5;
const char *LOCK_PATH = "/tmp/munic_exporter.lock";
RotatingFileWriter rate_logger("/dev/shm/munic.influx", 10 * 1024 * 1024, 5);
RotatingFileWriter metric_logger("/dev/shm/munic_metric.influx", 10 * 1024 * 1024, 5);

typedef struct rdma_port_stat
{
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    std::chrono::time_point<std::chrono::high_resolution_clock> rx_time_point;
    std::chrono::time_point<std::chrono::high_resolution_clock> tx_time_point;
} rdma_port_stat_t;

typedef struct rdma_port_rate
{
    float rx_rate;
    float tx_rate;
    int64_t ts_us;
} rdma_port_rate_t;

uint64_t readUint64FromFile_old(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path);
    }

    std::string content;
    file >> content; // Read the number as a string
    file.close();

    try
    {
        uint64_t value = std::stoull(content); // Convert to uint64_t
        return value;
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("Invalid uint64_t value in file: " + path + " (" + e.what() + ")");
    }
}

uint64_t readUint64FromFile(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("Failed to open file: %s\n", path);
        return 0;
    }
    char buf[32];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        printf("Empty file: %s\n", path);
        return 0;
    }

    buf[n] = '\0';

    uint64_t value = 0;
    for (int i = 0; i < n; i++)
    {
        char c = buf[i];
        if (c < '0' || c > '9')
            break;
        value = value * 10 + (c - '0');
    }

    return value;
}

void printNow(std::string p)
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto us = duration_cast<microseconds>(now.time_since_epoch()) % 1000000;
    std::cout << p << us.count() << '\n';
}

std::vector<std::string> getInfinibandDevices()
{
    const std::string infinibandPath = "/sys/class/infiniband";
    std::vector<std::string> devices;
    if (std::filesystem::exists(infinibandPath))
    {
        for (const auto &entry : std::filesystem::directory_iterator(infinibandPath))
        {
            if (entry.is_directory())
            {
                std::string deviceName = entry.path().filename().string();
                devices.push_back(deviceName);
            }
        }
    }
    return devices;
}

void exportInfinibandCounters()
{
    char dirname[256];
    char filename[256];
    char exportcontent[4096];
    const std::string infinibandPath = "/sys/class/infiniband";
    for (const auto &entry : std::filesystem::directory_iterator(infinibandPath))
    {
        if (entry.is_directory())
        {
            std::string deviceName = entry.path().filename().string();
            sprintf(dirname, "/sys/class/infiniband/%s/ports/1/counters", deviceName.c_str());
            int offset = 0;
            int num = 0;
            // ib_counters,ib_device=mlx5_1:1 rx_rate=0.02,tx_rate=23.30 1762504959175000000
            offset += sprintf(exportcontent + offset, "ib_native_counters,ib_device=%s", deviceName.c_str());
            for (const auto &centry : std::filesystem::directory_iterator(dirname))
            {
                if (centry.is_regular_file())
                {
                    std::string counterName = centry.path().filename().string();
                    sprintf(filename, "/sys/class/infiniband/%s/ports/1/counters/%s", deviceName.c_str(), counterName.c_str());
                    auto val = readUint64FromFile(filename);
                    if (val != 0)
                    {
                        if (num == 0)
                        {
                            offset += sprintf(exportcontent + offset, " %s=%ld", counterName.c_str(), val);
                        }
                        else
                        {
                            offset += sprintf(exportcontent + offset, ",%s=%ld", counterName.c_str(), val);
                        }
                        num++;
                    }
                }
            }
            if (num > 0)
            {
                auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                offset += sprintf(exportcontent + offset, " %ld\n", ts_us * 1000);
                metric_logger.write(exportcontent, strlen(exportcontent));
            }

            sprintf(dirname, "/sys/class/infiniband/%s/ports/1/hw_counters", deviceName.c_str());
            offset = 0;
            num = 0;
            // ib_counters,ib_device=mlx5_1:1 rx_rate=0.02,tx_rate=23.30 1762504959175000000
            offset += sprintf(exportcontent + offset, "ib_native_hwcounters,ib_device=%s", deviceName.c_str());
            for (const auto &centry : std::filesystem::directory_iterator(dirname))
            {
                if (centry.is_regular_file())
                {
                    std::string counterName = centry.path().filename().string();
                    sprintf(filename, "/sys/class/infiniband/%s/ports/1/hw_counters/%s", deviceName.c_str(), counterName.c_str());
                    auto val = readUint64FromFile(filename);
                    if (val != 0)
                    {
                        if (num == 0)
                        {
                            offset += sprintf(exportcontent + offset, " %s=%ld", counterName.c_str(), val);
                        }
                        else
                        {
                            offset += sprintf(exportcontent + offset, ",%s=%ld", counterName.c_str(), val);
                        }
                        num++;
                    }
                }
            }
            if (num > 0)
            {
                auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                offset += sprintf(exportcontent + offset, " %ld\n", ts_us * 1000);
                metric_logger.write(exportcontent, strlen(exportcontent));
            }

            fs::path netPath = entry.path() / "device/net";
            if (fs::exists(netPath) && fs::is_directory(netPath))
            {
                for (const auto &netEntry : fs::directory_iterator(netPath))
                {
                    if (!netEntry.is_directory())
                    {
                        continue;
                    }

                    std::string netDeviceName = netEntry.path().filename().string();
                    fs::path carrierPath = netEntry.path() / "carrier";
                    if (!fs::exists(carrierPath))
                    {
                        continue;
                    }

                    auto carrier = readUint64FromFile(carrierPath.c_str());
                    auto carrierChanges = readUint64FromFile((netEntry.path() / "carrier_changes").c_str());
                    auto carrierDownCount = readUint64FromFile((netEntry.path() / "carrier_down_count").c_str());
                    auto carrierUpCount = readUint64FromFile((netEntry.path() / "carrier_up_count").c_str());
                    int carrierOffset = sprintf(
                        exportcontent,
                        "ib_net_carrier,ib_device=%s,net_device=%s carrier=%llu,carrier_changes=%llu,carrier_down_count=%llu,carrier_up_count=%llu",
                        deviceName.c_str(), netDeviceName.c_str(),
                        static_cast<unsigned long long>(carrier),
                        static_cast<unsigned long long>(carrierChanges),
                        static_cast<unsigned long long>(carrierDownCount),
                        static_cast<unsigned long long>(carrierUpCount));
                    auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::high_resolution_clock::now().time_since_epoch())
                                     .count();
                    carrierOffset += sprintf(exportcontent + carrierOffset, " %ld\n", ts_us * 1000);
                    metric_logger.write(exportcontent, strlen(exportcontent));
                }
            }
        }
    }
}

void CalculateRate(const char *port_name)
{
    char tx_path[256];
    char rx_path[256];
    int next_index = 0;
    char content[256];
    float rx_rate, tx_rate;
    uint64_t tx_bytes = 0;
    uint64_t rx_bytes = 0;
    int agg_count = 0;
    float rx_agg = 0.0;
    float tx_agg = 0.0;
    int64_t agg_last_us = 0;
    int64_t agg_start_us = 0;
    float last_export_tx_rate = 0.0;
    float last_export_rx_rate = 0.0;
    int64_t last_export_us = 0;
    rdma_port_stat_t port_stat{};
    rdma_port_rate_t port_rates[MAX_RECORDS]{};
    std::chrono::time_point<std::chrono::high_resolution_clock> tx_time{};
    std::chrono::time_point<std::chrono::high_resolution_clock> rx_time{};
    std::chrono::time_point<std::chrono::high_resolution_clock> last_check_time{};
    float max_port_rate = MAX_RATE_MARKER;
    int exception = 0;

    // ib_counters,ib_device=mlx5_1:1 rx_rate=0.02,tx_rate=23.30 1762504959175000000
    auto export_rates = [&](const char *dev_name, float rx, float tx, int64_t ts_us)
    {
        sprintf(content, "ib_counters,ib_device=%s rx_rate=%.2f,tx_rate=%.2f %ld\n", dev_name,
                rx, tx, ts_us * 1000);
        rate_logger.write(content, strlen(content));
        last_export_tx_rate = tx;
        last_export_rx_rate = rx;
        last_export_us = ts_us;
        agg_count = 0;
        rx_agg = 0.0;
        tx_agg = 0.0;
        agg_last_us = -1;
        agg_start_us = -1;
    };

    if (port_name == NULL)
    {
        return;
    }
    printf("start monitoring %s\n", port_name);
    sprintf(tx_path, "/sys/class/infiniband/%s/ports/1/counters/port_xmit_data", port_name);
    sprintf(rx_path, "/sys/class/infiniband/%s/ports/1/counters/port_rcv_data", port_name);
    last_check_time = std::chrono::high_resolution_clock::now();
    if (strncmp(port_name, "mlx5_", strlen("mlx5_")) == 0) {
        char rate_path[256];
        uint64_t rate;
        sprintf(rate_path, "/sys/class/infiniband/%s/ports/1/rate", port_name);
        rate = readUint64FromFile(rate_path);
        if (rate > 0) {
            max_port_rate = rate;
        }
    }
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(HIGH_RES_US));
        tx_bytes = 4 * readUint64FromFile(tx_path); // word to bytes
        if (tx_bytes == 0)
        {
            std::this_thread::sleep_for(std::chrono::seconds(LOW_RES_SEC));
            continue;
        }
        tx_time = std::chrono::high_resolution_clock::now();

        rx_bytes = 4 * readUint64FromFile(rx_path); // word to bytes
        if (rx_bytes == 0)
        {
            std::this_thread::sleep_for(std::chrono::seconds(LOW_RES_SEC));
            continue;
        }
        rx_time = std::chrono::high_resolution_clock::now();

        if (port_stat.tx_bytes != 0 && port_stat.tx_bytes <= tx_bytes)
        {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(tx_time - port_stat.tx_time_point).count();
            //  printf("tx%ld:%ld\n", elapsed_us, bytes - port_stat.tx_bytes);
            if (elapsed_us > 0)
            {
                tx_rate = 1.0 * (tx_bytes - port_stat.tx_bytes) / elapsed_us * 8 / 1000; // Gbps
            }
            else
            {
                tx_rate = max_port_rate;
            }
            // printf("%f\n", tx_rate);
        }
        else
        {
            tx_rate = MIN_RATE_MARKER;
        }

        if (port_stat.rx_bytes != 0 && port_stat.rx_bytes <= rx_bytes)
        {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(rx_time - port_stat.rx_time_point).count();
            //  printf("rx%ld:%ld\n", elapsed_us, bytes - port_stat.rx_bytes);
            if (elapsed_us > 0)
            {
                rx_rate = 1.0 * (rx_bytes - port_stat.rx_bytes) / elapsed_us * 8 / 1000; // Gbps
            }
            else
            {
                rx_rate = max_port_rate;
            }
        }
        else
        {
            rx_rate = MIN_RATE_MARKER;
        }
        if (exception < MAX_EXCEPTION_COUNT && (rx_rate >= max_port_rate || tx_rate >= max_port_rate))
        {
            exception++;
            continue;
        }
        exception = 0;
        port_stat.tx_bytes = tx_bytes;
        port_stat.tx_time_point = tx_time;
        port_stat.rx_bytes = rx_bytes;
        port_stat.rx_time_point = rx_time;

        port_rates[next_index].tx_rate = tx_rate;
        port_rates[next_index].rx_rate = rx_rate;
        port_rates[next_index].ts_us = std::chrono::duration_cast<std::chrono::microseconds>(rx_time.time_since_epoch()).count();
        next_index++;
        auto check_time = std::chrono::high_resolution_clock::now();
        auto dur_sec = std::chrono::duration_cast<std::chrono::seconds>(check_time - last_check_time).count();
        if (next_index == MAX_RECORDS || dur_sec >= MAX_STAGING_SEC)
        {
            for (int i = 0; i < next_index; i++)
            {
                auto rx_rate = port_rates[i].rx_rate;
                auto tx_rate = port_rates[i].tx_rate;
                if (agg_count > 0)
                {
                    auto rx_agg_new = (rx_agg + rx_rate) / (agg_count + 1);
                    auto tx_agg_new = (tx_agg + tx_rate) / (agg_count + 1);
                    if (rx_rate > rx_agg_new * (1 + MAX_DEVIATION) ||
                        rx_rate < rx_agg_new * (1 - MAX_DEVIATION) ||
                        tx_rate > tx_agg_new * (1 + MAX_DEVIATION) ||
                        tx_rate < tx_agg_new * (1 - MAX_DEVIATION))
                    {
                        // printf("ts:%ld rx_rate:%f tx_rate:%f\n", agg_last_us/1000, rx_agg / agg_count, tx_agg / agg_count);
                        // ib_counters,ib_device=mlx5_1:1 rx_rate=0.02,tx_rate=23.30 1762504959175000000
                        export_rates(port_name, rx_agg / agg_count, tx_agg / agg_count, agg_last_us);
                        export_rates(port_name, rx_rate, tx_rate, port_rates[i].ts_us);
                        continue;
                    }
                }
                else if (agg_count == 0 && last_export_us != 0)
                {
                    if (rx_rate > last_export_rx_rate * (1 + MAX_DEVIATION) ||
                        rx_rate < last_export_rx_rate * (1 - MAX_DEVIATION) ||
                        tx_rate > last_export_tx_rate * (1 + MAX_DEVIATION) ||
                        tx_rate < last_export_tx_rate * (1 - MAX_DEVIATION))
                    {
                        export_rates(port_name, rx_rate, tx_rate, port_rates[i].ts_us);
                        continue;
                    }
                }
                agg_count += 1;
                rx_agg += rx_rate;
                tx_agg += tx_rate;
                agg_last_us = port_rates[i].ts_us;
                if (agg_count == 1)
                {
                    agg_start_us = agg_last_us;
                }
                if (agg_last_us - agg_start_us >= MAX_AGG_DUR_US)
                {
                    // printf("last_ts:%ld, start_ts:%ld\n", agg_last_us, agg_start_us);
                    // ib_counters,ib_device=mlx5_1:1 rx_rate=0.02,tx_rate=23.30 1762504959175000000
                    export_rates(port_name, rx_agg / agg_count, tx_agg / agg_count, agg_last_us);
                }
            }
            last_check_time = check_time;
            next_index = 0;
        }
    }
}

void MetricExporter()
{
    while (true)
    {
        exportInfinibandCounters();
        exportRdmaQpCounters(metric_logger);
        std::this_thread::sleep_for(std::chrono::seconds(METRIC_EXPORT_SEC));
    }
}

int main(int argc, char **argv)
{
    std::vector<std::thread> threads;

    int fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0666);
    if (fd == -1)
    {
        std::cerr << "Failed to open lock file\n";
        return 1;
    }

    // Try to get exclusive lock (non-blocking)
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        std::cerr << "Another instance is already running.\n";
        return 1;
    }

    auto devices = getInfinibandDevices();
    for (const auto &device : devices)
    {
        threads.push_back(std::thread(CalculateRate, device.c_str()));
    }
    
    threads.push_back(std::thread(MetricExporter));
    for (auto &t : threads)
    {
        t.join();
    }
    return 0;
}
