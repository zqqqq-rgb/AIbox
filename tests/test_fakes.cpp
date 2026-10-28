// Fake 四件套最小自测（任务分解 T0.3 DoD）
// 链路：FileSource → FileDecoder → SoftLetterbox → FakeInfer
// 零外部依赖，任何 x86 环境 `cmake --build && ctest` 即可运行
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/fakes/fake_infer.h"
#include "core/fakes/file_decoder.h"
#include "core/fakes/file_source.h"
#include "core/fakes/soft_letterbox.h"

using namespace aibox;

static int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(((a) - (b)) < (eps) && ((b) - (a)) < (eps))

// 生成一帧合成 NV12：Y = (x + y*W) % 256 渐变 + 白块(Y=235)；UV 全 128
static std::vector<uint8_t> make_nv12(uint32_t w, uint32_t h, uint32_t sq_x, uint32_t sq_y,
                                      uint32_t sq_w, uint32_t sq_h) {
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 3 / 2);
    uint8_t* y = f.data();
    uint8_t* uv = f.data() + static_cast<size_t>(w) * h;
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            const bool in_sq = c >= sq_x && c < sq_x + sq_w && r >= sq_y && r < sq_y + sq_h;
            y[r * w + c] = in_sq ? 235 : static_cast<uint8_t>((c + r * w) % 256);
        }
    }
    memset(uv, 128, static_cast<size_t>(w) * h / 2);
    return f;
}

int main() {
    const uint32_t W = 64, H = 48;
    const uint32_t FRAME_SIZE = FileDecoder::nv12_size(W, H);  // 4608
    const char* kTmp = "test_frame.nv12";

    // --- FileSource：写 2 帧合成资产再读回 ---
    {
        std::ofstream f(kTmp, std::ios::binary);
        auto fr = make_nv12(W, H, 16, 8, 16, 16);
        f.write(reinterpret_cast<const char*>(fr.data()), fr.size());
        f.write(reinterpret_cast<const char*>(fr.data()), fr.size());
    }

    FileSource src(kTmp, FRAME_SIZE, 40);
    CHECK(src.open());

    FileDecoder dec;
    CHECK(dec.open(W, H));
    CHECK(!dec.open(63, 48));  // NV12 要求偶数宽

    Packet pkt;
    CHECK(src.read(pkt));
    CHECK(pkt.size == FRAME_SIZE);
    CHECK(pkt.pts_ms == 0);

    Frame fr;
    CHECK(dec.decode(pkt, fr));
    CHECK(fr.frame_index == 0 && fr.width == W && fr.height == H);
    CHECK(fr.stride_y == W && fr.stride_uv == W);
    CHECK(fr.y_ptr != nullptr && fr.uv_ptr == fr.y_ptr + W * H);

    // --- SoftLetterbox：64x48 → 32x32，scale=0.5，pad_y=4，pad_x=0 ---
    SoftLetterbox lb;
    InferTensor tensor;
    tensor.width = 32;
    tensor.height = 32;
    LetterBoxParam param;
    CHECK(lb.run(fr, tensor, param));
    CHECK(tensor.data.size() == 32u * 32u * 3u);
    CHECK_NEAR(param.scale, 0.5f, 1e-5f);
    CHECK_NEAR(param.pad_x, 0.f, 1e-5f);
    CHECK_NEAR(param.pad_y, 4.f, 1e-5f);

    // padding 区（顶部 4 行）保持黑色
    bool pad_black = true;
    for (int x = 0; x < 32 && pad_black; ++x) {
        for (int c = 0; c < 3; ++c) {
            if (tensor.data[(2 * 32 + x) * 3 + c] != 0) { pad_black = false; break; }
        }
    }
    CHECK(pad_black);

    // 白块 src(16,8,16,16) → dst x∈[8,16) y∈[8,12)，中心应近白色
    const uint8_t* px = &tensor.data[(10 * 32 + 12) * 3];
    CHECK(px[0] > 240 && px[1] > 240 && px[2] > 240);

    // --- FakeInfer：预设帧序输出（帧0 有框，帧1 空目标） ---
    std::vector<Targets> presets(2);
    presets[0].frame_pts_ms = 0;
    presets[0].frame_index = 0;
    presets[0].boxes.push_back(Detection{10, 20, 30, 40, 0.9f, 1, -1});
    presets[1].frame_pts_ms = 40;
    presets[1].frame_index = 1;

    FakeInfer inf(presets);
    CHECK(inf.load(""));

    std::vector<std::vector<float>> outputs;
    CHECK(inf.run(tensor, outputs));
    CHECK(outputs.size() == 1 && outputs[0].size() == 6);
    CHECK(outputs[0][0] == 1.f && outputs[0][1] == 0.9f && outputs[0][2] == 10.f);
    CHECK(inf.last_targets().boxes.size() == 1);

    CHECK(inf.run(tensor, outputs));
    CHECK(outputs[0].empty() && inf.last_targets().boxes.empty());  // 空目标边界用例

    // --- EOF 语义：两帧读尽后 read 返回 false ---
    CHECK(src.read(pkt));
    CHECK(src.read(pkt) == false);

    src.close();
    std::remove(kTmp);

    if (g_fail == 0) {
        printf("test_fakes PASSED\n");
    }
    return g_fail;
}
