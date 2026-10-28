// AlgoPackage manifest 校验全路径单测（任务分解 T3.3 测试列，x86 纯逻辑）
// 覆盖：合法包 / 平台错 / 缺字段 / 尺寸越界 / core 非法 / 模型缺失 / 坏 JSON /
//       scan 跳过 .bak / root 不存在
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "model/AlgoPackage.h"
#include "test_util.h"

using namespace aibox;
namespace fs = std::filesystem;

namespace {

const char* kValidManifest = R"({
  "algo_id": "yolov8n-base",
  "version": "1.0.2",
  "platform": ")" AIBOX_TARGET_PLATFORM R"(",
  "model_file": "model.rknn",
  "input_w": 640,
  "input_h": 640,
  "num_cls": 80,
  "default_threshold": 0.45,
  "core": "steady",
  "npu_cost_ms": 12.5
})";

struct TempTree {
    fs::path root;
    explicit TempTree(const std::string& name)
        : root(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void pkg(const std::string& id, const std::string& manifest_text,
             bool with_model = true) {
        auto d = root / id;
        fs::create_directories(d);
        std::ofstream(d / "manifest.json") << manifest_text;
        if (with_model) {
            std::ofstream m(d / "model.rknn", std::ios::binary);
            m << "fake-model-bytes";
        }
    }
};

}  // namespace

static void run() {
    TempTree t("aibox_algo_pkg_test");

    // ---- 1. 合法包 ----
    t.pkg("good", kValidManifest);
    auto p = validate_package((t.root / "good").string());
    CHECK_MSG(p.ok, "合法包应通过: %s", p.error.c_str());
    CHECK(p.manifest.algo_id == "yolov8n-base");
    CHECK(p.manifest.version == "1.0.2");
    CHECK(p.manifest.in_w == 640 && p.manifest.in_h == 640);
    CHECK(p.manifest.num_cls == 80);
    CHECK_NEAR(p.manifest.threshold, 0.45f, 0.0001f);
    CHECK(p.manifest.core == "steady");
    CHECK_NEAR(p.manifest.npu_cost_ms, 12.5f, 0.001f);

    // ---- 2. platform 不匹配（架构 §11.4.2 版本/平台闸门）----
    t.pkg("badplat", R"({"algo_id":"x","version":"1","platform":"rk3568",)"
                     R"("model_file":"model.rknn","core":"steady"})");
    p = validate_package((t.root / "badplat").string());
    CHECK(!p.ok);
    CHECK_MSG(p.error.find("platform") != std::string::npos, "错误应指明 platform: %s",
              p.error.c_str());

    // ---- 3. 缺必填字段 ----
    t.pkg("noid", R"({"version":"1","platform":")" AIBOX_TARGET_PLATFORM R"(",)"
                  R"("model_file":"model.rknn","core":"steady"})");
    p = validate_package((t.root / "noid").string());
    CHECK(!p.ok);
    CHECK(p.error.find("algo_id") != std::string::npos);

    // ---- 4. 尺寸越界 / num_cls 越界 / threshold 越界 ----
    t.pkg("bigw", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                  R"(","model_file":"model.rknn","input_w":16384,"core":"steady"})");
    CHECK(!validate_package((t.root / "bigw").string()).ok);
    t.pkg("zerocls", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                     R"(","model_file":"model.rknn","num_cls":0,"core":"steady"})");
    CHECK(!validate_package((t.root / "zerocls").string()).ok);
    t.pkg("badthr", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                    R"(","model_file":"model.rknn","default_threshold":1.5,"core":"steady"})");
    CHECK(!validate_package((t.root / "badthr").string()).ok);

    // ---- 5. core 非法 ----
    t.pkg("badcore", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                     R"(","model_file":"model.rknn","core":"both"})");
    p = validate_package((t.root / "badcore").string());
    CHECK(!p.ok);
    CHECK(p.error.find("core") != std::string::npos);

    // ---- 6. 模型文件缺失 / 空文件 ----
    t.pkg("nomodel", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                     R"(","model_file":"model.rknn","core":"steady"})",
             false /*无模型*/);
    p = validate_package((t.root / "nomodel").string());
    CHECK(!p.ok);
    CHECK(p.error.find("模型文件") != std::string::npos);

    // ---- 7. 坏 JSON ----
    t.pkg("badjson", "{not json");
    CHECK(!validate_package((t.root / "badjson").string()).ok);

    // ---- 8. scan：好包 + 坏包都上报（坏包 disabled 不吞掉）；.bak 跳过；非目录跳过 ----
    t.pkg("yolov8n-p5", kValidManifest);                    // 另一个好包
    t.pkg("good.bak", kValidManifest);                      // 回滚副本
    { std::ofstream f(t.root / "afile.txt"); f << "not a dir"; }
    auto all = scan_packages(t.root.string());
    size_t good = 0, bad = 0;
    bool bak_listed = false;
    for (auto& q : all) {
        auto name = fs::path(q.dir).filename().string();
        if (name == "good" || name == "yolov8n-p5") good += q.ok ? 1 : 0;
        else if (name == "badplat" || name == "badjson") bad += !q.ok ? 1 : 0;
        if (name == "good.bak") bak_listed = true;
    }
    CHECK_MSG(good == 2, "好包应 2 个, 实得 %zu", good);
    CHECK_MSG(bad == 2, "坏包应以 disabled 出现 2 个, 实得 %zu", bad);
    CHECK_MSG(!bak_listed, ".bak 不得进入扫描结果");

    // ---- 9. root 不存在 → 空结果（不抛异常）----
    CHECK(scan_packages(t.root.string() + "/nonexistent").empty());

    // ---- 10. classes 白名单 ----
    // 10a. 未声明 = 全类输出（has_classes=false，兼容存量包）
    p = validate_package((t.root / "good").string());
    CHECK(p.ok && !p.manifest.has_classes && p.manifest.classes.empty());

    // 10b. 声明合法：多类白名单解析保序
    t.pkg("clsgood", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                     R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                     R"("classes":[0,5,7]})");
    p = validate_package((t.root / "clsgood").string());
    CHECK_MSG(p.ok, "合法 classes 应通过: %s", p.error.c_str());
    CHECK(p.manifest.has_classes);
    CHECK(p.manifest.classes.size() == 3);
    if (p.manifest.classes.size() == 3) {
        CHECK(p.manifest.classes[0] == 0 && p.manifest.classes[1] == 5 &&
              p.manifest.classes[2] == 7);
    }

    // 10c. 空数组 = 坏包（输出全类无意义且易误导）
    t.pkg("clsempty", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                     R"(","model_file":"model.rknn","core":"steady","classes":[]})");
    p = validate_package((t.root / "clsempty").string());
    CHECK(!p.ok);
    CHECK(p.error.find("classes") != std::string::npos);

    // 10d. 越界元素（>= num_cls）/ 负数 / 非数组 = 坏包
    t.pkg("clsoob", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                    R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                    R"("classes":[0,80]})");
    CHECK(!validate_package((t.root / "clsoob").string()).ok);
    t.pkg("clsneg", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                    R"(","model_file":"model.rknn","core":"steady","classes":[-1]})");
    CHECK(!validate_package((t.root / "clsneg").string()).ok);
    t.pkg("clsnotarr", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                      R"(","model_file":"model.rknn","core":"steady","classes":0})");
    p = validate_package((t.root / "clsnotarr").string());
    CHECK(!p.ok);
    CHECK(p.error.find("classes") != std::string::npos);

    // ---- 11. class_names 类别名映射 ----
    // 11a. 对象形态 {"0":"person"}：解析到下标 0
    t.pkg("cnsobj", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                   R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                   R"("classes":[0,5],"class_names":{"0":"person","5":"bus"}})");
    p = validate_package((t.root / "cnsobj").string());
    CHECK_MSG(p.ok, "对象 class_names 应通过: %s", p.error.c_str());
    CHECK(p.manifest.class_names.size() == 6);  // 稀疏键 resize：0~5
    if (p.manifest.class_names.size() == 6) {
        CHECK(p.manifest.class_names[0] == "person");
        CHECK(p.manifest.class_names[5] == "bus");
    }

    // 11b. 数组形态（下标即 id）
    t.pkg("cnsarr", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                   R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                   R"("class_names":["person","car"]})");
    p = validate_package((t.root / "cnsarr").string());
    CHECK_MSG(p.ok, "数组 class_names 应通过: %s", p.error.c_str());
    CHECK(p.manifest.class_names.size() == 2);
    if (p.manifest.class_names.size() == 2) CHECK(p.manifest.class_names[1] == "car");

    // 11c. 坏值：键越界 / 数组超长 / 非字符串元素
    t.pkg("cnsoob", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                   R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                   R"("class_names":{"80":"x"}})");
    CHECK(!validate_package((t.root / "cnsoob").string()).ok);
    t.pkg("cnslong", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                    R"(","model_file":"model.rknn","num_cls":2,"core":"steady",)"
                    R"("class_names":["a","b","c"]})");
    CHECK(!validate_package((t.root / "cnslong").string()).ok);
    t.pkg("cnsnum", R"({"algo_id":"x","version":"1","platform":")" AIBOX_TARGET_PLATFORM
                   R"(","model_file":"model.rknn","num_cls":80,"core":"steady",)"
                   R"("class_names":[1]})");
    CHECK(!validate_package((t.root / "cnsnum").string()).ok);
}

TEST_MAIN("algo_package")
