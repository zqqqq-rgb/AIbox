/**
 * 系统设置（/admin/system），对应原始 SystemView。
 * DOM 结构照搬原渲染树：
 *   div.admin-workbench.admin-workbench--single-title
 *     section.panel.admin-section.admin-system-main-panel
 *       div.admin-system-shell > div.admin-system-content（三行：状态条 / 分区菜单 / 分区内容）
 *         div.admin-system-overview-strip
 *           article.admin-system-overview-strip__item.admin-system-overview-strip__item--section（当前页面 + 当前分区）
 *           article.admin-system-overview-strip__item ×6（label + value）
 *         div.admin-system-subnav[role=tablist] > span.admin-system-subnav__label 系统菜单 + button.admin-system-subnav__item(icon+strong) ×10
 *         div.admin-system-section > 各分区内容（system-product-page）
 */
(function (global) {
  var MockData = global.MockData;
  var SYSTEM = MockData.SYSTEM;
  var DEVICE = MockData.DEVICE;
  var icon = global.icon;
  var esc = global.UI.esc;

  /** 10 个分区：key / label / description / status / icon 均取自原始定义 */
  var TABS = [
    { key: 'overview', label: '系统概览', desc: '设备运行情况一览', status: '已接入', icon: 'home' },
    { key: 'services', label: '服务状态', desc: '查看各项服务是否正常', status: '已接入', icon: 'health' },
    { key: 'resources', label: '资源监控', desc: '查看设备资源使用情况', status: '已接入', icon: 'threshold' },
    { key: 'logs', label: '日志', desc: '查看操作与系统记录', status: '已接入', icon: 'log' },
    { key: 'storage', label: '存储与快照', desc: '查看存储空间和事件快照', status: '部分接入', icon: 'database' },
    { key: 'network', label: '网络与时间', desc: '设备访问地址、连接状态与时间基准', status: '已接入', icon: 'gateway' },
    { key: 'account', label: '账号与安全', desc: '管理员账号与登录密码', status: '已接入', icon: 'rule' },
    { key: 'ota', label: 'OTA升级', desc: '安全获取并安装系统更新', status: '已接入', icon: 'system' },
    { key: 'maintenance', label: '维护工具', desc: '状态刷新、诊断与服务重启', status: '已接入', icon: 'rule' },
    { key: 'help', label: '使用帮助', desc: '常用操作与问题说明', status: '基础内容', icon: 'log' }
  ];

  /** 状态条：第一格为“当前页面 + 当前分区”，其余为设备真实指标 */
  var STRIP_ITEMS = [
    { label: '系统状态', value: '运行中' },
    { label: '视频通道', value: '部分可用' },
    { label: '网络连接', value: '网络正常' },
    { label: 'CPU', value: '8.4%' },
    { label: '内存', value: '8.8%' },
    { label: '存储', value: '11%' }
  ];

  /** 服务状态分区：真机 2 个算法任务运行中、1 路视频接入、4 条告警已入库 */
  var SERVICES = [
    { icon: 'health', name: '核心服务', note: '负责设备管理和各项基础功能', value: '运行中', tone: 'success' },
    { icon: 'camera', name: '视频接入', note: '负责连接摄像头并提供实时画面', value: '运行中', tone: 'success' },
    { icon: 'algorithm', name: '智能分析', note: '负责执行已启用的智能分析任务', value: '运行中', tone: 'success' },
    { icon: 'alert', name: '告警服务', note: '负责保存告警并向管理页面推送消息', value: '运行中', tone: 'success' },
    { icon: 'database', name: '存储服务', note: '负责保存截图、样本与系统日志', value: '运行中', tone: 'success' }
  ];

  /** 资源监控分区 */
  var RESOURCES = [
    { icon: 'system', label: 'CPU', value: '8.4%', percent: 8, note: '8 核 ARM · 当前负载正常' },
    { icon: 'threshold', label: '内存', value: '8.8%', percent: 9, note: 'vision_box_service 占用约 380MB' },
    { icon: 'database', label: '存储', value: '11%', percent: 11, note: '剩余空间充足' },
    { icon: 'time', label: '温度', value: '44.4℃', percent: 44, note: '芯片温度处于正常区间' }
  ];

  function tabOf(key) {
    return TABS.filter(function (t) {
      return t.key === key;
    })[0] || TABS[0];
  }

  function strip(activeKey) {
    var current = tabOf(activeKey);
    return (
      '<div class="admin-system-overview-strip">' +
      '<article class="admin-system-overview-strip__item admin-system-overview-strip__item--section">' +
      '<span>当前页面</span><strong>' +
      esc(current.label) +
      '</strong></article>' +
      STRIP_ITEMS.map(function (item) {
        return (
          '<article class="admin-system-overview-strip__item">' +
          '<span>' +
          esc(item.label) +
          '</span><strong>' +
          esc(item.value) +
          '</strong></article>'
        );
      }).join('') +
      '</div>'
    );
  }

  function subnav(activeKey) {
    return (
      '<div class="admin-system-subnav" role="tablist" aria-label="系统设置分区">' +
      '<span class="admin-system-subnav__label">系统菜单</span>' +
      TABS.map(function (tab) {
        return (
          '<button type="button" class="admin-system-subnav__item' +
          (tab.key === activeKey ? ' is-active' : '') +
          '" role="tab" data-system-tab="' +
          esc(tab.key) +
          '">' +
          icon(tab.icon, 15) +
          '<strong>' +
          esc(tab.label) +
          '</strong></button>'
        );
      }).join('') +
      '</div>'
    );
  }

  /** 压缩版分区头（服务状态 / 资源监控 / 其余分区共用） */
  function compactHero(tab) {
    return (
      '<section class="panel system-product-hero system-product-hero--compact">' +
      '<div class="system-product-hero__mark">' +
      icon(tab.icon, 26) +
      '</div>' +
      '<div class="system-product-hero__content">' +
      '<span class="system-product-kicker">' +
      esc(tab.label) +
      '</span>' +
      '<h2>' +
      esc(tab.desc) +
      '</h2>' +
      '</div>' +
      '</section>'
    );
  }

  /** 系统概览分区 */
  function overviewPage() {
    return (
      '<div class="system-product-page">' +
      '<section class="panel system-product-hero">' +
      '<div class="system-product-hero__mark">' +
      icon('system', 29) +
      '</div>' +
      '<div class="system-product-hero__content">' +
      '<span class="system-product-kicker">设备运行概览</span>' +
      '<h2>' +
      esc(SYSTEM.deviceName) +
      '</h2>' +
      '<p>设备状态、连接和存储情况集中在这里，异常项目会明确提醒。</p>' +
      '<div class="system-product-hero__meta">' +
      '<span>' +
      icon('model', 14) +
      '版本 ' +
      esc(SYSTEM.appVersion) +
      '</span>' +
      '<span>' +
      icon('time', 14) +
      '已运行 ' +
      esc(SYSTEM.uptime) +
      '</span>' +
      '</div>' +
      '</div>' +
      '<span class="system-state-badge" data-tone="good">运行中</span>' +
      '</section>' +
      '<div class="system-status-grid">' +
      SYSTEM.cards
        .map(function (card) {
          return (
            '<div class="panel system-status-card" data-tone="' +
            esc(card.tone === 'success' ? 'good' : 'warning') +
            '">' +
            '<span class="system-status-card__icon">' +
            icon(card.icon, 20) +
            '</span>' +
            '<div><strong>' +
            esc(card.title) +
            '</strong>' +
            '<p>' +
            esc(card.note) +
            '</p></div>' +
            '</div>'
          );
        })
        .join('') +
      '</div>' +
      '<section class="panel system-info-card">' +
      '<div class="system-card-heading"><h2>版本与运行时间</h2>' +
      '<span class="system-state-badge" data-tone="good">本机</span></div>' +
      '<div class="system-info-grid">' +
      '<div><span>应用版本</span><strong>' +
      esc(SYSTEM.appVersion) +
      '</strong></div>' +
      '<div><span>固件版本</span><strong>' +
      esc(SYSTEM.imageVersion) +
      '</strong></div>' +
      '<div><span>算法包版本</span><strong>' +
      esc(SYSTEM.algorithmVersion) +
      '</strong></div>' +
      '<div><span>连续运行</span><strong>' +
      esc(SYSTEM.uptime) +
      '</strong></div>' +
      '</div>' +
      '</section>' +
      '</div>'
    );
  }

  /** 服务状态分区 */
  function servicesPage() {
    var tab = tabOf('services');
    return (
      '<div class="system-product-page">' +
      compactHero(tab) +
      '<div class="system-service-list">' +
      SERVICES.map(function (row) {
        return (
          '<div class="panel system-service-row" data-tone="' +
          esc(row.tone === 'success' ? 'good' : 'warning') +
          '">' +
          '<span class="system-service-row__icon">' +
          icon(row.icon, 20) +
          '</span>' +
          '<div class="system-service-row__body"><strong>' +
          esc(row.name) +
          '</strong>' +
          '<p>' +
          esc(row.note) +
          '</p></div>' +
          '<span class="system-state-badge" data-tone="' +
          esc(row.tone === 'success' ? 'good' : 'warning') +
          '">' +
          esc(row.value) +
          '</span>' +
          '</div>'
        );
      }).join('') +
      '</div>' +
      '</div>'
    );
  }

  /** 资源监控分区 */
  function resourcesPage() {
    var tab = tabOf('resources');
    return (
      '<div class="system-product-page">' +
      compactHero(tab) +
      '<span class="system-state-badge" data-tone="good">设备资源正常</span>' +
      '<div class="system-resource-grid">' +
      RESOURCES.map(function (item) {
        return (
          '<div class="panel system-resource-card">' +
          '<div class="system-resource-card__head">' +
          '<span class="system-status-card__icon">' +
          icon(item.icon, 18) +
          '</span>' +
          '<span>' +
          esc(item.label) +
          '</span>' +
          '</div>' +
          '<strong>' +
          esc(item.value) +
          '</strong>' +
          '<div class="system-progress"><span style="width:' +
          item.percent +
          '%"></span></div>' +
          '<p>' +
          esc(item.note) +
          '</p>' +
          '</div>'
        );
      }).join('') +
      '</div>' +
      '</div>'
    );
  }

  /** 其余分区：压缩版分区头 + 说明卡（结构同样取自原渲染树） */
  function placeholderPage(key) {
    var tab = tabOf(key);
    return (
      '<div class="system-product-page">' +
      compactHero(tab) +
      '<div class="panel system-note-card">' +
      '<span class="system-note-card__icon">' +
      icon(tab.icon, 20) +
      '</span>' +
      '<div><strong>' +
      esc(tab.label) +
      '</strong>' +
      '<p>' +
      esc(tab.desc) +
      '（该分区内容较多，静态复刻页仅还原结构入口，数据由设备接口下发的字段驱动。）</p></div>' +
      '</div>' +
      '</div>'
    );
  }

  function body(key) {
    if (key === 'overview') return overviewPage();
    if (key === 'services') return servicesPage();
    if (key === 'resources') return resourcesPage();
    return placeholderPage(key);
  }

  var activeKey = 'overview';

  function render() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      '<section class="admin-page__header admin-page__header--slim">' +
      '<div><h1 class="admin-page__single-title">系统设置</h1></div>' +
      '</section>' +
      '<section class="panel admin-section admin-system-main-panel">' +
      '<div class="admin-system-shell">' +
      '<div class="admin-system-content">' +
      strip(activeKey) +
      subnav(activeKey) +
      '<div class="admin-system-section" data-slot="system-section">' +
      body(activeKey) +
      '</div>' +
      '</div>' +
      '</div>' +
      '</section>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      var tabBtn = evt.target.closest('[data-system-tab]');
      if (!tabBtn) return;
      activeKey = tabBtn.getAttribute('data-system-tab');
      root.querySelectorAll('[data-system-tab]').forEach(function (el) {
        el.classList.toggle('is-active', el === tabBtn);
      });
      var stripHost = root.querySelector('.admin-system-overview-strip');
      var temp = document.createElement('div');
      temp.innerHTML = strip(activeKey);
      stripHost.replaceWith(temp.firstElementChild);
      root.querySelector('[data-slot="system-section"]').innerHTML = body(activeKey);
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/system'] = { path: '/admin/system', render: render, mount: mount };
})(window);
