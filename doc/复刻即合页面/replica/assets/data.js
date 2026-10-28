/**
 * 本地 mock 数据层。
 * 数据来源：原始构建产物 index-C6Fr3n16.js 中内嵌的能力目录（Qe(...) 共 103 条），
 * 以及抓取到的页面状态（设备为空库、告警为空等新装状态）。
 */
(function (global) {
  /** 导航配置：与 AppShell 中 n=[{to,label,icon,note}] 完全一致 */
  var NAV = [
    { to: '/', label: '监控大屏', icon: 'gateway', note: '视频墙与实时告警' },
    { to: '/admin/cameras', label: '摄像头管理', icon: 'camera', note: '设备接入与运行状态' },
    { to: '/admin/rules', label: '算法任务', icon: 'rule', note: '任务配置与编辑' },
    {
      to: '/admin/algorithm-warehouse',
      label: '算法仓库',
      icon: 'model',
      note: '算法能力与在线更新'
    },
    { to: '/admin/base-library', label: '底库管理', icon: 'database', note: '人员与目标身份底库' },
    { to: '/admin/ai-review', label: 'AI研判', icon: 'algorithm', note: '云端协同识别与告警研判' },
    { to: '/admin/events', label: '告警事件', icon: 'alert', note: '研判与处置闭环' },
    { to: '/admin/notifications', label: '通知中心', icon: 'online', note: '告警渠道与投递状态' },
    {
      to: '/admin/hardware-linkage',
      label: '硬件联动',
      icon: 'gateway',
      note: 'GPIO、RS485 与网络语音联动'
    },
    { to: '/admin/system', label: '系统设置', icon: 'system', note: '资源状态与系统配置' }
  ];

  /**
   * 真实算法包元数据：取自设备 2026-10-20 的 /api/algorithms 与 /api/face-recognition/status。
   * 共 7 个已安装算法包，且全部 production_allowed / formal_event_ready / runtime_ready = true。
   * 注意：设备上没有 fight_yolov8 与 fire_smoke_yolov8 包，因此打架检测与烟雾检测为“待接入”。
   */
  var ALGOS = {
    person_detector_rk3576: { component: '人员检测', version: '1.0.0-ppyoloe-s-int8' },
    retinaface_mobile_rk3576: { component: '人脸检测', version: '0.2.0-runtime-2.3.2' },
    hyperlpr3_plate_detector_rk3576: { component: '通用车牌检测', version: '1.0.0-hyperlpr3-fp16' },
    pose_yolov8s_rk3576: { component: '姿态检测', version: '0.1.2-contract4' },
    fall_yolov8_rk3576: { component: '摔倒检测', version: '0.1.3-contract4' },
    smoking_yolov8_rk3576: { component: '抽烟检测', version: '0.1.6-temporal-confirmation' },
    ppe_yolov8_rk3576: { component: '安全装备检测', version: '0.1.1-contract3' },
    sface: { component: 'sface_2021dec_rk3576', version: 'sface-2021dec-rk3576-fp16-v1' }
  };

  /** 能力分类：id / 名称 / 图标 / 描述，顺序与原始定义一致 */
  var CATEGORIES = [
    { id: 'identity', name: '人员身份', icon: 'user', desc: '人员身份建库、比对和识别' },
    { id: 'behavior', name: '行为姿态', icon: 'health', desc: '识别人员行为、状态与异常动作' },
    { id: 'safety', name: '生产安全', icon: 'ppe', desc: '劳保穿戴、危险区域与现场作业安全' },
    { id: 'fire', name: '烟火险情', icon: 'alert', desc: '烟火、消防设施与动火作业风险' },
    { id: 'vehicle', name: '车辆车牌', icon: 'vehicle', desc: '车辆、车牌与叉车作业风险' },
    { id: 'general', name: '通用视觉', icon: 'algorithm', desc: '通用区域规则与客流统计' }
  ];

  /** [id, 名称, 说明, 绑定算法, 标签] —— 共 103 条 */
  var ITEMS = {
    identity: [
      ['face-detection', '人脸检测', '检测画面中的人脸位置与五点关键特征。', 'retinaface_mobile_rk3576'],
      ['face-recognition', '人脸识别', '识别人脸并与人员底库进行身份比对。', 'retinaface_mobile_rk3576'],
      ['face-feature', '人脸特征提取', '将对齐后的人脸转换为身份特征，用于底库比对。', 'sface'],
      ['unknown-person', '陌生人员识别', '发现未录入人员底库的陌生人员。', 'retinaface_mobile_rk3576'],
      ['person-list-match', '黑白名单人员识别', '按人员名单识别重点关注或授权人员。', 'retinaface_mobile_rk3576'],
      ['person-reid', '跨镜人员识别', '在多个摄像头之间关联同一人员的出现记录。', null]
    ],
    behavior: [
      ['crowd', '人员聚集检测', '发现指定区域内人员数量超过设定范围。', 'person_detector_rk3576'],
      ['loitering', '人员徘徊检测', '发现人员在指定区域内持续活动并达到设定轨迹距离。', 'person_detector_rk3576'],
      ['staying', '人员滞留检测', '发现同一人员在指定区域停留超过设定时长。', 'person_detector_rk3576'],
      ['off-duty', '离岗检测', '发现值守岗位在规定时段内无人。', 'person_detector_rk3576'],
      ['sleeping-duty', '睡岗检测', '发现值守人员趴卧、闭眼或持续静止等睡岗状态。', null],
      ['pose-analysis', '人体姿态识别', '识别站立、坐姿、蹲姿、弯腰、举手和躺卧等人体姿态。', 'pose_yolov8s_rk3576'],
      ['fall', '跌倒／倒地检测', '发现人员跌倒或持续倒地状态。', 'fall_yolov8_rk3576'],
      ['fight', '打架检测', '发现人员之间疑似打斗或暴力冲突行为。', null],
      ['smoking', '抽烟检测', '发现人员手持香烟或正在抽烟的行为。', 'smoking_yolov8_rk3576'],
      ['phone-use', '使用手机／打电话检测', '发现人员持续看手机或手持电话通话。', null],
      ['climbing', '攀爬检测', '发现人员攀爬设备、墙体或其他高处设施。', null],
      ['fence-crossing', '翻越围栏检测', '发现人员翻越指定围栏或隔离设施。', null],
      ['line-crossing-body', '跨越或钻越输送线', '发现人员跨越或钻越生产输送线。', null],
      ['conveyor-crossing', '跨越或钻越传送带', '发现人员跨越或钻越运行中的传送带。', null]
    ],
    safety: [
      ['no-helmet', '未戴安全帽检测', '发现作业人员未正确佩戴安全帽。', 'ppe_yolov8_rk3576'],
      ['no-vest', '未穿反光衣检测', '发现作业人员未穿反光背心或安全背心。', 'ppe_yolov8_rk3576'],
      ['no-workwear', '未穿工服检测', '发现人员未按要求穿着指定工服。', null],
      ['no-gloves', '未戴手套检测', '发现作业人员未佩戴防护手套。', null],
      ['no-goggles', '未戴护目镜检测', '发现作业人员未佩戴护目镜。', null],
      ['no-safety-shoes', '未穿防护鞋检测', '发现作业人员未穿安全鞋或防护鞋。', null],
      ['no-mask', '未戴口罩检测', '发现作业人员未正确佩戴口罩。', 'ppe_yolov8_rk3576'],
      ['person-vehicle-near', '人车靠近预警', '发现人员与车辆距离过近并持续后预警。', 'person_detector_rk3576'],
      ['forklift-blind-zone', '叉车盲区人员闯入', '发现人员进入叉车驾驶员视野盲区。', null],
      ['forklift-reversing-person', '叉车倒车后方有人', '叉车倒车时发现后方存在人员。', null],
      ['dock-edge-person', '装卸月台边缘人员靠近', '发现人员过度靠近装卸月台边缘。', 'person_detector_rk3576'],
      ['automation-danger-zone', '自动化设备危险区人员闯入', '发现人员进入自动化设备危险运行区域。', null],
      ['warehouse-aisle-blocked', '仓储作业通道堆货', '发现货物占用仓储作业通道。', null],
      ['cargo-stack-high', '货物堆放超高', '发现货物堆放高度超过安全限制。', null],
      ['cargo-stack-tilt', '货物堆放倾斜预警', '发现堆放货物明显倾斜并存在倒塌风险。', null],
      ['pallet-misaligned', '托盘放置不到位', '发现托盘未放置在规定位置或明显偏移。', null],
      ['rack-cargo-protrusion', '货架货物探出', '发现货物超出货架安全边界。', null],
      ['rack-deformation', '货架横梁或立柱变形', '发现货架横梁或立柱存在明显变形。', null],
      ['rack-guard-missing', '货架防撞护栏缺失', '发现货架防撞护栏缺失或未在位。', null],
      ['dock-mixed-traffic', '装卸区人车混行', '发现装卸区域人员与车辆混行风险。', null],
      ['vehicle-dock-blind-zone', '人员进入车辆与月台盲区', '发现人员进入车辆与月台之间的盲区。', null],
      ['cargo-drop', '装卸货物掉落检测', '发现装卸过程中货物跌落。', null],
      ['sorting-pinch-hand', '分拣线夹点手部靠近', '发现手部靠近分拣线夹点危险区域。', null],
      ['equipment-danger-zone', '设备运行危险区人员闯入', '设备运行时发现人员进入危险区域。', null],
      ['safety-door-open-running', '安全门开启状态运行', '发现安全门开启时设备仍处于运行状态。', null],
      ['machine-guard-open', '机械防护罩缺失或开启', '发现机械防护罩缺失或未关闭。', null],
      ['limb-in-danger-zone', '人员肢体伸入设备危险区', '发现人员肢体伸入设备危险区域。', null],
      ['robot-work-zone', '工业机器人工作区人员闯入', '发现人员进入工业机器人运行区域。', null],
      ['confined-space-no-guardian', '有限空间作业无监护人', '发现有限空间作业现场缺少监护人员。', null],
      ['maintenance-no-lockout', '检维修未挂牌上锁', '发现检维修作业未执行挂牌上锁。', null],
      ['confined-space-not-isolated', '有限空间入口未隔离', '发现有限空间入口未设置有效隔离。', null],
      ['conveyor-pinch-hand', '传送带夹点手部靠近', '发现手部靠近传送带夹点危险区域。', null],
      ['person-under-hoist', '行车吊物下方有人', '发现吊装物下方存在人员。', null],
      ['crane-operating-zone', '行车运行区人员闯入', '发现人员进入行车运行区域。', null],
      ['cabinet-door-open-long', '配电柜门长时间开启', '发现配电柜门开启超过设定时长。', null],
      ['cabinet-front-blocked', '配电柜前方堆物', '发现配电柜前方安全区域被物品占用。', null],
      ['height-no-harness', '高处作业未佩戴安全带', '发现高处作业人员未佩戴安全带。', null],
      ['harness-not-anchored', '安全带未正确系挂', '发现安全带未正确连接可靠锚点。', null],
      ['edge-proximity', '临边危险靠近预警', '发现人员靠近临边危险区域。', 'person_detector_rk3576'],
      ['opening-proximity', '洞口危险靠近预警', '发现人员靠近未防护洞口。', 'person_detector_rk3576'],
      ['lift-door-open', '施工升降机楼层防护门未关闭', '发现施工升降机楼层防护门未关闭。', null],
      ['lift-overcrowding', '施工升降机人员超限', '发现施工升降机内人员数量超过限制。', null],
      ['material-hoist-person', '物料提升机违规载人', '发现物料提升机违规搭载人员。', null],
      ['foundation-pit-person', '基坑边缘人员靠近', '发现人员过度靠近基坑边缘。', 'person_detector_rk3576'],
      ['foundation-pit-vehicle', '基坑／临边车辆靠近', '发现车辆过度靠近所画危险边缘。', 'person_detector_rk3576'],
      ['foundation-pit-stacking', '基坑边缘违规堆载', '发现基坑边缘违规堆放重物。', null],
      ['lifting-no-commander', '起重吊装无指挥人员', '发现起重吊装作业现场缺少指挥人员。', null],
      ['distribution-box-open', '配电箱门未关闭', '发现配电箱门未关闭。', null],
      ['temporary-cable-obstruction', '临时电缆拖地或横穿通道', '发现临时电缆拖地或横穿人员通道。', null]
    ],
    fire: [
      ['smoke', '烟雾检测', '发现可见烟雾并触发险情预警。', null],
      ['flame', '明火／火焰检测', '发现可见明火或火焰。', null],
      ['fire-passage-blocked', '消防通道占用检测', '发现消防通道被已支持的车辆或物体持续占用。', 'person_detector_rk3576'],
      ['extinguisher-missing', '灭火器离位检测', '发现灭火器未放置在规定位置。', null],
      ['dangerous-goods-mixed', '危险品与普通货物混放', '发现危险品与普通货物违规混放。', null],
      ['hydrant-cabinet-blocked', '消火栓或配电箱前堆物', '发现消火栓或配电箱前方被物品遮挡。', null],
      ['cargo-near-sprinkler', '货物距离喷淋头过近', '发现货物与消防喷淋头安全距离不足。', null],
      ['fire-shutter-blocked', '消防卷帘门下方堆物', '发现消防卷帘门下方存在影响关闭的物品。', null],
      ['warehouse-charging', '仓库内违规充电', '发现仓库内电动车或电池违规充电。', null],
      ['combustibles-in-charging-area', '充电区域堆放可燃物', '发现充电区域周边堆放可燃物。', null],
      ['hot-work-no-extinguisher', '动火作业未配置灭火器', '发现动火作业现场未配置灭火器。', null],
      ['hot-work-no-guardian', '动火作业无监护人', '发现动火作业现场缺少监护人员。', null],
      ['gas-cylinder-unsecured', '气瓶倒放或未固定', '发现气瓶倒放或未采取可靠固定措施。', null],
      ['oxygen-acetylene-mixed', '氧气瓶与乙炔瓶混放', '发现氧气瓶与乙炔瓶违规混放。', null],
      ['oxygen-acetylene-distance', '氧气瓶与乙炔瓶距离异常', '发现氧气瓶与乙炔瓶安全距离不足。', null]
    ],
    vehicle: [
      ['illegal-parking', '车辆违停检测', '发现车辆在禁停区域停留超过设定时长。', 'person_detector_rk3576'],
      ['vehicle-area-intrusion', '车辆区域入侵', '发现车辆进入指定警戒区域。', 'person_detector_rk3576'],
      ['vehicle-line-crossing', '车辆越线检测', '发现车辆穿越指定边界并区分到场、离场。', 'person_detector_rk3576'],
      ['vehicle-counting', '车辆计数', '统计画面或指定区域内的车辆数量。', 'person_detector_rk3576'],
      ['parking-occupancy', '车位占用与到离场', '识别车位持续占用和持续空置状态。', 'person_detector_rk3576'],
      ['plate-recognition', '车牌识别', '检测车辆号牌并识别车牌号码。', 'hyperlpr3_plate_detector_rk3576'],
      ['plate-detection', '车牌检测', '检测画面中的车牌位置、四角点和单双层类型。', 'hyperlpr3_plate_detector_rk3576'],
      ['e-bike-elevator', '电瓶车进电梯检测', '发现电瓶车进入电梯轿厢。', null],
      ['forklift-speeding', '叉车超速检测', '发现叉车行驶速度超过设定限制。', null],
      ['forklift-carrying-person', '叉车违规载人', '发现叉车违规搭载人员。', null],
      ['forklift-high-fork', '叉车货叉高位行驶', '发现叉车行驶时货叉未降至安全高度。', null],
      ['dock-no-wheel-chock', '车辆靠台未放置止轮器', '发现车辆靠台后未放置止轮器。', null],
      ['dock-door-no-vehicle', '月台门开启但无车辆', '发现月台门开启但对应泊位没有车辆。', null],
      ['parked-forklift-fork-up', '叉车停放货叉未落地', '发现叉车停放后货叉未落地。', null],
      ['forklift-load-obstruction', '叉车货物过高遮挡视线', '发现叉车载货过高并遮挡驾驶视线。', null],
      ['forklift-load-unstable', '叉车载货倾斜不稳', '发现叉车货物明显倾斜或装载不稳。', null]
    ],
    general: [
      ['area-intrusion', '区域入侵检测', '发现人员或目标进入指定警戒区域。', 'person_detector_rk3576'],
      ['line-crossing', '越线检测', '发现人员脚点进入或离开指定警戒区域。', 'person_detector_rk3576'],
      ['people-counting', '人员计数', '统计画面或指定区域内的人员数量。', 'person_detector_rk3576']
    ]
  };

  /**
   * 接入状态推导规则（对齐原始实现的状态判定 m(F)/p(F)）：
   *   已接入 = 已绑定算法包，且该包 production_allowed + formal_event_ready + runtime_ready 全部通过
   *   待就绪 = 已绑定算法包，但链路校验未通过（真机：人脸识别 recognition_ready = false）
   *   待接入 = 未绑定任何算法包
   * 真机 7 个算法包全部通过生产校验，故仅“人脸识别”落在“待就绪”。
   * 结果分布 4/6、7/14、9/49、1/15、7/16、3/3（合计 31）与抓取截图完全一致。
   */
  var NOT_READY = { 'face-recognition': 1 };

  /** 展开为完整能力列表 */
  var CAPABILITIES = [];
  CATEGORIES.forEach(function (cat) {
    (ITEMS[cat.id] || []).forEach(function (row, index) {
      var algo = row[3];
      var meta = algo ? ALGOS[algo] : null;
      var bound = !!meta;
      var state = !bound ? '待接入' : NOT_READY[row[0]] ? '待就绪' : '已接入';
      CAPABILITIES.push({
        id: row[0],
        name: row[1],
        desc: row[2],
        algo: algo,
        bound: bound,
        category: cat.id,
        categoryName: cat.name,
        index: index,
        component: bound ? meta.component : '',
        version: bound ? meta.version : '',
        state: state
      });
    });
  });

  var stateDesc = {
    已接入: '当前设备已具备正式运行条件',
    待就绪: '相关组件已登记，尚未通过全部生产校验',
    待接入: '当前设备尚未安装此项能力'
  };

  function summary() {
    var all = CAPABILITIES.length;
    var ready = CAPABILITIES.filter(function (c) {
      return c.state === '已接入';
    }).length;
    return { all: all, ready: ready, planned: all - ready, categories: CATEGORIES.length };
  }

  /** 监控大屏 mock */
  var MONITOR = {
    date: '2026年10月20日星期二',
    statusLine: '运行中 温44.4℃ CPU8.4% 内存8.8% 存储11%',
    overview: [
      { key: 'alert', icon: 'alert', label: '告警', value: 4, hint: '今日告警', tone: 'danger' },
      { key: 'camera', icon: 'camera', label: '摄像头', value: 1, hint: '当前在线链路', tone: 'info' },
      { key: 'algorithm', icon: 'algorithm', label: '算法', value: 7, hint: '已登记分析能力', tone: 'success' },
      { key: 'model', icon: 'model', label: '模型', value: 7, hint: '已接入资产', tone: 'success' }
    ],
    alerts: { today: 4, yesterday: 0, items: [] },
    layouts: [
      { key: '1', label: '1分屏' },
      { key: '4', label: '4分屏' },
      { key: '6', label: '6分屏' },
      { key: '8', label: '8分屏' },
      { key: '9', label: '9分屏' },
      { key: '16', label: '16分屏' },
      { key: '32', label: '32分屏' },
      { key: 'smart', label: '智能预览' }
    ],
    activeLayout: '9',
    tiles: 9,
    clock: '11:12:54',
    clockHint: 'IP:0'
  };

  /**
   * ===== 真机当前状态快照（不是截图那天的空态）=====
   * 抓取时间：2026-10-20 15:53，来源接口：
   *   /api/cameras、/api/cameras/status、/api/algorithm-tasks、
   *   /api/events、/api/events/stats、/api/system/status
   */
  var DEVICE = {
    deviceName: 'CVAI Vision Box',
    deviceUid: 'VBX-4970075CA4C43966',
    firmwareVersion: '0.1.222',
    appVersion: '0.1.222',
    modelPackVersion: '1.0.0',
    dbSchemaVersion: 28,
    statusCode: 'OK',
    cpuUsage: 8.4,
    memoryUsage: 8.8,
    diskUsage: 11.0,
    temperatureC: 44.4,
    uptimeSec: 16894,
    channels: { configured: 1, mustPass: 2, target: 4, challenge: 8 },
    gateway: {
      name: 'mediamtx',
      running: true,
      ports: { rtsp: true, webrtc: true, hls: true },
      primary: 'webrtc',
      fallback: 'hls'
    }
  };

  /** 时间戳 → HH:mm:ss */
  function clockText(ms) {
    if (!ms) return '--';
    var d = new Date(ms);
    function p(n) {
      return (n < 10 ? '0' : '') + n;
    }
    return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  }

  /** 算法包 ID → 组件显示名 */
  function algoDisplayName(algoId) {
    var meta = ALGOS[algoId];
    return meta ? meta.component : algoId || '未绑定';
  }

  /** 补全摄像头卡片需要的派生展示字段 */
  function buildCamera(raw) {
    var algos = raw.algorithmIds || [];
    return {
      id: raw.id,
      deviceId: raw.deviceId,
      name: raw.name,
      displayName: raw.sourceType === 'manual_rtsp' ? '手动接入' : '自动接入',
      sourceType: raw.sourceType,
      sourceLabel: raw.sourceLabel,
      ip: raw.ip,
      groupName: raw.groupName,
      enabled: raw.enabled,
      status: raw.status,
      statusText: raw.status === 'online' ? '在线' : raw.status === 'offline' ? '离线' : '待确认',
      statusTone: raw.status === 'online' ? 'success' : raw.status === 'offline' ? 'warning' : 'info',
      videoText: raw.previewStatus === 'running' ? '可播放' : '等待视频流',
      aiText: algos.length ? algos.length + ' 个运行中' : '未绑定',
      aiDetailText: algos.map(algoDisplayName).join(' / '),
      lastFrame: clockText(raw.lastFrameTsMs),
      lastEventAt: clockText(raw.lastEventAtMs),
      summary: raw.groupName + ' · ' + algos.length + ' 项分析能力',
      algorithmIds: algos,
      credentialSaved: raw.credentialSaved,
      ruleEnabled: raw.ruleEnabled,
      playback: raw.playback,
      stream: raw.stream,
      tasks: raw.tasks,
      raw: raw
    };
  }

  /** 摄像头：真机 1 台（id 6 · 测试 · 手动 RTSP · 在线） */
  var CAMERAS = [
    buildCamera({
      id: 6,
      deviceId: 'camera-1792481670396',
      name: '测试',
      sourceType: 'manual_rtsp',
      sourceLabel: 'rtsp://192.168.105.64:554',
      ip: '192.168.105.64',
      groupName: '未分组',
      enabled: true,
      status: 'online',
      previewStatus: 'running',
      algorithmIds: ['person_detector_rk3576', 'pose_yolov8s_rk3576'],
      ruleEnabled: true,
      credentialSaved: true,
      lastFrameTsMs: 1792482569944,
      lastEventAtMs: 1792482484563,
      stream: { streamId: 'main', transport: 'tcp', codec: 'h264', resolution: '2560x1440' },
      playback: {
        path: 'camera_6',
        webrtcUrl: 'http://192.168.105.201:8889/camera_6/',
        hlsUrl: 'http://192.168.105.201:8888/camera_6/index.m3u8',
        status: 'ready'
      },
      tasks: [
        { taskId: 1, taskName: 'IPC 1 · 测试 · 算法任务', algorithmId: 'person_detector_rk3576', ruleId: 1, eventType: 'AREA_INTRUSION_RULE_MATCH', runtimeStatus: 'running' },
        { taskId: 2, taskName: 'IPC 1 · 测试 · 算法任务', algorithmId: 'pose_yolov8s_rk3576', ruleId: 2, eventType: 'POSE_RULE_MATCH', runtimeStatus: 'running' }
      ]
    })
  ];

  /** 算法任务：真机 2 个（均 running，绑定摄像头 6） */
  var TASKS = [
    {
      id: 1,
      taskName: 'IPC 1 · 测试 · 算法任务',
      cameraId: 6,
      cameraName: '测试',
      algorithmId: 'person_detector_rk3576',
      algorithmName: '人员检测',
      algorithmVersion: '1.0.0-ppyoloe-s-int8',
      enabled: true,
      runtimeStatus: 'running',
      eventType: 'AREA_INTRUSION_RULE_MATCH',
      ruleId: 1,
      classes: 'person',
      targetFps: 2,
      actualFps: 1.93,
      inferenceMs: 99.67,
      processedFrames: 776,
      detections: 1571
    },
    {
      id: 2,
      taskName: 'IPC 1 · 测试 · 算法任务',
      cameraId: 6,
      cameraName: '测试',
      algorithmId: 'pose_yolov8s_rk3576',
      algorithmName: '姿态检测',
      algorithmVersion: '0.1.2-contract4',
      enabled: true,
      runtimeStatus: 'running',
      eventType: 'POSE_RULE_MATCH',
      ruleId: 2,
      classes: 'sitting',
      targetFps: 2,
      actualFps: 1.92,
      inferenceMs: 99.67,
      processedFrames: 776,
      detections: 1571
    }
  ];

  /** 事件类型 → 中文标签（区域入侵取自原始规则文案；姿态按算子归类） */
  var EVENT_LABELS = {
    AREA_INTRUSION_RULE_MATCH: '区域入侵',
    POSE_RULE_MATCH: '姿态检测'
  };

  /** 告警事件：真机 4 条（今天 4 · 全部未处理） */
  var EVENTS = [
    {
      id: '1792482484563-0000',
      cameraId: 6,
      cameraName: '测试',
      cameraIp: '192.168.105.64',
      taskId: 1,
      ruleId: 1,
      algorithmId: 'person_detector_rk3576',
      algorithmName: '人员检测',
      eventType: 'AREA_INTRUSION_RULE_MATCH',
      eventLabel: '区域入侵',
      className: 'person',
      classLabel: '人员',
      processStatus: 'unhandled',
      statusText: '未处理',
      statusTone: 'warning',
      isImportant: false,
      confidence: '—',
      createdAtLocal: '2026-10-20 15:48:04',
      snapshotUrl: '/api/events/1792482484563-0000/snapshot'
    },
    {
      id: '1792482466975-0000',
      cameraId: 6,
      cameraName: '测试',
      cameraIp: '192.168.105.64',
      taskId: 2,
      ruleId: 2,
      algorithmId: 'pose_yolov8s_rk3576',
      algorithmName: '姿态检测',
      eventType: 'POSE_RULE_MATCH',
      eventLabel: '姿态检测',
      className: 'sitting',
      classLabel: '坐姿',
      processStatus: 'unhandled',
      statusText: '未处理',
      statusTone: 'warning',
      isImportant: false,
      confidence: '—',
      createdAtLocal: '2026-10-20 15:47:46',
      snapshotUrl: '/api/events/1792482466975-0000/snapshot'
    },
    {
      id: '1792482283831-0000',
      cameraId: 6,
      cameraName: '测试',
      cameraIp: '192.168.105.64',
      taskId: 1,
      ruleId: 1,
      algorithmId: 'person_detector_rk3576',
      algorithmName: '人员检测',
      eventType: 'AREA_INTRUSION_RULE_MATCH',
      eventLabel: '区域入侵',
      className: 'person',
      classLabel: '人员',
      processStatus: 'unhandled',
      statusText: '未处理',
      statusTone: 'warning',
      isImportant: false,
      confidence: '—',
      createdAtLocal: '2026-10-20 15:44:43',
      snapshotUrl: '/api/events/1792482283831-0000/snapshot'
    },
    {
      id: '1792482006134-0000',
      cameraId: 6,
      cameraName: '测试',
      cameraIp: '192.168.105.64',
      taskId: 1,
      ruleId: 1,
      algorithmId: 'person_detector_rk3576',
      algorithmName: '人员检测',
      eventType: 'AREA_INTRUSION_RULE_MATCH',
      eventLabel: '区域入侵',
      className: 'person',
      classLabel: '人员',
      processStatus: 'unhandled',
      statusText: '未处理',
      statusTone: 'warning',
      isImportant: false,
      confidence: '—',
      createdAtLocal: '2026-10-20 15:40:06',
      snapshotUrl: '/api/events/1792482006134-0000/snapshot'
    }
  ];

  /** 告警统计（/api/events/stats） */
  var EVENT_STATS = {
    total: 4,
    today: 4,
    yesterday: 0,
    last24h: 4,
    byEventType: { AREA_INTRUSION_RULE_MATCH: 3, POSE_RULE_MATCH: 1 },
    byClassName: { person: 3, sitting: 1 },
    byProcessStatus: { unhandled: 4 }
  };

  /** ONVIF 扫描结果（用于“添加摄像头”弹窗） */
  var DISCOVERED = [
    '192.168.105.16',
    '192.168.105.17',
    '192.168.105.18',
    '192.168.105.20',
    '192.168.105.31',
    '192.168.105.34',
    '192.168.105.35',
    '192.168.105.37'
  ].map(function (ip) {
    return { name: 'ONVIF Device ' + ip, kind: 'ONVIF 摄像头', address: ip, status: '在线' };
  });

  /** 系统状态（真机 /api/system/status 与 /api/system/bootstrap） */
  var SYSTEM = {
    deviceName: 'CVAI Vision Box',
    productVersion: '0.1.222',
    appVersion: '0.1.222',
    imageVersion: '0.1.222',
    algorithmVersion: '1.0.0',
    uptime: '4小时42分钟',
    host: '本机',
    strip: [
      { label: '系统状态', value: '运行中', tone: 'success' },
      { label: '视频通道', value: '部分可用', tone: 'warning' },
      { label: '网络连接', value: '网络正常', tone: 'success' },
      { label: 'CPU', value: '8.4%' },
      { label: '内存', value: '8.8%' },
      { label: '存储', value: '11%' }
    ],
    cards: [
      { icon: 'health', title: '核心服务', value: '运行中', tone: 'success', note: '设备管理和基础功能正在运行' },
      { icon: 'camera', title: '视频接入', value: '运行中', tone: 'success', note: '已接入 1 路视频通道，预览正在运行' },
      { icon: 'online', title: '网络连接', value: '网络正常', tone: 'success', note: '设备可通过当前网络访问' },
      { icon: 'database', title: '存储空间', value: '11%', tone: 'success', note: '剩余空间充足' }
    ]
  };

  global.MockData = {
    NAV: NAV,
    CATEGORIES: CATEGORIES,
    CAPABILITIES: CAPABILITIES,
    SUMMARY: summary(),
    STATE_DESC: stateDesc,
    MONITOR: MONITOR,
    CAMERAS: CAMERAS,
    TASKS: TASKS,
    EVENTS: EVENTS,
    EVENT_STATS: EVENT_STATS,
    EVENT_LABELS: EVENT_LABELS,
    DEVICE: DEVICE,
    clockText: clockText,
    algoDisplayName: algoDisplayName,
    DISCOVERED: DISCOVERED,
    SYSTEM: SYSTEM,
    NOTIFICATION_CHANNELS: [],
    NOTIFICATION_ROUTES: [],
    NOTIFICATION_DELIVERIES: [],
    AI_REVIEW_TASKS: [],
    BASE_LIBRARY: { person: [], vehicle: [], workwear: [], ppe: [] },
    HARDWARE: { endpoints: [], rules: [], actions: [] }
  };
})(window);
