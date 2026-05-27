#include "flame_graph.h"
#include "executor.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

// ===================================================================
// Internal tree node for flame graph
// ===================================================================
struct FlameNode {
    std::string name;
    std::string comm;        // process name
    uint64_t    count;
    double      x;           // normalized X position
    double      width;       // normalized width (fraction of total)
    int         depth;       // 0 = bottom (root)
    bool        is_kernel;

    std::vector<std::unique_ptr<FlameNode>> children;
    std::map<std::string, FlameNode*>       child_map; // during building

    FlameNode(const std::string& n, uint64_t c, int d, bool k,
              const std::string& cm = "")
        : name(n), comm(cm), count(c), x(0), width(0),
          depth(d), is_kernel(k) {}
};

// ===================================================================
// Color helpers  —  HSL → RGB,  function-name hash
// ===================================================================
static uint32_t fnv_hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Hue ∈ [0,360), Sat ∈ [0,1], Lit ∈ [0,1] → "rgb(r,g,b)"
static std::string hsl_color(double hue, double sat, double lit) {
    auto f = [](double p, double q, double t) -> double {
        if (t < 0) t += 1.0;
        if (t > 1) t -= 1.0;
        if (t < 1.0/6) return p + (q-p)*6*t;
        if (t < 0.5)   return q;
        if (t < 2.0/3) return p + (q-p)*(2.0/3 - t)*6;
        return p;
    };
    double q = lit < 0.5 ? lit*(1+sat) : lit+sat-lit*sat;
    double p = 2*lit - q;
    double r = f(p, q, hue/360.0 + 1.0/3);
    double g = f(p, q, hue/360.0);
    double b = f(p, q, hue/360.0 - 1.0/3);
    char buf[48];
    snprintf(buf, sizeof(buf), "rgb(%d,%d,%d)",
             int(r*255), int(g*255), int(b*255));
    return buf;
}

// Decide fill colour for a frame
// Kernel → blue-ish, idle → green-ish, others → warm (name-hash)
static std::string frame_color(const FlameNode* node) {
    if (node->is_kernel) {
        // Blue tones
        uint32_t h = fnv_hash(node->name);
        double hue = 200.0 + (h % 40);
        return hsl_color(hue, 0.55, 0.55 + (h % 10) * 0.01);
    }
    // Idle detection
    std::string lower = node->name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("idle") != std::string::npos ||
        lower.find("halt") != std::string::npos ||
        lower.find("sleep") != std::string::npos) {
        uint32_t h = fnv_hash(node->name);
        double hue = 100.0 + (h % 40); // green
        return hsl_color(hue, 0.50, 0.60 + (h % 10) * 0.01);
    }
    // Warm palette
    uint32_t h = fnv_hash(node->name);
    double hue = 15.0 + (h % 50);           // red-orange range ~15–65
    double sat = 0.65 + (h % 15) * 0.01;    // 0.65–0.79
    double lit = 0.50 + (h % 20) * 0.01;    // 0.50–0.69
    return hsl_color(hue, sat, lit);
}

// ===================================================================
// Tree building
// ===================================================================
static uint64_t build_tree(
    const std::vector<AggregatedStack>& stacks,
    FlameNode* root)
{
    uint64_t total = 0;
    for (const auto& s : stacks) {
        uint64_t cnt = s.count;
        total += cnt;

        // frames are leaf → root; we walk root → leaf
        FlameNode* cur = root;
        for (int i = (int)s.frames.size() - 1; i >= 0; i--) {
            const std::string& fname = s.frames[i];
            auto it = cur->child_map.find(fname);
            if (it != cur->child_map.end()) {
                cur = it->second;
                cur->count += cnt;
            } else {
                bool is_kernel = (fname.find("k]") != std::string::npos ||
                                  fname.find("sys_") == 0 ||
                                  fname.find("do_") == 0);
                auto node = new FlameNode(fname, cnt,
                    cur->depth + 1, is_kernel, s.comm);
                cur->child_map[fname] = node;
                cur->children.emplace_back(node);
                cur = node;
            }
        }
    }
    return total;
}

// ===================================================================
// Layout: assign X and width to every node
//
// node->x and node->width must be pre-set before calling.
// Children are sized proportionally to their count share.
// ===================================================================
static void assign_layout(FlameNode* node)
{
    double cx = node->x;

    std::sort(node->children.begin(), node->children.end(),
        [](const std::unique_ptr<FlameNode>& a,
           const std::unique_ptr<FlameNode>& b) { return a->name < b->name; });

    for (auto& child : node->children) {
        child->x = cx;
        child->width = (node->count > 0)
            ? (static_cast<double>(child->count) / node->count) * node->width
            : 0.0;
        assign_layout(child.get());
        cx += child->width;
    }
}

// ===================================================================
// SVG rendering
// ===================================================================
static const int FRAME_H    = 17;   // height per frame row
static const int SVG_W      = 1200; // SVG viewport width
static const int TOP_MARGIN = 40;   // space for title / info
static const int BOT_MARGIN = 4;
static const int MIN_TEXT_W = 30;   // minimum rect width to show text

static std::string escape_xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

static void render_svg(const FlameNode* root,
                       uint64_t total_samples,
                       int max_depth,
                       std::ostream& os)
{
    int svg_h = TOP_MARGIN + max_depth * FRAME_H + BOT_MARGIN;
    double sf = static_cast<double>(SVG_W); // scale factor: normalized [0,1] → px

    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
       << "     viewBox=\"0 0 " << SVG_W << " " << svg_h << "\"\n"
       << "     style=\"background-color: #1a1a1a; font-family: 'DejaVu Sans Mono', monospace;\">\n";

    // Title
    os << "<text x=\"" << SVG_W/2 << "\" y=\"22\" text-anchor=\"middle\"\n"
       << "      fill=\"#ccc\" font-size=\"14\" font-weight=\"bold\">"
       << "CPU Flame Graph  —  " << total_samples << " samples"
       << " (" << (total_samples / 99) << "s @ 99 Hz)"
       << "</text>\n";

    os << "<text x=\"10\" y=\"36\" fill=\"#888\" font-size=\"10\">"
       << "Frame width ∝ CPU time  ·  hover for details  ·  "
       << "warm = user  ·  blue = kernel  ·  green = idle"
       << "</text>\n";

    // Help overlay note
    os << "<text x=\"" << (SVG_W - 10) << "\" y=\"36\" text-anchor=\"end\""
       << " fill=\"#555\" font-size=\"10\">"
       << "perf flame graph</text>\n";

    // Render each node BFS by depth (roots at bottom, leaves at top)
    // Group nodes by depth for rendering
    std::vector<std::vector<const FlameNode*>> levels(max_depth + 1);
    {
        std::vector<const FlameNode*> queue;
        queue.push_back(root);
        while (!queue.empty()) {
            auto node = queue.back(); queue.pop_back();
            if (node->depth >= 0 && node->depth <= max_depth) {
                levels[node->depth].push_back(node);
            }
            for (const auto& c : node->children) {
                queue.push_back(c.get());
            }
        }
    }

    for (int d = max_depth; d >= 0; d--) {
        double y_base = svg_h - BOT_MARGIN - (d + 1) * FRAME_H;

        for (const auto* node : levels[d]) {
            if (node->width < 0.001) continue;

            double rx = node->x * sf;
            double rw = node->width * sf;
            if (rw < 1.0) rw = 1.0; // minimum visible width

            std::string fill = frame_color(node);
            std::string name = escape_xml(node->name);
            double pct = total_samples > 0
                ? (100.0 * node->count / total_samples) : 0.0;

            // Rectangle with hover
            os << "<g>\n";
            os << "<rect x=\"" << rx << "\" y=\"" << y_base
               << "\" width=\"" << rw << "\" height=\"" << (FRAME_H - 1)
               << "\" fill=\"" << fill << "\" rx=\"1\"\n";
            os << "      onmouseover=\"this.style.stroke='#fff';this.style['stroke-width']='1.5'\"\n";
            os << "      onmouseout=\"this.style.stroke='';this.style['stroke-width']=''\"\n";
            os << "      pointer-events=\"all\" />\n";

            // Tooltip
            os << "<title>"
               << name << " (" << node->count << " samples, "
               << std::fixed << std::setprecision(1) << pct << "%)"
               << "\nprocess: " << escape_xml(node->comm)
               << (node->is_kernel ? "\n[kernel]" : "")
               << "</title>\n";

            // Text label (only if wide enough)
            if (rw >= MIN_TEXT_W) {
                // Truncate text if needed
                int max_chars = static_cast<int>((rw - 6) / 7); // ~7px per char at font 10
                std::string label = node->name;
                if (static_cast<int>(label.length()) > max_chars && max_chars > 3) {
                    label = label.substr(0, max_chars - 2) + "..";
                }
                if (static_cast<int>(label.length()) <= max_chars) {
                    os << "<text x=\"" << (rx + 4) << "\" y=\"" << (y_base + 12)
                       << "\" fill=\"#000\" font-size=\"10\" opacity=\"0.85\">"
                       << escape_xml(label) << "</text>\n";
                }
            }

            // Percentage label for wide frames
            if (rw >= 80 && pct > 1.0) {
                os << "<text x=\"" << (rx + rw - 4) << "\" y=\"" << (y_base + 12)
                   << "\" fill=\"#000\" font-size=\"9\" text-anchor=\"end\" opacity=\"0.60\">"
                   << std::fixed << std::setprecision(1) << pct << "%</text>\n";
            }

            os << "</g>\n";
        }
    }

    os << "</svg>\n";
}

// ===================================================================
// Public entry point
// ===================================================================
std::string generate_flame_graph(const std::vector<AggregatedStack>& stacks,
                                 const std::string& output_path)
{
    if (stacks.empty()) return "";

    // --- build tree ---
    // Virtual root that groups every top-level frame
    FlameNode root("__root__", 0, -1, false);
    uint64_t total = build_tree(stacks, &root);

    if (total == 0) return "";

    // --- layout ---
    // Virtual root spans the full width
    root.count = total;
    root.x = 0.0;
    root.width = 1.0;

    // Top-level children get proportional widths, then cascade recursively
    double cx = 0.0;
    std::sort(root.children.begin(), root.children.end(),
        [](const std::unique_ptr<FlameNode>& a,
           const std::unique_ptr<FlameNode>& b) { return a->name < b->name; });
    for (auto& child : root.children) {
        child->x = cx;
        child->width = static_cast<double>(child->count) / total;
        assign_layout(child.get());
        cx += child->width;
    }

    // --- compute max depth ---
    int max_depth = 0;
    {
        std::vector<FlameNode*> stack;
        for (auto& c : root.children) stack.push_back(c.get());
        while (!stack.empty()) {
            auto n = stack.back(); stack.pop_back();
            if (n->depth > max_depth) max_depth = n->depth;
            for (auto& c : n->children) stack.push_back(c.get());
        }
    }

    // --- determine output path ---
    std::string path = output_path;
    if (path.empty()) {
        path = "perf_flame_" + utils::timestamp() + ".svg";
        // Replace colons for filesystem safety
        std::replace(path.begin(), path.end(), ':', '-');
    }

    // --- render to file ---
    std::ofstream ofs(path);
    if (!ofs) return "";

    render_svg(&root, total, max_depth, ofs);
    ofs.close();

    // --- also try to open in browser? no, just print path ---

    return path;
}
