#include "collectors.h"
#include "executor.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

// -------------------------------------------------------------------
// helpers
// -------------------------------------------------------------------

static int cpu_count() {
    std::string out = executor::run_cmd("nproc 2>/dev/null");
    if (!out.empty()) {
        try { return std::stoi(utils::strip(out)); } catch (...) {}
    }
    // fallback
    out = executor::run_cmd("grep -c ^processor /proc/cpuinfo 2>/dev/null");
    if (!out.empty()) {
        try { return std::stoi(utils::strip(out)); } catch (...) {}
    }
    return 1;
}

static std::string dmesg_tail(int lines = 20) {
    return executor::run_cmd("dmesg 2>/dev/null | tail -" + std::to_string(lines));
}

static int dmesg_error_count(const std::string& pattern) {
    std::string cmd = "dmesg 2>/dev/null | grep -ciE '" + pattern + "' || echo 0";
    std::string out = executor::run_cmd(cmd);
    try { return std::stoi(utils::strip(out)); } catch (...) { return 0; }
}

// -------------------------------------------------------------------
// CPU
// -------------------------------------------------------------------
ResourceResult collect_cpu() {
    ResourceResult result;
    result.resource = "CPU";

    int ncpu = cpu_count();
    std::string vmstat_out = executor::run_cmd("vmstat 1 2 2>/dev/null");

    // Parse vmstat output — last line has the second sample
    // Format: procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----
    //  r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st
    auto lines = utils::split(vmstat_out, '\n');
    std::string data_line;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string l = utils::strip(*it);
        if (l.empty() || l.find("procs") != std::string::npos ||
            l.find("r") == 0 || l.find("memory") != std::string::npos ||
            l.find("swap") != std::string::npos || l.find("io") != std::string::npos ||
            l.find("system") != std::string::npos || l.find("cpu") != std::string::npos) {
            continue;
        }
        // Check it starts with a digit
        if (!l.empty() && (std::isdigit(l[0]) || l[0] == ' ')) {
            data_line = l;
            break;
        }
    }

    double us = 0, sy = 0, st = 0, id = 0, wa = 0;
    long r_val = 0;
    if (!data_line.empty()) {
        auto tokens = utils::split(data_line, ' ');
        // Remove empty tokens from repeated spaces
        std::vector<std::string> cols;
        for (auto& t : tokens) {
            std::string s = utils::strip(t);
            if (!s.empty()) cols.push_back(s);
        }
        // proc: r b
        // mem: swpd free buff cache
        // swap: si so
        // io: bi bo
        // system: in cs
        // cpu: us sy id wa st
        if (cols.size() >= 17) {
            r_val = std::stol(cols[0]);
            us = std::stod(cols[12]);
            sy = std::stod(cols[13]);
            id = std::stod(cols[14]);
            wa = std::stod(cols[15]);
            st = std::stod(cols[16]);
        }
    }

    // Utilization = us + sy + st
    double cpu_util = us + sy + st;

    // Thresholds
    static const Threshold util_thresh = {70.0, 90.0};
    Metric util_metric;
    util_metric.name = "Utilization";
    util_metric.command = "vmstat 1 2";
    {
        std::ostringstream val;
        val.precision(1);
        val << std::fixed << cpu_util << "%";
        util_metric.value = val.str();
        std::ostringstream det;
        det << "us=" << us << "% sy=" << sy << "%";
        if (st > 0) det << " st=" << st << "%";
        det << " id=" << id << "% wa=" << wa << "%";
        det << " (CPUs=" << ncpu << ")";
        util_metric.detail = det.str();
    }
    util_metric.status = utils::classify(cpu_util, util_thresh);
    result.metrics.push_back(util_metric);

    // Saturation: run queue vs CPU count
    static const Threshold sat_thresh_ratio = {1.0, 2.0}; // ratio to ncpu
    Metric sat_metric;
    sat_metric.name = "Saturation";
    sat_metric.command = "vmstat 1 2 (r column)";
    {
        std::ostringstream val;
        val << "runq=" << r_val << "/" << ncpu;
        sat_metric.value = val.str();
        sat_metric.detail = "runnable threads = " + std::to_string(r_val);
    }
    if (ncpu > 0) {
        double ratio = static_cast<double>(r_val) / ncpu;
        sat_metric.status = utils::classify(ratio, sat_thresh_ratio);
    } else {
        sat_metric.status = Status::OK;
    }
    result.metrics.push_back(sat_metric);

    // Errors: check dmesg for CPU-related errors
    Metric err_metric;
    err_metric.name = "Errors";
    err_metric.command = "dmesg | grep -iE 'cpu|thermal|machine.check'";
    {
        int cpu_errs = dmesg_error_count("thermal throttl|Machine check|CPU.*failed|TSC_DEADLINE|soft lockup|hard lockup|Watchdog detected");
        std::string dmesg_out = dmesg_tail(15);
        // Count lines mentioning error patterns
        err_metric.value = (cpu_errs > 0) ? std::to_string(cpu_errs) + " events" : "0";
        err_metric.detail = (cpu_errs > 0) ? "Check dmesg for details" : "No CPU errors in dmesg";
        err_metric.status = (cpu_errs > 0) ? Status::WARNING : Status::OK;
    }
    result.metrics.push_back(err_metric);

    return result;
}

// -------------------------------------------------------------------
// Memory
// -------------------------------------------------------------------
ResourceResult collect_memory() {
    ResourceResult result;
    result.resource = "Memory";

    // --- free -m (force C locale for reliable parsing) ---
    std::string free_out = executor::run_cmd("LANG=C free -m 2>/dev/null");
    double mem_total = 0, mem_used = 0, mem_avail = 0, mem_util = 0;
    double swap_total = 0, swap_used = 0;
    {
        auto lines = utils::split(free_out, '\n');
        for (auto& l : lines) {
            l = utils::strip(l);
            auto tokens = utils::split(l, ' ');
            std::vector<std::string> cols;
            for (auto& t : tokens) {
                std::string s = utils::strip(t);
                if (!s.empty()) cols.push_back(s);
            }
            if (cols.empty()) continue;
            if (cols[0] == "Mem:" && cols.size() >= 3) {
                mem_total = std::stod(cols[1]);
                mem_used = std::stod(cols[2]);
                if (cols.size() >= 7) mem_avail = std::stod(cols[6]);
            }
            if (cols[0] == "Swap:" && cols.size() >= 3) {
                swap_total = std::stod(cols[1]);
                swap_used = std::stod(cols[2]);
            }
        }
    }
    if (mem_total > 0) {
        mem_util = (mem_used / mem_total) * 100.0;
    }

    // Utilization
    static const Threshold mem_util_thresh = {80.0, 95.0};
    Metric util_metric;
    util_metric.name = "Utilization";
    util_metric.command = "free -m";
    {
        std::ostringstream val;
        val.precision(1);
        val << std::fixed << mem_util << "%";
        util_metric.value = val.str();
        std::ostringstream det;
        det.precision(1);
        det << std::fixed;
        det << "used=" << mem_used << "M / total=" << mem_total << "M"
            << " avail=" << mem_avail << "M";
        if (swap_total > 0) {
            det << " swap=" << swap_used << "M/" << swap_total << "M";
        }
        util_metric.detail = det.str();
    }
    util_metric.status = utils::classify(mem_util, mem_util_thresh);
    result.metrics.push_back(util_metric);

    // Saturation: swap activity + OOM
    Metric sat_metric;
    sat_metric.name = "Saturation";
    sat_metric.command = "vmstat 1 2 (si/so)";
    {
        std::string vmstat_out = executor::run_cmd("vmstat 1 2 2>/dev/null");
        double si = 0, so = 0;
        auto lines = utils::split(vmstat_out, '\n');
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            std::string l = utils::strip(*it);
            if (l.empty() || l.find("procs") != std::string::npos ||
                l.find("r") == 0 || l.find("memory") != std::string::npos ||
                l.find("swap") != std::string::npos || l.find("io") != std::string::npos ||
                l.find("system") != std::string::npos || l.find("cpu") != std::string::npos) {
                continue;
            }
            if (!l.empty() && (std::isdigit(l[0]) || l[0] == ' ')) {
                auto tokens = utils::split(l, ' ');
                std::vector<std::string> cols;
                for (auto& t : tokens) {
                    std::string s = utils::strip(t);
                    if (!s.empty()) cols.push_back(s);
                }
                // si = col[6], so = col[7] in vmstat full output
                if (cols.size() >= 8) {
                    si = std::stod(cols[6]);
                    so = std::stod(cols[7]);
                }
                break;
            }
        }

        double total_swap = si + so;
        std::ostringstream val;
        val.precision(1);
        val << std::fixed;
        if (total_swap > 0) {
            val << "swap: si=" << si << " so=" << so << " KB/s";
        } else {
            val << "swap=0 KB/s";
        }
        sat_metric.value = val.str();
        sat_metric.detail = "Page swap in/out";

        static const Threshold swap_thresh = {0.1, 100.0}; // KB/s
        sat_metric.status = utils::classify(total_swap, swap_thresh);
    }
    result.metrics.push_back(sat_metric);

    // Errors
    Metric err_metric;
    err_metric.name = "Errors";
    err_metric.command = "dmesg | grep -iE 'oom|out of memory|page allocation error'";
    {
        int oom_count = dmesg_error_count("oom|out of memory|page allocation error|allocation failure");
        err_metric.value = (oom_count > 0) ? std::to_string(oom_count) + " OOM events" : "0";
        err_metric.detail = (oom_count > 0) ? "OOM killer invoked!" : "No memory errors";
        err_metric.status = (oom_count > 0) ? Status::DANGER : Status::OK;
    }
    result.metrics.push_back(err_metric);

    return result;
}

// -------------------------------------------------------------------
// Network
// -------------------------------------------------------------------
std::vector<ResourceResult> collect_network() {
    std::vector<ResourceResult> results;

    // Enumerate interfaces (skip loopback)
    std::string ip_out = executor::run_cmd(
        "ip -o link show 2>/dev/null | grep -v 'LOOPBACK' | awk -F': ' '{print $2}'");
    auto ifaces = utils::split(ip_out, '\n');

    if (ifaces.empty()) {
        // fallback: /proc/net/dev
        auto dev_lines = executor::run_cmd_lines("tail -n +3 /proc/net/dev 2>/dev/null");
        for (auto& l : dev_lines) {
            auto cols = utils::split(utils::strip(l), ':');
            if (cols.size() >= 2 && cols[0].find("lo") == std::string::npos) {
                ifaces.push_back(utils::strip(cols[0]));
            }
        }
    }

    if (ifaces.empty()) {
        // No interfaces found, return a placeholder
        ResourceResult r;
        r.resource = "Network";
        Metric m;
        m.name = "Info";
        m.value = "No non-loopback interfaces";
        m.status = Status::INFO;
        m.detail = "Could not enumerate network interfaces";
        r.metrics.push_back(m);
        results.push_back(r);
        return results;
    }

    // Remove duplicates and empty lines
    std::set<std::string> unique_ifaces;
    for (auto& iface : ifaces) {
        std::string s = utils::strip(iface);
        if (!s.empty()) unique_ifaces.insert(s);
    }

    // Get per-interface stats from /proc/net/dev for accuracy
    std::string netdev = executor::run_cmd("cat /proc/net/dev 2>/dev/null");

    for (const auto& iface : unique_ifaces) {
        ResourceResult r;
        r.resource = "Network (" + iface + ")";

        // Parse /proc/net/dev line for this interface
        // Format: face: bytes packets errs drop fifo frame compressed multicast
        //         (RX)                                     (TX similar)
        double rx_bytes = 0, tx_bytes = 0;
        long rx_errs = 0, tx_errs = 0, rx_drop = 0, tx_drop = 0;

        auto nd_lines = utils::split(netdev, '\n');
        for (auto& l : nd_lines) {
            if (l.find(iface + ":") != 0 && l.find(" " + iface + ":") == std::string::npos)
                continue;
            auto parts = utils::split(l, ':');
            if (parts.size() < 2) continue;
            std::string data = utils::strip(parts[1]);
            auto tokens = utils::split(data, ' ');
            std::vector<std::string> cols;
            for (auto& t : tokens) {
                std::string s = utils::strip(t);
                if (!s.empty()) cols.push_back(s);
            }
            // RX: bytes packets errs drop fifo frame compressed multicast
            // TX: bytes packets errs drop fifo colls carrier compressed
            if (cols.size() >= 16) {
                rx_bytes = std::stod(cols[0]);
                // cols[1] = packets, cols[2] = errs, cols[3] = drop
                rx_errs  = std::stol(cols[2]);
                rx_drop  = std::stol(cols[3]);
                // TX: bytes packets errs drop ...
                tx_bytes = std::stod(cols[8]);
                tx_errs  = std::stol(cols[10]);
                tx_drop  = std::stol(cols[11]);
            } else if (cols.size() >= 10) {
                rx_bytes = std::stod(cols[0]);
                rx_errs  = std::stol(cols[2]);
                rx_drop  = std::stol(cols[3]);
                tx_bytes = std::stod(cols[8]);
                tx_errs  = std::stol(cols[10]);
                tx_drop  = std::stol(cols[11]);
            }
            break;
        }

        // Utilization (rough estimate — no bandwidth limit known, use relative)
        std::string rx_hr, tx_hr;
        {
            std::ostringstream r, t;
            r.precision(1);
            t.precision(1);
            if (rx_bytes > 1e9)  { r << std::fixed << rx_bytes/1e9 << "GB"; }
            else if (rx_bytes > 1e6) { r << std::fixed << rx_bytes/1e6 << "MB"; }
            else                     { r << std::fixed << rx_bytes/1e3 << "KB"; }
            if (tx_bytes > 1e9)  { t << std::fixed << tx_bytes/1e9 << "GB"; }
            else if (tx_bytes > 1e6) { t << std::fixed << tx_bytes/1e6 << "MB"; }
            else                     { t << std::fixed << tx_bytes/1e3 << "KB"; }
            rx_hr = r.str();
            tx_hr = t.str();
        }

        Metric util_metric;
        util_metric.name = "Utilization";
        util_metric.command = "ip -s link & /proc/net/dev";
        {
            // Use packet rate as a rough indicator — we can't know max bandwidth
            // without knowing link speed. Check via ethtool if available.
            std::string speed_str = executor::run_cmd(
                "cat /sys/class/net/" + iface + "/speed 2>/dev/null || echo 0");
            long speed_mbps = 0;
            try { speed_mbps = std::stol(utils::strip(speed_str)); } catch (...) {}

            std::ostringstream val;
            val.precision(1);
            val << std::fixed;
            if (speed_mbps > 0) {
                // Rough utilization: (rx+tx bytes/sec) * 8 / speed
                // We have cumulative bytes — we'd need delta. Skip % for now.
                val << "RX=" << rx_hr << " TX=" << tx_hr;
                util_metric.value = "cumulative since boot";
            } else {
                val << "RX=" << rx_hr << " TX=" << tx_hr;
                util_metric.value = val.str();
            }
            std::ostringstream det;
            det << "RX=" << rx_hr << " TX=" << tx_hr;
            if (speed_mbps > 0) det << " link=" << speed_mbps << "Mbps";
            util_metric.detail = det.str();
            util_metric.status = Status::INFO; // can't determine % without delta
        }
        r.metrics.push_back(util_metric);

        // Saturation: drops
        Metric sat_metric;
        sat_metric.name = "Saturation";
        sat_metric.command = "/proc/net/dev (drop)";
        {
            long total_drops = rx_drop + tx_drop;
            std::ostringstream val;
            val << "drops=" << total_drops << " (rx=" << rx_drop << " tx=" << tx_drop << ")";
            sat_metric.value = val.str();
            sat_metric.detail = "Interface dropped packets";
            static const Threshold drop_thresh = {1, 1000};
            sat_metric.status = utils::classify(static_cast<double>(total_drops), drop_thresh);
        }
        r.metrics.push_back(sat_metric);

        // Errors
        Metric err_metric;
        err_metric.name = "Errors";
        err_metric.command = "/proc/net/dev (errs)";
        {
            long total_errs = rx_errs + tx_errs;
            std::ostringstream val;
            val << "errors=" << total_errs << " (rx=" << rx_errs << " tx=" << tx_errs << ")";
            err_metric.value = val.str();
            err_metric.detail = "Interface error counters";
            static const Threshold err_thresh = {1, 100};
            err_metric.status = utils::classify(static_cast<double>(total_errs), err_thresh);
        }
        r.metrics.push_back(err_metric);

        results.push_back(r);
    }

    // TCP stats summary
    std::string tcp_out = executor::run_cmd(
        "netstat -s -t 2>/dev/null | grep -iE 'retransmit|segments sent|segments received|connection errors' "
        "|| ss -s 2>/dev/null");
    if (!tcp_out.empty()) {
        ResourceResult tcp_r;
        tcp_r.resource = "Network (TCP)";
        Metric m;
        m.name = "TCP Status";
        m.command = "netstat -s | grep -i retransmit";
        m.value = "See detail";
        m.detail = utils::strip(tcp_out.substr(0, 200));
        m.status = Status::INFO;
        tcp_r.metrics.push_back(m);
        results.push_back(tcp_r);
    }

    return results;
}

// -------------------------------------------------------------------
// Storage
// -------------------------------------------------------------------
std::vector<ResourceResult> collect_storage() {
    std::vector<ResourceResult> results;

    // --- I/O stats via iostat ---
    std::string iostat_out = executor::run_cmd("iostat -xz 1 2 2>/dev/null || iostat -x 1 2 2>/dev/null");
    if (!iostat_out.empty()) {
        auto lines = utils::split(iostat_out, '\n');

        // Find the second sample (after the second empty line or header occurrence)
        // iostat output: header, sample1, header, sample2
        std::vector<std::string> sample_lines;

        // More robust: take the last block that starts with a device name
        // Format lines like: sda 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00
        for (auto& l : lines) {
            std::string line = utils::strip(l);
            if (line.empty()) continue;
            if (line.find("Device") != std::string::npos ||
                line.find("avg-cpu") != std::string::npos ||
                line.find("Linux") != std::string::npos ||
                line.find("iostat") != std::string::npos) {
                continue;
            }
            // Check if first token looks like a device name (sdX, nvmeXnY, vdX, mmcblkX, etc.)
            auto tokens = utils::split(line, ' ');
            if (tokens.empty()) continue;
            std::string first = utils::strip(tokens[0]);
            if (first.empty() || (!std::isalpha(first[0]))) continue;

            // Check it has Device: ... %util at the end
            // iostat -x format: Device r/s w/s rkB/s wkB/s rrqm/s wrqm/s %rrqm %wrqm r_await w_await aqu-sz rareq-sz wareq-sz svctm %util
            // iostat -xz same but without 2 fields
            std::vector<std::string> cols;
            for (auto& t : tokens) {
                std::string s = utils::strip(t);
                if (!s.empty()) cols.push_back(s);
            }
            double p_util = 0, avgqu_sz = 0, r_await = 0, w_await = 0;
            double rs = 0, ws = 0;

            // Try to match device + number columns
            // iostat -x has %util as last column, avgqu-sz near the middle
            if (cols.size() >= 14 && first.length() < 32) {
                try {
                    rs = std::stod(cols[1]);  // r/s
                    ws = std::stod(cols[2]);  // w/s
                    // cols[3]=rkB/s, cols[4]=wkB/s
                    r_await  = std::stod(cols[9]);   // r_await
                    w_await  = std::stod(cols[10]);  // w_await
                    avgqu_sz = std::stod(cols[11]);  // aqu-sz
                    // svctm    = std::stod(cols[*]);
                    p_util   = std::stod(cols[cols.size() - 1]); // %util
                } catch (...) {
                    continue;
                }

                // Skip devices with no I/O
                if (rs < 0.01 && ws < 0.01) continue;

                // Skip loop devices with near-zero I/O
                if ((first.find("loop") != std::string::npos ||
                     first.find("ram") != std::string::npos) &&
                    rs < 1.0 && ws < 1.0) continue;

                ResourceResult r;
                r.resource = "Storage (" + first + ")";

                // Utilization
                static const Threshold util_thresh_io = {60.0, 85.0};
                Metric u;
                u.name = "Utilization";
                u.command = "iostat -xz 1 2";
                {
                    std::ostringstream val;
                    val.precision(1);
                    val << std::fixed << p_util << "%";
                    u.value = val.str();
                    std::ostringstream det;
                    det.precision(1);
                    det << std::fixed;
                    det << "r/s=" << rs << " w/s=" << ws;
                    u.detail = det.str();
                    u.status = utils::classify(p_util, util_thresh_io);
                }
                r.metrics.push_back(u);

                // Saturation
                static const Threshold aqu_thresh = {1.0, 8.0};
                static const Threshold await_thresh = {10.0, 25.0};
                Metric s;
                s.name = "Saturation";
                s.command = "iostat -xz 1 2 (avgqu-sz, await)";
                {
                    std::ostringstream val;
                    val.precision(1);
                    val << std::fixed << "avgqu-sz=" << avgqu_sz << " await(r=" << r_await;
                    if (w_await >= 0) val << " w=" << w_await;
                    val << "ms)";
                    s.value = val.str();
                    s.detail = "Average queue length & wait time";

                    // Combine: either high queue OR high await triggers warning
                    Status s1 = utils::classify(avgqu_sz, aqu_thresh);
                    Status s2 = utils::classify(std::max(r_await, w_await), await_thresh);
                    s.status = (s1 > s2) ? s1 : s2;
                }
                r.metrics.push_back(s);

                // Errors — check dmesg for this device
                Metric e;
                e.name = "Errors";
                e.command = "dmesg | grep -i " + first;
                {
                    std::string cmd = "dmesg 2>/dev/null | grep -ci '" + first + ".*error\\|" + first + ".*fail\\|I/O error.*" + first + "' || echo 0";
                    std::string eout = executor::run_cmd(cmd);
                    int io_errs = 0;
                    try { io_errs = std::stoi(utils::strip(eout)); } catch (...) {}
                    e.value = (io_errs > 0) ? std::to_string(io_errs) + " events" : "0";
                    e.detail = (io_errs > 0) ? "I/O errors in dmesg" : "No I/O errors";
                    e.status = (io_errs > 0) ? Status::WARNING : Status::OK;
                }
                r.metrics.push_back(e);

                results.push_back(r);
            }
        }
    }

    // --- Capacity via df ---
    std::string df_out = executor::run_cmd("df -h 2>/dev/null | tail -n +2");
    if (!df_out.empty()) {
        auto df_lines = utils::split(df_out, '\n');
        for (auto& l : df_lines) {
            std::string line = utils::strip(l);
            if (line.empty()) continue;
            auto tokens = utils::split(line, ' ');
            std::vector<std::string> cols;
            for (auto& t : tokens) {
                std::string s = utils::strip(t);
                if (!s.empty()) cols.push_back(s);
            }
            // Filesystem Size Used Avail Use% Mounted
            if (cols.size() >= 6) {
                std::string fs = cols[0];
                std::string mount = cols[5];
                // Skip synthetic / pseudo filesystems and snap packages
                if (fs.find("tmpfs") == 0 || fs.find("devtmpfs") == 0 ||
                    fs.find("overlay") == 0 || fs.find("squashfs") == 0 ||
                    mount.find("/boot") == 0 || mount.find("/snap/") == 0 ||
                    mount == "/dev" || mount == "/sys" || mount == "/proc")
                    continue;

                std::string pct_str = cols[4]; // e.g., "72%"
                double pct = utils::parse_percent(pct_str);

                ResourceResult r;
                r.resource = "Storage (" + mount + ")";

                Metric m;
                m.name = "Capacity";
                m.command = "df -h";
                {
                    std::ostringstream val;
                    val << pct_str << " used";
                    m.value = val.str();
                    std::ostringstream det;
                    det << cols[1] << " total, " << cols[2] << " used, "
                        << cols[3] << " avail on " << mount;
                    m.detail = det.str();

                    static const Threshold cap_thresh = {80.0, 95.0};
                    m.status = utils::classify(pct, cap_thresh);
                }
                r.metrics.push_back(m);
                results.push_back(r);
            }
        }
    }

    return results;
}

// -------------------------------------------------------------------
// Software resources
// -------------------------------------------------------------------
ResourceResult collect_software() {
    ResourceResult result;
    result.resource = "Software Resources";

    // --- File descriptors ---
    {
        std::string fnr = utils::strip(executor::run_cmd("cat /proc/sys/fs/file-nr 2>/dev/null || echo '0 0 0'"));
        auto parts = utils::split(fnr, ' ');
        long allocated = 0, max_fd = 0;
        if (parts.size() >= 3) {
            try { allocated = std::stol(parts[0]); } catch (...) {}
            try { max_fd   = std::stol(parts[2]); } catch (...) {}
        }
        if (max_fd > 0) {
            double fd_pct = (static_cast<double>(allocated) / max_fd) * 100.0;
            Metric m;
            m.name = "File Descriptors";
            m.command = "cat /proc/sys/fs/file-nr";
            {
                std::ostringstream val;
                val.precision(1);
                val << std::fixed << fd_pct << "% (" << allocated << "/" << max_fd << ")";
                m.value = val.str();
                m.detail = "Allocated FD / system max";
                static const Threshold fd_thresh = {70.0, 90.0};
                m.status = utils::classify(fd_pct, fd_thresh);
            }
            result.metrics.push_back(m);
        }
    }

    // --- Task/thread capacity ---
    {
        std::string tmax_s = utils::strip(executor::run_cmd("cat /proc/sys/kernel/threads-max 2>/dev/null || echo '0'"));
        long tmax = 0;
        try { tmax = std::stol(tmax_s); } catch (...) {}

        std::string tasks_s = utils::strip(executor::run_cmd("ps aux 2>/dev/null | wc -l"));
        long tasks = 0;
        try { tasks = std::stol(tasks_s); } catch (...) {}

        if (tmax > 0 && tasks > 0) {
            double task_pct = (static_cast<double>(tasks) / tmax) * 100.0;
            Metric m;
            m.name = "Tasks";
            m.command = "ps aux | wc -l; cat /proc/sys/kernel/threads-max";
            {
                std::ostringstream val;
                val.precision(1);
                val << std::fixed << task_pct << "% (" << tasks << "/" << tmax << ")";
                m.value = val.str();
                m.detail = "Processes / threads-max";
                static const Threshold task_thresh = {70.0, 90.0};
                m.status = utils::classify(task_pct, task_thresh);
            }
            result.metrics.push_back(m);
        }
    }

    // --- Load averages (context) ---
    {
        std::string load = utils::strip(executor::run_cmd("cat /proc/loadavg 2>/dev/null || uptime"));
        Metric m;
        m.name = "Load Average";
        m.command = "uptime or /proc/loadavg";
        {
            // loadavg format: "0.45 0.30 0.20 1/234 12345"
            auto parts = utils::split(load, ' ');
            if (parts.size() >= 3) {
                m.value = parts[0] + " " + parts[1] + " " + parts[2];
            } else {
                m.value = load.substr(0, 60);
            }
            m.detail = "1min / 5min / 15min load averages";
            m.status = Status::INFO;
        }
        result.metrics.push_back(m);
    }

    // --- Uptime ---
    {
        std::string up = utils::strip(executor::run_cmd("cat /proc/uptime 2>/dev/null || echo '0'"));
        double secs = 0;
        try {
            auto parts = utils::split(up, ' ');
            if (!parts.empty()) secs = std::stod(parts[0]);
        } catch (...) {}

        Metric m;
        m.name = "Uptime";
        m.command = "cat /proc/uptime";
        {
            long days = static_cast<long>(secs / 86400);
            long hours = static_cast<long>((secs - days * 86400) / 3600);
            long mins = static_cast<long>((secs - days * 86400 - hours * 3600) / 60);
            if (days > 0)
                m.value = std::to_string(days) + "d " + std::to_string(hours) + "h";
            else if (hours > 0)
                m.value = std::to_string(hours) + "h " + std::to_string(mins) + "m";
            else
                m.value = std::to_string(mins) + "m " + std::to_string(static_cast<long>(secs) % 60) + "s";
            m.detail = "System uptime";
            m.status = Status::INFO;
        }
        result.metrics.push_back(m);
    }

    return result;
}

// -------------------------------------------------------------------
// Quick 60s check (Netflix-style)
// -------------------------------------------------------------------
std::vector<ResourceResult> collect_quick() {
    std::vector<ResourceResult> results;

    // 1. uptime (done inside software)
    results.push_back(collect_cpu());
    results.push_back(collect_memory());
    auto net = collect_network();
    // Just first interface
    if (!net.empty()) results.push_back(net[0]);
    auto stor = collect_storage();
    // Just first I/O device and first filesystem
    for (auto& s : stor) {
        if (results.size() < 6) results.push_back(s);
    }
    results.push_back(collect_software());
    return results;
}

// -------------------------------------------------------------------
// Dispatcher
// -------------------------------------------------------------------
std::vector<ResourceResult> collect_all(const std::vector<std::string>& resources) {
    std::vector<ResourceResult> results;
    bool all = resources.empty();

    if (all || std::find(resources.begin(), resources.end(), "cpu") != resources.end()) {
        results.push_back(collect_cpu());
    }
    if (all || std::find(resources.begin(), resources.end(), "memory") != resources.end()) {
        results.push_back(collect_memory());
    }
    if (all || std::find(resources.begin(), resources.end(), "network") != resources.end()) {
        auto net = collect_network();
        results.insert(results.end(), net.begin(), net.end());
    }
    if (all || std::find(resources.begin(), resources.end(), "storage") != resources.end()) {
        auto stor = collect_storage();
        results.insert(results.end(), stor.begin(), stor.end());
    }
    if (all || std::find(resources.begin(), resources.end(), "software") != resources.end()) {
        results.push_back(collect_software());
    }

    return results;
}
