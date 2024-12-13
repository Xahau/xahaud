//
#ifndef RIPPLE_APP_MAIN_DATAGRAMMONITOR_H_INCLUDED
#define RIPPLE_APP_MAIN_DATAGRAMMONITOR_H_INCLUDED

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <ripple/basics/UptimeClock.h>
#include <ripple/ledger/CachedSLEs.h>
#include <ripple/nodestore/Database.h>
#include <ripple/nodestore/DatabaseShard.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/protocol/BuildInfo.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/shamap/ShardFamily.h>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <ifaddrs.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#include <thread>
#include <vector>

namespace ripple {

// Magic number for server info packets: 'XDGM' (le) Xahau DataGram Monitor
constexpr uint32_t SERVER_INFO_MAGIC = 0x4D474458;
constexpr uint32_t SERVER_INFO_VERSION = 1;

// Warning flag bits
constexpr uint32_t WARNING_AMENDMENT_BLOCKED = 1 << 0;
constexpr uint32_t WARNING_UNL_BLOCKED = 1 << 1;
constexpr uint32_t WARNING_AMENDMENT_WARNED = 1 << 2;
constexpr uint32_t WARNING_NOT_SYNCED = 1 << 3;

// Time window statistics for rates
struct [[gnu::packed]] MetricRates
{
    double rate_1m;   // Average rate over last minute
    double rate_5m;   // Average rate over last 5 minutes
    double rate_1h;   // Average rate over last hour
    double rate_24h;  // Average rate over last 24 hours
};

struct AllRates
{
    MetricRates network_in;
    MetricRates network_out;
    MetricRates disk_read;
    MetricRates disk_write;
};

// Structure to represent a ledger sequence range
struct [[gnu::packed]] LgrRange
{
    uint32_t start;
    uint32_t end;
};

// Map is returned separately since variable-length data
// shouldn't be included in network structures
using ObjectCountMap = std::vector<std::pair<std::basic_string<char>, int>>;

struct [[gnu::packed]] DebugCounters
{
    // Database metrics
    std::uint64_t dbKBTotal{0};
    std::uint64_t dbKBLedger{0};
    std::uint64_t dbKBTransaction{0};
    std::uint64_t localTxCount{0};

    // Basic metrics
    std::uint32_t writeLoad{0};
    std::int32_t historicalPerMinute{0};

    // Cache metrics
    std::uint32_t sleHitRate{0};  // Stored as fixed point, multiplied by 1000
    std::uint32_t ledgerHitRate{
        0};  // Stored as fixed point, multiplied by 1000
    std::uint32_t alSize{0};
    std::uint32_t alHitRate{0};  // Stored as fixed point, multiplied by 1000
    std::int32_t fullbelowSize{0};
    std::uint32_t treenodeCacheSize{0};
    std::uint32_t treenodeTrackSize{0};

    // Shard metrics
    std::int32_t shardFullbelowSize{0};
    std::uint32_t shardTreenodeCacheSize{0};
    std::uint32_t shardTreenodeTrackSize{0};
    std::uint32_t shardWriteLoad{0};
    std::uint64_t shardNodeWrites{0};
    std::uint64_t shardNodeReadsTotal{0};
    std::uint64_t shardNodeReadsHit{0};
    std::uint64_t shardNodeWrittenBytes{0};
    std::uint64_t shardNodeReadBytes{0};

    // Node store metrics (when not using shards)
    std::uint64_t nodeWriteCount{0};
    std::uint64_t nodeWriteSize{0};
    std::uint64_t nodeFetchCount{0};
    std::uint64_t nodeFetchHitCount{0};
    std::uint64_t nodeFetchSize{0};
};

// Core server metrics in the fixed header
struct [[gnu::packed]] ServerInfoHeader
{
    // Fixed header fields come first
    uint32_t magic;               // Magic number to identify packet type
    uint32_t version;             // Protocol version number
    uint32_t network_id;          // Network ID from config
    uint32_t server_state;        // Operating mode as enum
    uint32_t peer_count;          // Number of connected peers
    uint32_t node_size;           // Size category (0=tiny through 4=huge)
    uint32_t cpu_cores;           // CPU core count
    uint32_t ledger_range_count;  // Number of range entries
    uint32_t warning_flags;       // Warning flags (reduced size)

    uint32_t padding_1;  // padding for alignment

    // 64-bit metrics
    uint64_t timestamp;          // System time in microseconds
    uint64_t uptime;             // Server uptime in seconds
    uint64_t io_latency_us;      // IO latency in microseconds
    uint64_t validation_quorum;  // Validation quorum count
    uint64_t fetch_pack_size;    // Size of fetch pack cache
    uint64_t proposer_count;     // Number of proposers in last close
    uint64_t converge_time_ms;   // Last convergence time in ms
    uint64_t load_factor;        // Load factor (scaled by 1M)
    uint64_t load_base;          // Load base value
    uint64_t reserve_base;       // Reserve base amount
    uint64_t reserve_inc;        // Reserve increment amount
    uint64_t ledger_seq;         // Latest ledger sequence

    // Fixed-size byte arrays
    uint8_t ledger_hash[32];      // Latest ledger hash
    uint8_t node_public_key[33];  // Node's public key
    uint8_t padding2[7];          // Padding to maintain 8-byte alignment
    uint8_t version_string[32];

    // System metrics
    uint64_t process_memory_pages;  // Process memory usage in bytes
    uint64_t system_memory_total;   // Total system memory in bytes
    uint64_t system_memory_free;    // Free system memory in bytes
    uint64_t system_memory_used;    // Used system memory in bytes
    uint64_t system_disk_total;     // Total disk space in bytes
    uint64_t system_disk_free;      // Free disk space in bytes
    uint64_t system_disk_used;      // Used disk space in bytes
    uint64_t io_wait_time;          // IO wait time in milliseconds
    double load_avg_1min;           // 1 minute load average
    double load_avg_5min;           // 5 minute load average
    double load_avg_15min;          // 15 minute load average

    // State transition metrics
    uint64_t state_transitions[5];  // Count for each operating mode
    uint64_t state_durations[5];    // Duration in each mode
    uint64_t initial_sync_us;       // Initial sync duration

    // Network and disk rates remain unchanged
    struct
    {
        MetricRates network_in;
        MetricRates network_out;
        MetricRates disk_read;
        MetricRates disk_write;
    } rates;

    DebugCounters dbg_counters;
};

// System metrics collected for rate calculations
struct SystemMetrics
{
    uint64_t timestamp;           // When metrics were collected
    uint64_t network_bytes_in;    // Current total bytes in
    uint64_t network_bytes_out;   // Current total bytes out
    uint64_t disk_bytes_read;     // Current total bytes read
    uint64_t disk_bytes_written;  // Current total bytes written
};

class MetricsTracker
{
private:
    static constexpr size_t SAMPLES_1M = 60;    // 1 sample/second for 1 minute
    static constexpr size_t SAMPLES_5M = 300;   // 1 sample/second for 5 minutes
    static constexpr size_t SAMPLES_1H = 3600;  // 1 sample/second for 1 hour
    static constexpr size_t SAMPLES_24H = 1440;  // 1 sample/minute for 24 hours

    std::vector<SystemMetrics> samples_1m{SAMPLES_1M};
    std::vector<SystemMetrics> samples_5m{SAMPLES_5M};
    std::vector<SystemMetrics> samples_1h{SAMPLES_1H};
    std::vector<SystemMetrics> samples_24h{SAMPLES_24H};

    size_t index_1m{0}, index_5m{0}, index_1h{0}, index_24h{0};
    std::chrono::system_clock::time_point last_24h_sample{};

    double
    calculateRate(
        const SystemMetrics& current,
        const std::vector<SystemMetrics>& samples,
        size_t current_index,
        size_t max_samples,
        bool is_24h_window,
        std::function<uint64_t(const SystemMetrics&)> metric_getter)
    {
        // If we don't have at least 2 samples, the rate is 0
        if (current_index < 2)
        {
            return 0.0;
        }

        // Calculate time window based on the window type
        uint64_t expected_window_micros;
        if (is_24h_window)
        {
            expected_window_micros =
                24ULL * 60ULL * 60ULL * 1000000ULL;  // 24 hours in microseconds
        }
        else
        {
            expected_window_micros = max_samples *
                1000000ULL;  // window in seconds * 1,000,000 for microseconds
        }

        // For any window where we don't have full data, we should scale the
        // rate based on the actual time we have data for
        uint64_t actual_window_micros =
            current.timestamp - samples[0].timestamp;
        double window_scale = std::min(
            1.0,
            static_cast<double>(actual_window_micros) / expected_window_micros);

        // Get the oldest valid sample
        size_t oldest_index = (current_index >= max_samples)
            ? ((current_index + 1) % max_samples)
            : 0;
        const auto& oldest = samples[oldest_index];

        double elapsed = actual_window_micros /
            1000000.0;  // Convert microseconds to seconds

        // Ensure we have a meaningful time difference
        if (elapsed < 0.001)
        {  // Less than 1ms difference
            return 0.0;
        }

        uint64_t current_value = metric_getter(current);
        uint64_t oldest_value = metric_getter(oldest);

        // Handle counter wraparound
        uint64_t diff = (current_value >= oldest_value)
            ? (current_value - oldest_value)
            : (std::numeric_limits<uint64_t>::max() - oldest_value +
               current_value + 1);

        // Calculate the rate and scale it based on our window coverage
        return (static_cast<double>(diff) / elapsed) * window_scale;
    }

    MetricRates
    calculateMetricRates(
        const SystemMetrics& current,
        std::function<uint64_t(const SystemMetrics&)> metric_getter)
    {
        MetricRates rates;
        rates.rate_1m = calculateRate(
            current, samples_1m, index_1m, SAMPLES_1M, false, metric_getter);
        rates.rate_5m = calculateRate(
            current, samples_5m, index_5m, SAMPLES_5M, false, metric_getter);
        rates.rate_1h = calculateRate(
            current, samples_1h, index_1h, SAMPLES_1H, false, metric_getter);
        rates.rate_24h = calculateRate(
            current, samples_24h, index_24h, SAMPLES_24H, true, metric_getter);
        return rates;
    }

public:
    void
    addSample(const SystemMetrics& metrics)
    {
        auto now = std::chrono::system_clock::now();

        // Update 1-minute window (every second)
        samples_1m[index_1m++ % SAMPLES_1M] = metrics;

        // Update 5-minute window (every second)
        samples_5m[index_5m++ % SAMPLES_5M] = metrics;

        // Update 1-hour window (every second)
        samples_1h[index_1h++ % SAMPLES_1H] = metrics;

        // Update 24-hour window (every minute)
        if (last_24h_sample + std::chrono::minutes(1) <= now)
        {
            samples_24h[index_24h++ % SAMPLES_24H] = metrics;
            last_24h_sample = now;
        }
    }

    AllRates
    getRates(const SystemMetrics& current)
    {
        AllRates rates;
        rates.network_in = calculateMetricRates(
            current, [](const SystemMetrics& m) { return m.network_bytes_in; });
        rates.network_out = calculateMetricRates(
            current,
            [](const SystemMetrics& m) { return m.network_bytes_out; });
        rates.disk_read = calculateMetricRates(
            current, [](const SystemMetrics& m) { return m.disk_bytes_read; });
        rates.disk_write = calculateMetricRates(
            current,
            [](const SystemMetrics& m) { return m.disk_bytes_written; });
        return rates;
    }
};

class DatagramMonitor
{
private:
    Application& app_;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    MetricsTracker metrics_tracker_;

    struct EndpointInfo
    {
        std::string ip;
        uint16_t port;
        bool is_ipv6;
    };
    EndpointInfo
    parseEndpoint(std::string const& endpoint)
    {
        auto space_pos = endpoint.find(' ');
        if (space_pos == std::string::npos)
            throw std::runtime_error("Invalid endpoint format");

        EndpointInfo info;
        info.ip = endpoint.substr(0, space_pos);
        info.port = std::stoi(endpoint.substr(space_pos + 1));
        info.is_ipv6 = info.ip.find(':') != std::string::npos;
        return info;
    }

    int
    createSocket(EndpointInfo const& endpoint)
    {
        int sock = socket(endpoint.is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            throw std::runtime_error("Failed to create socket");
        return sock;
    }

    void
    sendPacket(
        int sock,
        EndpointInfo const& endpoint,
        std::vector<uint8_t> const& buffer)
    {
        struct sockaddr_storage addr;
        socklen_t addr_len;

        if (endpoint.is_ipv6)
        {
            struct sockaddr_in6* addr6 =
                reinterpret_cast<struct sockaddr_in6*>(&addr);
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(endpoint.port);
            inet_pton(AF_INET6, endpoint.ip.c_str(), &addr6->sin6_addr);
            addr_len = sizeof(struct sockaddr_in6);
        }
        else
        {
            struct sockaddr_in* addr4 =
                reinterpret_cast<struct sockaddr_in*>(&addr);
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(endpoint.port);
            inet_pton(AF_INET, endpoint.ip.c_str(), &addr4->sin_addr);
            addr_len = sizeof(struct sockaddr_in);
        }

        sendto(
            sock,
            buffer.data(),
            buffer.size(),
            0,
            reinterpret_cast<struct sockaddr*>(&addr),
            addr_len);
    }

    // Returns both the counters and object count map separately
    std::pair<DebugCounters, ObjectCountMap>
    getDebugCounters()
    {
        DebugCounters counters;
        ObjectCountMap objectCounts =
            CountedObjects::getInstance().getCounts(1);

        // Database metrics if applicable
        if (!app_.config().reporting() && app_.config().useTxTables())
        {
            auto const db =
                dynamic_cast<SQLiteDatabase*>(&app_.getRelationalDatabase());
            if (!db)
                Throw<std::runtime_error>("Failed to get relational database");

            if (auto dbKB = db->getKBUsedAll())
                counters.dbKBTotal = dbKB;
            if (auto dbKB = db->getKBUsedLedger())
                counters.dbKBLedger = dbKB;
            if (auto dbKB = db->getKBUsedTransaction())
                counters.dbKBTransaction = dbKB;
            if (auto count = app_.getOPs().getLocalTxCount())
                counters.localTxCount = count;
        }

        // Basic metrics
        counters.writeLoad = app_.getNodeStore().getWriteLoad();
        counters.historicalPerMinute =
            static_cast<std::int32_t>(app_.getInboundLedgers().fetchRate());

        // Cache metrics - convert floating point rates to fixed point
        counters.sleHitRate =
            static_cast<std::uint32_t>(app_.cachedSLEs().rate() * 1000);
        counters.ledgerHitRate = static_cast<std::uint32_t>(
            app_.getLedgerMaster().getCacheHitRate() * 1000);
        counters.alSize = app_.getAcceptedLedgerCache().size();
        counters.alHitRate = static_cast<std::uint32_t>(
            app_.getAcceptedLedgerCache().getHitRate() * 1000);
        counters.fullbelowSize = static_cast<std::int32_t>(
            app_.getNodeFamily().getFullBelowCache(0)->size());
        counters.treenodeCacheSize =
            app_.getNodeFamily().getTreeNodeCache(0)->getCacheSize();
        counters.treenodeTrackSize =
            app_.getNodeFamily().getTreeNodeCache(0)->getTrackSize();

        // Handle shard metrics if available
        if (auto shardStore = app_.getShardStore())
        {
            auto shardFamily =
                dynamic_cast<ShardFamily*>(app_.getShardFamily());
            auto const [cacheSz, trackSz] = shardFamily->getTreeNodeCacheSize();

            counters.shardFullbelowSize = shardFamily->getFullBelowCacheSize();
            counters.shardTreenodeCacheSize = cacheSz;
            counters.shardTreenodeTrackSize = trackSz;
            counters.shardWriteLoad = shardStore->getWriteLoad();
            counters.shardNodeWrites = shardStore->getStoreCount();
            counters.shardNodeReadsTotal = shardStore->getFetchTotalCount();
            counters.shardNodeReadsHit = shardStore->getFetchHitCount();
            counters.shardNodeWrittenBytes = shardStore->getStoreSize();
            counters.shardNodeReadBytes = shardStore->getFetchSize();
        }
        else
        {
            // Get regular node store metrics
            counters.nodeWriteCount = app_.getNodeStore().getStoreCount();
            counters.nodeWriteSize = app_.getNodeStore().getStoreSize();
            counters.nodeFetchCount = app_.getNodeStore().getFetchTotalCount();
            counters.nodeFetchHitCount = app_.getNodeStore().getFetchHitCount();
            counters.nodeFetchSize = app_.getNodeStore().getFetchSize();
        }

        return {counters, objectCounts};
    }

    uint32_t
    getPhysicalCPUCount()
    {
        static uint32_t count = 0;
        if (count > 0)
            return count;

#if defined(__linux__)
        try
        {
            std::ifstream cpuinfo("/proc/cpuinfo");
            if (!cpuinfo)
            {
                JLOG(app_.journal("DatagramMonitor").error())
                    << "Unable to open file: /proc/cpuinfo";
                return count;
            }
            std::string line;
            std::set<std::string> physical_ids;
            std::string current_physical_id;

            while (std::getline(cpuinfo, line))
            {
                if (line.find("core id") != std::string::npos)
                {
                    current_physical_id = line.substr(line.find(":") + 1);
                    // Trim whitespace
                    current_physical_id.erase(
                        0, current_physical_id.find_first_not_of(" \t"));
                    current_physical_id.erase(
                        current_physical_id.find_last_not_of(" \t") + 1);
                    physical_ids.insert(current_physical_id);
                }
            }

            count = physical_ids.size();
        }
        catch (const std::exception& e)
        {
            JLOG(app_.journal("DatagramMonitor").error())
                << "Error getting CPU count: " << e.what();
        }

        // Return at least 1 if we couldn't determine the count
        return count > 0 ? count : (count = 1);
#elif defined(__APPLE__)
        int value = 0;
        size_t size = sizeof(value);
        if (sysctlbyname("hw.physicalcpu", &value, &size, NULL, 0) == 0)
            count = value;
        return count > 0 ? count : (count = 1);
#endif
    }

    SystemMetrics
    collectSystemMetrics()
    {
        SystemMetrics metrics{};
        metrics.timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

#if defined(__linux__)
        // Network stats collection
        try
        {
            std::ifstream net_file("/proc/net/dev");
            if (!net_file)
            {
                JLOG(app_.journal("DatagramMonitor").error())
                    << "Unable to open file /proc/net/dev";
                return metrics;
            }

            std::string line;
            uint64_t total_bytes_in = 0, total_bytes_out = 0;

            // Skip header lines
            std::getline(net_file, line);  // Inter-|   Receive...
            std::getline(net_file, line);  // face |bytes...

            while (std::getline(net_file, line))
            {
                if (line.find(':') != std::string::npos)
                {
                    std::string interface = line.substr(0, line.find(':'));
                    interface =
                        interface.substr(interface.find_first_not_of(" \t"));
                    interface = interface.substr(
                        0, interface.find_last_not_of(" \t") + 1);

                    // Skip loopback interface
                    if (interface == "lo")
                        continue;

                    uint64_t bytes_in, bytes_out;
                    std::istringstream iss(line.substr(line.find(':') + 1));
                    iss >> bytes_in;  // First field after : is bytes_in
                    for (int i = 0; i < 8; ++i)
                        iss >> std::ws;  // Skip 8 fields
                    iss >> bytes_out;    // 9th field is bytes_out

                    total_bytes_in += bytes_in;
                    total_bytes_out += bytes_out;
                }
            }
            metrics.network_bytes_in = total_bytes_in;
            metrics.network_bytes_out = total_bytes_out;
        }
        catch (const std::exception& e)
        {
            JLOG(app_.journal("DatagramMonitor").error())
                << "Error collecting network stats: " << e.what();
        }

        // Disk stats collection
        try
        {
            std::ifstream disk_file("/proc/diskstats");
            if (!disk_file)
            {
                JLOG(app_.journal("DatagramMonitor").error())
                    << "Unable to open file: /proc/diskstats";
                return metrics;
            }
            std::string line;
            uint64_t total_bytes_read = 0, total_bytes_written = 0;

            while (std::getline(disk_file, line))
            {
                unsigned int major, minor;
                char dev_name[32];
                uint64_t reads, read_sectors, writes, write_sectors;

                if (sscanf(
                        line.c_str(),
                        "%u %u %31s %lu %*u %lu %*u %lu %*u %lu",
                        &major,
                        &minor,
                        dev_name,
                        &reads,
                        &read_sectors,
                        &writes,
                        &write_sectors) == 7)
                {
                    // Only process physical devices
                    std::string device_name(dev_name);
                    if (device_name.substr(0, 3) == "dm-" ||
                        device_name.substr(0, 4) == "loop" ||
                        device_name.substr(0, 3) == "ram")
                    {
                        continue;
                    }

                    // Skip partitions (usually have a number at the end)
                    if (std::isdigit(device_name.back()))
                    {
                        continue;
                    }

                    uint64_t bytes_read = read_sectors * 512;
                    uint64_t bytes_written = write_sectors * 512;

                    total_bytes_read += bytes_read;
                    total_bytes_written += bytes_written;
                }
            }
            metrics.disk_bytes_read = total_bytes_read;
            metrics.disk_bytes_written = total_bytes_written;
        }
        catch (const std::exception& e)
        {
            JLOG(app_.journal("DatagramMonitor").error())
                << "Error collecting disk stats: " << e.what();
        }
#elif defined(__APPLE__)
        // Network stats collection
        try
        {
            struct ifaddrs* ifap;
            if (getifaddrs(&ifap) == 0)
            {
                uint64_t total_bytes_in = 0, total_bytes_out = 0;
                for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next)
                {
                    if (ifa->ifa_addr != NULL &&
                        ifa->ifa_addr->sa_family == AF_LINK)
                    {
                        struct if_data* ifd = (struct if_data*)ifa->ifa_data;
                        if (ifd != NULL)
                        {
                            // Skip loopback interface
                            if (strcmp(ifa->ifa_name, "lo0") == 0)
                                continue;

                            total_bytes_in += ifd->ifi_ibytes;
                            total_bytes_out += ifd->ifi_obytes;
                        }
                    }
                }
                freeifaddrs(ifap);

                metrics.network_bytes_in = total_bytes_in;
                metrics.network_bytes_out = total_bytes_out;
            }
        }
        catch (const std::exception& e)
        {
            JLOG(app_.journal("DatagramMonitor").error())
                << "Error collecting network stats: " << e.what();
        }

        // Disk stats collection
        // Disk IO stats are not easily accessible in macOS.
        // We'll set these values to zero for now.
        metrics.disk_bytes_read = 0;
        metrics.disk_bytes_written = 0;
#endif
        return metrics;
    }

    std::vector<uint8_t>
    generateServerInfo()
    {
        auto& ops = app_.getOPs();

        auto [dbg_counters, obj_count_map] = getDebugCounters();

        // Get the RangeSet directly
        auto rangeSet = app_.getLedgerMaster().getCompleteLedgersRangeSet();
        auto currentMetrics = collectSystemMetrics();
        metrics_tracker_.addSample(currentMetrics);

        // Count only non-zero intervals and calculate total size needed
        size_t validRangeCount = 0;
        for (auto const& interval : rangeSet)
        {
            // Skip intervals where both lower and upper are 0
            if (interval.lower() != 0 || interval.upper() != 0)
            {
                validRangeCount++;
            }
        }

        size_t totalSize = sizeof(ServerInfoHeader) +
            (validRangeCount * sizeof(LgrRange)) + (64 * obj_count_map.size());

        // Allocate buffer and initialize header
        std::vector<uint8_t> buffer(totalSize);
        auto* header = reinterpret_cast<ServerInfoHeader*>(buffer.data());
        memset(header, 0, sizeof(ServerInfoHeader));

        // Set magic number and version
        header->magic = SERVER_INFO_MAGIC;
        header->version = SERVER_INFO_VERSION;
        header->network_id = app_.config().NETWORK_ID;
        header->timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        header->uptime = UptimeClock::now().time_since_epoch().count();
        header->io_latency_us = app_.getIOLatency().count();
        header->validation_quorum = app_.validators().quorum();

        if (!app_.config().reporting())
            header->peer_count = app_.overlay().size();

        header->node_size = app_.config().NODE_SIZE;

        // Get state accounting data
        auto const [counters, mode, start, initialSync] =
            app_.getOPs().getStateAccountingData();

        // Pack state metrics into header
        for (size_t i = 0; i < 5; ++i)
        {
            header->state_transitions[i] = counters[i].transitions;
            header->state_durations[i] = counters[i].dur.count();
        }
        header->initial_sync_us = initialSync;

        // Pack warning flags
        if (ops.isAmendmentBlocked())
            header->warning_flags |= WARNING_AMENDMENT_BLOCKED;
        if (ops.isUNLBlocked())
            header->warning_flags |= WARNING_UNL_BLOCKED;
        if (ops.isAmendmentWarned())
            header->warning_flags |= WARNING_AMENDMENT_WARNED;

        // Pack consensus info
        auto& mConsensus = ops.getConsensus();
        header->proposer_count = mConsensus.prevProposers();
        header->converge_time_ms = mConsensus.prevRoundTime().count();

        // Pack fetch pack size if present
        auto& ledgerMaster = ops.getLedgerMaster();
        auto const lastClosed = ledgerMaster.getClosedLedger();
        auto const validated = ledgerMaster.getValidatedLedger();

        if (lastClosed && validated)
        {
            auto consensus =
                ledgerMaster.getLedgerByHash(lastClosed->info().hash);
            if (!consensus)
                consensus = app_.getInboundLedgers().acquire(
                    lastClosed->info().hash,
                    0,
                    InboundLedger::Reason::CONSENSUS);

            if (consensus &&
                (!ledgerMaster.canBeCurrent(consensus) ||
                 !ledgerMaster.isCompatible(
                     *consensus,
                     app_.journal("DatagramMonitor").debug(),
                     "Not switching")))
            {
                header->warning_flags |= WARNING_NOT_SYNCED;
            }
        }
        else
        {
            // If we don't have both lastClosed and validated ledgers, we're
            // definitely not synced
            header->warning_flags |= WARNING_NOT_SYNCED;
        }

        auto const fp = ledgerMaster.getFetchPackCacheSize();
        if (fp != 0)
            header->fetch_pack_size = fp;

        // Pack load factor info if not reporting
        if (!app_.config().reporting())
        {
            auto const escalationMetrics =
                app_.getTxQ().getMetrics(*app_.openLedger().current());
            auto const loadFactorServer = app_.getFeeTrack().getLoadFactor();
            auto const loadBaseServer = app_.getFeeTrack().getLoadBase();
            auto const loadFactorFeeEscalation =
                mulDiv(
                    escalationMetrics.openLedgerFeeLevel,
                    loadBaseServer,
                    escalationMetrics.referenceFeeLevel)
                    .second;

            header->load_factor = std::max(
                safe_cast<std::uint64_t>(loadFactorServer),
                loadFactorFeeEscalation);
            header->load_base = loadBaseServer;
        }

#if defined(__linux__)
        // Get system info using sysinfo
        struct sysinfo si;
        if (sysinfo(&si) == 0)
        {
            header->system_memory_total = si.totalram * si.mem_unit;
            header->system_memory_free = si.freeram * si.mem_unit;
            header->system_memory_used =
                header->system_memory_total - header->system_memory_free;
            header->load_avg_1min = si.loads[0] / (float)(1 << SI_LOAD_SHIFT);
            header->load_avg_5min = si.loads[1] / (float)(1 << SI_LOAD_SHIFT);
            header->load_avg_15min = si.loads[2] / (float)(1 << SI_LOAD_SHIFT);
        }
#elif defined(__APPLE__)
        // Get total physical memory
        int64_t physical_memory;
        size_t length = sizeof(physical_memory);
        if (sysctlbyname("hw.memsize", &physical_memory, &length, NULL, 0) == 0)
        {
            header->system_memory_total = physical_memory;
        }

        // Get free and used memory
        vm_statistics_data_t vm_stats;
        mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
        if (host_statistics(
                mach_host_self(),
                HOST_VM_INFO,
                (host_info_t)&vm_stats,
                &count) == KERN_SUCCESS)
        {
            uint64_t page_size;
            length = sizeof(page_size);
            sysctlbyname("hw.pagesize", &page_size, &length, NULL, 0);

            header->system_memory_free =
                (uint64_t)vm_stats.free_count * page_size;
            header->system_memory_used =
                header->system_memory_total - header->system_memory_free;
        }

        // Get load averages
        double loadavg[3];
        if (getloadavg(loadavg, 3) == 3)
        {
            header->load_avg_1min = loadavg[0];
            header->load_avg_5min = loadavg[1];
            header->load_avg_15min = loadavg[2];
        }
#endif

        // Get process memory usage
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        header->process_memory_pages = usage.ru_maxrss;

        // Get disk usage
#if defined(__linux__)
        struct statvfs fs;
        if (statvfs("/", &fs) == 0)
        {
            header->system_disk_total = fs.f_blocks * fs.f_frsize;
            header->system_disk_free = fs.f_bfree * fs.f_frsize;
            header->system_disk_used =
                header->system_disk_total - header->system_disk_free;
        }
#elif defined(__APPLE__)
        struct statfs fs;
        if (statfs("/", &fs) == 0)
        {
            header->system_disk_total = fs.f_blocks * fs.f_bsize;
            header->system_disk_free = fs.f_bfree * fs.f_bsize;
            header->system_disk_used =
                header->system_disk_total - header->system_disk_free;
        }
#endif

        // Get CPU core count
        header->cpu_cores = getPhysicalCPUCount();

        // Get rate statistics
        auto rates = metrics_tracker_.getRates(currentMetrics);
        header->rates.network_in = rates.network_in;
        header->rates.network_out = rates.network_out;
        header->rates.disk_read = rates.disk_read;
        header->rates.disk_write = rates.disk_write;

        // Pack ledger info and ranges
        auto lpClosed = ledgerMaster.getValidatedLedger();
        if (!lpClosed && !app_.config().reporting())
            lpClosed = ledgerMaster.getClosedLedger();

        if (lpClosed)
        {
            header->ledger_seq = lpClosed->info().seq;
            auto const& hash = lpClosed->info().hash;
            std::memcpy(header->ledger_hash, hash.data(), 32);
            header->reserve_base = lpClosed->fees().accountReserve(0).drops();
            header->reserve_inc = lpClosed->fees().increment.drops();
        }

        // Pack node public key
        auto const& nodeKey = app_.nodeIdentity().first;
        std::memcpy(header->node_public_key, nodeKey.data(), 33);

        // Pack version string
        memset(&header->version_string, 0, 32);
        memcpy(
            &header->version_string,
            BuildInfo::getVersionString().c_str(),
            BuildInfo::getVersionString().size() > 32
                ? 32
                : BuildInfo::getVersionString().size());

        header->dbg_counters = dbg_counters;

        // Set the complete ledger count
        header->ledger_range_count = validRangeCount;

        // Append only non-zero ranges after the header
        auto* rangeData = reinterpret_cast<LgrRange*>(
            buffer.data() + sizeof(ServerInfoHeader));
        size_t i = 0;
        for (auto const& interval : rangeSet)
        {
            // Only pack non-zero ranges
            if (interval.lower() != 0 || interval.upper() != 0)
            {
                rangeData[i].start = interval.lower();
                rangeData[i].end = interval.upper();
                ++i;
            }
        }

        uint8_t* end_of_ranges = reinterpret_cast<uint8_t*>(buffer.data()) +
            sizeof(ServerInfoHeader) + (validRangeCount * sizeof(LgrRange));

        memset(end_of_ranges, 0, 64 * obj_count_map.size());

        uint8_t* ptr = end_of_ranges;
        for (auto& [name, val] : obj_count_map)
        {
            size_t to_write = name.size() > 56 ? 56 : name.size();
            memcpy(ptr, name.c_str(), to_write);
            ptr += 56;
            *reinterpret_cast<uint64_t*>(ptr) = val;
            ptr += 8;
        }

        return buffer;
    }
    void
    monitorThread()
    {
        std::vector<std::pair<EndpointInfo, int>> endpoints;

        for (auto const& epStr : app_.config().DATAGRAM_MONITOR)
        {
            auto endpoint = parseEndpoint(epStr);
            endpoints.push_back(
                std::make_pair(endpoint, createSocket(endpoint)));
        }

        while (running_)
        {
            try
            {
                auto info = generateServerInfo();
                for (auto const& ep : endpoints)
                {
                    sendPacket(ep.second, ep.first, info);
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            catch (const std::exception& e)
            {
                // Log error but continue monitoring
                JLOG(app_.journal("DatagramMonitor").error())
                    << "Server info monitor error: " << e.what();
            }
        }

        for (auto const& ep : endpoints)
        {
            close(ep.second);
        }
    }

public:
    DatagramMonitor(Application& app) : app_(app)
    {
    }

    void
    start()
    {
        if (!running_.exchange(true))
        {
            monitor_thread_ =
                std::thread(&DatagramMonitor::monitorThread, this);
        }
    }

    void
    stop()
    {
        if (running_.exchange(false))
        {
            if (monitor_thread_.joinable())
                monitor_thread_.join();
        }
    }

    ~DatagramMonitor()
    {
        stop();
    }
};
}  // namespace ripple
#endif
