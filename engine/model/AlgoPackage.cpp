#include "model/AlgoPackage.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <json/json.h>

namespace aibox {

namespace {

bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool file_exists_nonempty(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

}  // namespace

AlgoPackage validate_package(const std::string& dir) {
    AlgoPackage pkg;
    pkg.dir = dir;

    std::string manifest_path = dir + "/manifest.json";
    std::string text;
    if (!read_file(manifest_path, text)) {
        pkg.error = "manifest.json 缺失或不可读";
        return pkg;
    }

    Json::Value root;
    Json::CharReaderBuilder b;
    std::string errs;
    std::istringstream ss(text);
    if (!Json::parseFromStream(b, ss, &root, &errs)) {
        pkg.error = "manifest.json 解析失败: " + errs;
        return pkg;
    }

    auto get_str = [&](const char* k) -> std::string {
        return root.isMember(k) && root[k].isString() ? root[k].asString() : "";
    };
    auto get_u32 = [&](const char* k, uint32_t def) -> uint32_t {
        return root.isMember(k) && root[k].isUInt() ? root[k].asUInt() : def;
    };
    auto get_f = [&](const char* k, float def) -> float {
        return root.isMember(k) && root[k].isNumeric()
                   ? static_cast<float>(root[k].asDouble())
                   : def;
    };

    AlgoManifest& m = pkg.manifest;
    m.algo_id = get_str("algo_id");
    m.version = get_str("version");
    m.platform = get_str("platform");
    m.model_file = get_str("model_file");
    m.in_w = get_u32("input_w", 640);
    m.in_h = get_u32("input_h", 640);
    m.num_cls = static_cast<int>(get_u32("num_cls", 80));
    m.threshold = get_f("default_threshold", 0.45f);
    m.core = get_str("core");
    if (m.core.empty()) m.core = "steady";
    m.npu_cost_ms = get_f("npu_cost_ms", 0.f);

    if (m.algo_id.empty()) { pkg.error = "缺 algo_id"; return pkg; }
    if (m.version.empty()) { pkg.error = "缺 version"; return pkg; }
    if (m.platform != AIBOX_TARGET_PLATFORM) {
        pkg.error = "platform 不匹配: " + m.platform + " != " + AIBOX_TARGET_PLATFORM;
        return pkg;
    }
    if (m.model_file.empty()) { pkg.error = "缺 model_file"; return pkg; }
    if (m.in_w < 64 || m.in_h < 64 || m.in_w > 8192 || m.in_h > 8192) {
        pkg.error = "input_w/h 越界";
        return pkg;
    }
    if (m.num_cls <= 0 || m.num_cls > 10000) { pkg.error = "num_cls 越界"; return pkg; }
    if (m.threshold <= 0.f || m.threshold >= 1.f) {
        pkg.error = "default_threshold 须在 (0,1)";
        return pkg;
    }
    if (m.core != "steady" && m.core != "triggered") {
        pkg.error = "core 须为 steady|triggered";
        return pkg;
    }

    // classes 类别白名单（可选）：声明时必须是非空数组、元素为 [0,num_cls) 整数
    // （[] 空数组判坏包——输出全类没有意义且易误导；未声明 = 全类输出，兼容存量包）
    if (root.isMember("classes")) {
        const Json::Value& arr = root["classes"];
        if (!arr.isArray() || arr.empty()) {
            pkg.error = "classes 须为非空数组（未声明则全类输出）";
            return pkg;
        }
        for (const Json::Value& c : arr) {
            if (!c.isInt() || c.asInt() < 0 || c.asInt() >= m.num_cls) {
                pkg.error = "classes 元素须为 [0,num_cls) 整数";
                return pkg;
            }
            m.classes.push_back(c.asInt());
        }
        m.has_classes = true;
    }

    // class_names 类别名映射（可选，与 classes 对齐或覆盖 num_cls 全类）：
    // 对象 {"0":"person", ...} 或数组 ["person","car",...]（数组下标即 id）。
    // 校验：对象键/数组下标须在 [0,num_cls)；提供时前端显示类名而非裸 id
    if (root.isMember("class_names")) {
        const Json::Value& cn = root["class_names"];
        if (cn.isObject()) {
            for (auto it = cn.begin(); it != cn.end(); ++it) {
                const int id = std::atoi(it.key().asCString());
                if (id < 0 || id >= m.num_cls || !it->isString()) {
                    pkg.error = "class_names 键须为 [0,num_cls) 整数串，值为名称";
                    return pkg;
                }
                if (static_cast<size_t>(m.class_names.size()) <= static_cast<size_t>(id)) {
                    m.class_names.resize(static_cast<size_t>(id) + 1);
                }
                m.class_names[static_cast<size_t>(id)] = it->asString();
            }
        } else if (cn.isArray()) {
            if (cn.size() > static_cast<Json::ArrayIndex>(m.num_cls)) {
                pkg.error = "class_names 数组长度不得超 num_cls";
                return pkg;
            }
            m.class_names.reserve(cn.size());
            for (const Json::Value& s : cn) {
                if (!s.isString()) {
                    pkg.error = "class_names 元素须为字符串";
                    return pkg;
                }
                m.class_names.push_back(s.asString());
            }
        } else {
            pkg.error = "class_names 须为对象或数组";
            return pkg;
        }
    }

    std::string model_path = dir + "/" + m.model_file;
    if (!file_exists_nonempty(model_path)) {
        pkg.error = "模型文件缺失或为空: " + model_path;
        return pkg;
    }

    pkg.ok = true;
    return pkg;
}

std::vector<AlgoPackage> scan_packages(const std::string& root) {
    std::vector<AlgoPackage> out;
    DIR* d = opendir(root.c_str());
    if (!d) return out;

    dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        // .bak 是回滚副本，不是可加载包
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bak") == 0) continue;

        std::string dir = root + "/" + name;
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        out.push_back(validate_package(dir));
    }
    closedir(d);
    return out;
}

}  // namespace aibox
