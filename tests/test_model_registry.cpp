// ModelRegistry 单测（任务分解 T3.3，x86 纯逻辑：工厂注入 Soft/Fake 假件）
// 覆盖：load_all 好坏混合（坏包 disabled 不拖垮）/ get 未启用返回空 / snapshot 字段 /
//       selftest 失败 → disabled 带原因 / reload 新包损坏 → .bak 回滚成功 /
//       新旧全坏 → disabled（1002 语义由调度侧 get==nullptr 承接）
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/fakes/fake_infer.h"
#include "core/fakes/soft_letterbox.h"
#include "model/ModelRegistry.h"
#include "test_util.h"

using namespace aibox;
namespace fs = std::filesystem;

namespace {

const char* kManifest = R"({
  "algo_id": "ALGOID",
  "version": "1.0.0",
  "platform": ")" AIBOX_TARGET_PLATFORM R"(",
  "model_file": "model.rknn",
  "input_w": 640,
  "input_h": 640,
  "num_cls": 80,
  "default_threshold": 0.45,
  "core": "steady"
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
    void pkg(const std::string& id) {
        auto d = root / id;
        fs::create_directories(d);
        std::ofstream(d / "manifest.json")
            << std::string(kManifest).replace(std::string(kManifest).find("ALGOID"), 6, id);
        // 24576B 模型文件：snapshot 的 mem_kb 估算断言依据（24576/1024 = 24KB）
        std::ofstream m(d / "model.rknn", std::ios::binary);
        for (int i = 0; i < 24576; ++i) m << 'x';
    }
    void pkg_raw(const std::string& id, const std::string& text) {
        auto d = root / id;
        fs::create_directories(d);
        std::ofstream(d / "manifest.json") << text;
        std::ofstream m(d / "model.rknn", std::ios::binary);
        m << "fake-model";
    }
};

// 可编程失败的推理假件：selftest 路径验证用
class FailInfer : public IInferEngine {
public:
    bool load(const std::string&) override { return true; }
    bool run(const InferTensor&, std::vector<std::vector<float>>&) override {
        return false;  // 模拟运行期错误（内存不足/核绑定失败等）
    }
};

ModelRegistry::RegistryDeps ok_deps() {
    ModelRegistry::RegistryDeps d;
    d.make_preproc = [] {
        return std::static_pointer_cast<IPreproc>(std::make_shared<SoftLetterbox>());
    };
    d.make_infer = [](const AlgoManifest&, const std::string&,
                      std::string*) -> std::shared_ptr<IInferEngine> {
        Detection det;
        det.score = 0.9f;
        det.x = det.y = 100;
        det.w = det.h = 50;
        Targets t;
        t.boxes.push_back(det);
        return std::make_shared<FakeInfer>(std::vector<Targets>{t});
    };
    d.make_post = nullptr;  // FakeInfer 直出 [n,6] → default_post
    return d;
}

}  // namespace

static void run() {
    TempTree t("aibox_registry_test");

    // ---- 1. 好坏混合 load_all：坏包 disabled + 原因，好包可获取 ----
    t.pkg("good");
    t.pkg_raw("broken", R"({"algo_id":"broken","version":"1","platform":"no-such-plat"})");
    ModelRegistry reg(t.root.string(), ok_deps());
    reg.load_all();
    CHECK(reg.size() == 2);
    CHECK(reg.get("good") != nullptr);
    CHECK(reg.get("good")->preproc != nullptr);
    CHECK(reg.get("good")->infer != nullptr);
    CHECK(reg.get("broken") == nullptr);  // disabled → get 空（set_algos 预检 1002 语义）
    CHECK(reg.get("missing") == nullptr);

    auto snap = reg.snapshot();
    CHECK(snap.algos.size() == 2);
    for (auto& a : snap.algos) {
        if (a.algo_id == "good") {
            CHECK(!a.disabled);
            CHECK(a.version == "1.0.0");
            CHECK(a.trigger == "steady");
            CHECK(a.model_ref == "model.rknn");
            CHECK(a.npu_mem_kb == 24);  // 模型文件体积自动估算（24576B → 24KB）
        } else {
            CHECK(a.disabled);
        }
    }

    // ---- 2. selftest 失败 → disabled + 原因 ----
    TempTree t2("aibox_registry_test2");
    t2.pkg("runfail");
    ModelRegistry::RegistryDeps failing = ok_deps();
    failing.make_infer = [](const AlgoManifest&, const std::string&,
                            std::string*) -> std::shared_ptr<IInferEngine> {
        return std::make_shared<FailInfer>();  // load 成功但 run 恒败
    };
    ModelRegistry reg2(t2.root.string(), std::move(failing));
    reg2.load_all();
    CHECK(reg2.get("runfail") == nullptr);
    auto snap2 = reg2.snapshot();
    CHECK(snap2.algos.size() == 1);
    CHECK(snap2.algos[0].disabled);

    // ---- 3. reload：新包损坏 + .bak 存在 → 回滚成功 ----
    t.pkg("good.bak");  // 合法备份
    // 破坏在线包：manifest 指向不存在模型
    {
        std::ofstream f(t.root / "good" / "manifest.json");
        f << R"({"algo_id":"good","version":"9.9","platform":")" AIBOX_TARGET_PLATFORM
          << R"(","model_file":"gone.rknn","core":"steady"})";
    }
    CHECK(reg.reload("good"));  // 回滚 .bak → 成功
    CHECK(reg.get("good") != nullptr);
    auto snap3 = reg.snapshot();
    bool rolled = false;
    for (auto& a : snap3.algos)
        if (a.algo_id == "good") rolled = !a.disabled;
    CHECK(rolled);

    // ---- 4. reload：新旧全坏 → disabled（不抛异常）----
    TempTree t3("aibox_registry_test3");
    t3.pkg("doomed");
    ModelRegistry reg3(t3.root.string(), ok_deps());
    reg3.load_all();
    CHECK(reg3.get("doomed") != nullptr);
    {
        // 弄坏在线包与备份
        std::ofstream f(t3.root / "doomed" / "manifest.json");
        f << "no json here";
        fs::create_directories(t3.root / "doomed.bak");
        std::ofstream f2(t3.root / "doomed.bak" / "manifest.json");
        f2 << "no json here";
    }
    CHECK(!reg3.reload("doomed"));
    CHECK(reg3.get("doomed") == nullptr);  // 降级为不可用，进程不崩

    // ---- 5. cost_ms_of / reload 不存在的 algo ----
    CHECK_NEAR(reg.cost_ms_of("good"), 0.f, 0.001f);  // manifest 未填 → 0
    CHECK(!reg.reload("never-existed"));
}

TEST_MAIN("model_registry")
