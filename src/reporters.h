#ifndef REPORTERS_H
#define REPORTERS_H

#include <vector>
#include "types.h"

class Reporter {
public:
    virtual ~Reporter() = default;
    virtual void report(const std::vector<ResourceResult>& results) = 0;
};

class TableReporter : public Reporter {
public:
    explicit TableReporter(bool show_ok = true) : show_ok_(show_ok) {}
    void report(const std::vector<ResourceResult>& results) override;
    void set_show_ok(bool s) { show_ok_ = s; }

private:
    bool show_ok_;
    void print_header();
    void print_metric(const Metric& m);
    void print_resource(const ResourceResult& r);
    void print_summary(const std::vector<ResourceResult>& results);
};

class JsonReporter : public Reporter {
public:
    void report(const std::vector<ResourceResult>& results) override;

private:
    std::string escape_json(const std::string& s);
    std::string status_to_string(Status s);
};

#endif // REPORTERS_H
