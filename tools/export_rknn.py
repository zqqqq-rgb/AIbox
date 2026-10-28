from rknn.api import RKNN

# ============ 配置区 ============
ONNX_PATH   = 'yolov8n.onnx'
RKNN_PATH   = 'yolov8n.rknn'
DATASET     = './calib.txt'      # 注意: 内部图片路径相对于本脚本运行目录
PLATFORM    = 'rk3588'           # 验证期 rk3588; 量产需重转 rk3576(架构§11.3)
QUANT       = True               # INT8 量化(架构§11.3: INT8 非对称)
DO_ACC_ANALYSIS = True           # 量化精度自检(模拟器跑, 输出逐层余弦相似度)
# ================================

if __name__ == '__main__':
    r = RKNN(verbose=False)

    # 第一步: config —— 归一化(YOLO: /255 到 0~1) + 目标平台
    r.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform=PLATFORM,
    )

    # 第二步: 加载 ONNX
    # ★前提检查: 本 onnx 必须是 model zoo 兼容版(9 输出分支)。
    #   zoo 的 download_model.sh 下载版 ✓
    #   ultralytics "yolo export format=onnx" 原版 ✗ (C++ demo 后处理对不上)
    #   自训练模型需用 "format=rknn" 导出(zoo 的 RKNN 优化分支)
    ret = r.load_onnx(ONNX_PATH)
    assert ret == 0, "load_onnx 失败: 检查onnx路径/输入shape是否静态[1,3,640,640]"
    print("[1/4] load_onnx OK")

    # 第三步: 构建(INT8 量化)
    ret = r.build(do_quantization=QUANT, dataset=DATASET)
    assert ret == 0, "build 失败: 常见原因是 calib.txt 图片路径错或内存不足"
    print("[2/4] build OK (quantization=%s)" % QUANT)

    # 第四步: 量化精度自检(可选, 强烈建议) —— 模拟器对比 fp/量化输出
    # 关注报告末尾 overall similarity: >0.98 优秀, <0.95 需检查校准集
    if DO_ACC_ANALYSIS:
        # 取校准集中第一张图做分析(路径须与 calib.txt 内写法一致)
        first_img = open(DATASET).readline().strip()
        ret = r.accuracy_analysis(inputs=[first_img], target=None)
        # target=None=本机模拟器(无需板子); 板上运行时可传 'rk3588' 连板分析
        # 报告输出在 ./snapshot 目录
        if ret != 0:
            print("[!] accuracy_analysis 失败(不影响导出), 详见上方日志")

    # 第五步: 导出
    ret = r.export_rknn(RKNN_PATH)
    assert ret == 0, "export 失败"
    print("[3/4] export OK -> %s" % RKNN_PATH)

    # 第六步: 版本登记(架构§15.2 版本锁定表)
    # 板端运行时版本必须与 toolkit 一致: 板上 strings /usr/lib/librknnrt.so | grep version
    print("[4/4] toolkit 版本: 见日志首行 'I rknn-toolkit2 version' —— 请登记入版本锁定表")

    r.release()
    print("全部完成")
