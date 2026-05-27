#include "reporters.h"
#include "utils.h"

#include <algorithm>
#include <iostream>
#include <sstream>

// ===================================================================
// TableReporter
// ===================================================================

static std::string status_label(Status s) {
    switch (s) {
        case Status::OK:      return "OK";
        case Status::WARNING: return "WARNING";
        case Status::DANGER:  return "DANGER";
        case Status::INFO:    return "INFO";
        case Status::NA:      return "N/A";
    }
    return "?";
}

static std::string pad_right(const std::string& s, int width) {
    if ((int)s.length() >= width) return s.substr(0, width);
    return s + std::string(width - s.length(), ' ');
}

void TableReporter::print_header() {
    std::cout << "\n" << utils::color_status(Status::INFO)
              << "=== USE Method: Linux Performance Analysis ==="
              << utils::color_reset() << "\n";
    std::cout << "Timestamp: " << utils::timestamp() << "\n";
}

void TableReporter::print_metric(const Metric& m) {
    std::string status_str = status_label(m.status);
    std::string colored_status = utils::colorize(status_str, m.status);
    std::string colored_value = utils::colorize(m.value, m.status);

    std::cout << "  " << pad_right(m.name, 14)
              << colored_value << "  " << colored_status << "\n";

    if (!m.detail.empty()) {
        std::cout << "  " << std::string(14, ' ')
                  << utils::colorize(m.detail, Status::INFO) << "\n";
    }
}

void TableReporter::print_resource(const ResourceResult& r) {
    // Skip if all OK and show_ok_ is false
    if (!show_ok_) {
        bool all_ok = true;
        for (auto& m : r.metrics) {
            if (m.status != Status::OK && m.status != Status::INFO && m.status != Status::NA) {
                all_ok = false;
                break;
            }
        }
        if (all_ok) return;
    }

    std::string title = " " + r.resource + " ";
    int box_width = 60;
    int dashes = box_width - 2 - title.length();
    int left = dashes / 2;
    int right = dashes - left;

    std::string dash_line = std::string(left, '-') + title + std::string(right, '-');
    std::cout << "\n" << utils::color_status(Status::INFO)
              << dash_line
              << utils::color_reset() << "\n";

    for (const auto& m : r.metrics) {
        print_metric(m);
    }
}

void TableReporter::print_summary(const std::vector<ResourceResult>& results) {
    int total_issues = utils::count_issues(results);
    int total_metrics = 0;
    for (auto& r : results) total_metrics += r.metrics.size();

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "Summary: " << total_issues << " issue(s) across "
              << results.size() << " resource(s) (" << total_metrics << " metrics)\n";

    std::vector<std::string> issue_list;
    for (auto& r : results) {
        for (auto& m : r.metrics) {
            if (m.status == Status::WARNING || m.status == Status::DANGER) {
                issue_list.push_back(r.resource + ": " + m.name + "=" + m.value);
            }
        }
    }
    if (!issue_list.empty()) {
        std::cout << "Issues:\n";
        for (auto& iss : issue_list) {
            std::cout << "  " << utils::colorize("⚠", Status::WARNING) << " " << iss << "\n";
        }
    } else {
        std::cout << utils::colorize("  All OK!", Status::OK) << "\n";
    }
}

void TableReporter::report(const std::vector<ResourceResult>& results) {
    print_header();
    for (const auto& r : results) {
        print_resource(r);
    }
    print_summary(results);
}

// ===================================================================
// JsonReporter
// ===================================================================

std::string JsonReporter::status_to_string(Status s) {
    switch (s) {
        case Status::OK:      return "ok";
        case Status::WARNING: return "warning";
        case Status::DANGER:  return "danger";
        case Status::INFO:    return "info";
        case Status::NA:      return "na";
    }
    return "unknown";
}

std::string JsonReporter::escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

void JsonReporter::report(const std::vector<ResourceResult>& results) {
    std::ostringstream json;

    json << "{\n";
    json << "  \"timestamp\": \"" << escape_json(utils::timestamp()) << "\",\n";
    json << "  \"issues\": " << utils::count_issues(results) << ",\n";
    json << "  \"resources\": [\n";

    for (size_t i = 0; i < results.size(); i++) {
        auto& r = results[i];
        json << "    {\n";
        json << "      \"name\": \"" << escape_json(r.resource) << "\",\n";
        json << "      \"metrics\": [\n";

        for (size_t j = 0; j < r.metrics.size(); j++) {
            auto& m = r.metrics[j];
            json << "        {\n";
            json << "          \"name\": \"" << escape_json(m.name) << "\",\n";
            json << "          \"value\": \"" << escape_json(m.value) << "\",\n";
            json << "          \"status\": \"" << status_to_string(m.status) << "\",\n";
            json << "          \"detail\": \"" << escape_json(m.detail) << "\",\n";
            json << "          \"command\": \"" << escape_json(m.command) << "\"\n";
            json << "        }";
            if (j < r.metrics.size() - 1) json << ",";
            json << "\n";
        }

        json << "      ]\n";
        json << "    }";
        if (i < results.size() - 1) json << ",";
        json << "\n";
    }

    json << "  ]\n";
    json << "}\n";

    std::cout << json.str();
}
