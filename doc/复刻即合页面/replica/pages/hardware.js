/**
 * 硬件联动（/admin/hardware-linkage），对应原始 HardwareLinkageView。
 * 真实结构（取自渲染树）：
 *   div.admin-page
 *     section.admin-page__header.admin-page__header--slim（原版被 CSS 隐藏）
 *     section.notification-summary[aria-label="硬件联动概览"] > div(span 标签 + strong 数值) ×4
 *     div.notification-toolbar
 *       div.admin-tab-switch > button[is-active] 硬件端点 / 联动规则 / 执行记录
 *       button.btn-primary 新增端点 / 新增规则 ｜ button.btn-secondary 全部解除
 *     div.error-banner(key0)
 *     div.admin-empty-state 或 div.notification-table-wrap > div.notification-table
 */
(function (global) {
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  var TABS = [
    { key: 'endpoints', label: '硬件端点', action: '新增端点', columns: ['端点', '类型', '地址', '实时状态', '启用', '操作'], empty: '尚未配置硬件端点' },
    { key: 'rules', label: '联动规则', action: '新增规则', columns: ['规则', '输出端点', '摄像头', '事件类型', '执行策略', '操作'], empty: '尚未配置联动规则' },
    { key: 'actions', label: '执行记录', action: '全部解除', columns: ['事件', '联动端点', '状态', '尝试', '发生时间', '结果', '操作'], empty: '暂无硬件联动执行记录' }
  ];

  var activeTab = 'endpoints';

  function tabOf(key) {
    return TABS.filter(function (t) {
      return t.key === key;
    })[0];
  }

  function summary() {
    var rows = [
      ['启用端点', 0, false],
      ['启用规则', 0, false],
      ['活动输出', 0, false],
      ['异常动作', 0, true]
    ];
    return (
      '<section class="notification-summary" aria-label="硬件联动概览">' +
      rows
        .map(function (row) {
          return (
            '<div><span>' +
            esc(row[0]) +
            '</span><strong' +
            (row[2] ? ' class="danger"' : '') +
            '>' +
            row[1] +
            '</strong></div>'
          );
        })
        .join('') +
      '</section>'
    );
  }

  function toolbar() {
    var tab = tabOf(activeTab);
    var actionBtn =
      activeTab === 'actions'
        ? '<button type="button" class="btn-secondary" data-action="clear-outputs" disabled>全部解除</button>'
        : '<button type="button" class="btn-primary" data-action="new-hardware" disabled>' +
          icon('plus', 16) +
          ' ' +
          esc(tab.action) +
          '</button>';
    return (
      '<div class="notification-toolbar">' +
      '<div class="admin-tab-switch" role="tablist">' +
      TABS.map(function (item) {
        return (
          '<button type="button" class="' +
          (item.key === activeTab ? 'is-active' : '') +
          '" data-hardware-tab="' +
          esc(item.key) +
          '">' +
          esc(item.label) +
          '</button>'
        );
      }).join('') +
      '</div>' +
      actionBtn +
      '</div>'
    );
  }

  function tabBody() {
    var tab = tabOf(activeTab);
    return (
      UI.emptyState(tab.empty, null, '') +
      '<div class="notification-table-wrap"><div class="notification-table"></div></div>'
    );
  }

  function render() {
    return (
      '<div class="admin-page">' +
      '<section class="admin-page__header admin-page__header--slim">' +
      '<div><h1 class="admin-page__single-title">硬件联动</h1>' +
      '<p class="muted">管理 GPIO、RS485 与网络语音播报联动</p></div>' +
      '<button type="button" class="btn-secondary" data-action="refresh-hardware" title="刷新">' +
      icon('online', 17) +
      ' 刷新</button>' +
      '</section>' +
      summary() +
      toolbar() +
      '<div data-slot="hardware-body">' +
      tabBody() +
      '</div>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      var tabBtn = evt.target.closest('[data-hardware-tab]');
      if (tabBtn) {
        activeTab = tabBtn.getAttribute('data-hardware-tab');
        root.querySelectorAll('[data-hardware-tab]').forEach(function (el) {
          el.classList.toggle('is-active', el === tabBtn);
        });
        var toolbarEl = root.querySelector('.notification-toolbar');
        var temp = document.createElement('div');
        temp.innerHTML = toolbar();
        toolbarEl.replaceWith(temp.firstElementChild);
        root.querySelector('[data-slot="hardware-body"]').innerHTML = tabBody();
        return;
      }
      if (evt.target.closest('[data-action="refresh-hardware"]')) {
        global.App.notify('success', '硬件状态已刷新');
        return;
      }
      if (evt.target.closest('[data-action="clear-outputs"]')) {
        if (global.confirm('确认解除当前全部硬件输出吗？')) {
          global.App.notify('success', '已请求解除全部输出');
        }
        return;
      }
      if (evt.target.closest('[data-action="new-hardware"]')) {
        global.App.notify('info', '新增端点/规则需要设备接口支持，静态复刻页仅展示入口。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/hardware-linkage'] = {
    path: '/admin/hardware-linkage',
    render: render,
    mount: mount
  };
})(window);
