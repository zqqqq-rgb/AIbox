/**
 * 告警事件（/admin/events），对应原始 EventsView。
 * 有事件时渲染 admin-event-table（8 列），顶栏提供“告警设置 / 样本回流”入口。
 * 注：事件缩略图来自 /api/events/{id}/snapshot，需登录鉴权，静态复刻页无法直接取图，故显示“暂无画面”。
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  function topActions() {
    return (
      '<button type="button" class="btn-secondary" data-action="event-settings">' +
      icon('system', 15) +
      '<span>告警设置</span></button>' +
      '<a class="btn-secondary" href="#/admin/events/sample-uploads">' +
      icon('upload', 15) +
      '<span>样本回流</span></a>'
    );
  }

  function queryBar() {
    return (
      '<div class="panel admin-section admin-event-query-panel">' +
      '<div class="admin-query-bar admin-query-bar--events">' +
      '<div class="field"><label>' +
      icon('image', 14) +
      ' 告警对象 </label>' +
      '<select><option>全部对象</option><option>人员（3） </option>' +
      '<option>坐姿（1） </option></select></div>' +
      '<div class="field"><label>' +
      icon('rule', 14) +
      ' 告警事件 </label>' +
      '<select><option>全部事件</option><option>区域入侵</option>' +
      '<option>姿态检测</option></select></div>' +
      '<div class="field admin-event-time-field"><label>' +
      icon('time', 14) +
      ' 告警时间 </label>' +
      '<div class="admin-event-time-range">' +
      '<input type="datetime-local" step="60" aria-label="告警开始时间" />' +
      '<span>至</span>' +
      '<input type="datetime-local" step="60" aria-label="告警结束时间" />' +
      '</div></div>' +
      '<div class="field"><label>' +
      icon('filter', 14) +
      ' 处理状态 </label>' +
      '<select><option>全部状态</option><option>未处理</option><option>处理中</option>' +
      '<option>已处理</option><option>已忽略</option></select></div>' +
      '<div class="admin-query-bar__actions">' +
      '<button type="button" class="btn-secondary">' +
      icon('offline', 16) +
      ' 重置 </button>' +
      '</div>' +
      '</div>' +
      '</div>'
    );
  }

  function eventRow(evt) {
    return (
      '<article class="admin-event-table__row" data-event="' +
      esc(evt.id) +
      '">' +
      '<button type="button" class="admin-event-table__thumb">' +
      '<span class="muted">暂无画面</span>' +
      '</button>' +
      '<strong>' +
      esc(evt.eventLabel) +
      '</strong>' +
      '<span>' +
      esc(evt.classLabel) +
      '</span>' +
      '<div>' +
      '<strong title="' +
      esc(evt.cameraName + ' · ' + evt.cameraIp) +
      '">' +
      esc(evt.cameraName) +
      '</strong>' +
      '<span title="' +
      esc(evt.algorithmName + ' · 任务 ' + evt.taskId) +
      '">' +
      esc(evt.algorithmName) +
      ' · ' +
      esc('任务 ' + evt.taskId) +
      '</span>' +
      (evt.isImportant ? '<em>重要告警</em>' : '') +
      '</div>' +
      '<span>' +
      esc(evt.createdAtLocal) +
      '</span>' +
      '<span>' +
      esc(evt.confidence) +
      '</span>' +
      '<span class="admin-tone-chip" data-tone="' +
      esc(evt.statusTone) +
      '">' +
      esc(evt.statusText) +
      '</span>' +
      '<div>' +
      '<button type="button" class="admin-icon-button" title="查看详情" data-action="event-detail">' +
      icon('eye', 15) +
      '</button>' +
      '<button type="button" class="admin-icon-button" title="确认处理" data-action="event-resolve">' +
      icon('online', 15) +
      '</button>' +
      '<button type="button" class="admin-icon-button" title="忽略" data-action="event-ignore">' +
      icon('offline', 15) +
      '</button>' +
      '</div>' +
      '</article>'
    );
  }

  function render() {
    var events = MockData.EVENTS;
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      queryBar() +
      '<div class="panel admin-section admin-event-gallery-panel">' +
      '<div class="admin-section__head admin-section__head--event-list">' +
      '<div class="admin-section__title-line"><strong class="admin-section__title">截图列表</strong>' +
      '<span class="admin-section__subline muted">共 ' +
      events.length +
      ' 条</span></div>' +
      '<div class="admin-event-list-actions">' +
      '<button type="button" class="btn-secondary" data-action="refresh-events">' +
      icon('export', 15) +
      '<span>刷新</span></button>' +
      '<button type="button" class="btn-primary" data-action="export-events">' +
      icon('upload', 15) +
      '<span>导出告警</span></button>' +
      '</div>' +
      '</div>' +
      (events.length
        ? '<div class="admin-event-table admin-scroll-region">' +
          '<div class="admin-event-table__head">' +
          '<span>截图</span><span>告警事件</span><span>告警对象</span><span>来源信息</span>' +
          '<span>告警时间</span><span>置信度</span><span>处理状态</span><span>操作</span>' +
          '</div>' +
          '<div class="admin-event-table__body">' +
          events.map(eventRow).join('') +
          '</div>' +
          '</div>' +
          '<div class="event-pagination">' +
          '<div class="event-pagination__summary"><strong>1-' +
          events.length +
          '</strong><span>共 ' +
          events.length +
          ' 条</span></div>' +
          '<div class="event-pagination__controls">' +
          '<button type="button" class="admin-icon-button" disabled>' +
          icon('chevron-left', 15) +
          '</button>' +
          '<button type="button" class="admin-icon-button" disabled>' +
          icon('chevron-right', 15) +
          '</button>' +
          '</div>' +
          '<label class="event-pagination__size muted">每页条数' +
          '<select><option>20</option><option>50</option><option>100</option></select></label>' +
          '</div>'
        : UI.emptyState('暂无符合条件的告警', '调整筛选条件后，新的事件会在这里显示。')) +
      '</div>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      if (evt.target.closest('[data-action="refresh-events"]')) {
        global.App.notify('success', '告警事件已刷新');
        return;
      }
      if (evt.target.closest('[data-action="export-events"]')) {
        global.App.notify('info', '导出告警需要设备接口支持，静态复刻页仅展示入口。');
        return;
      }
      if (evt.target.closest('[data-action="event-settings"]')) {
        global.App.notify('info', '告警设置：保留策略、磁盘保护与新任务默认值。');
        return;
      }
      if (evt.target.closest('.admin-event-table__row')) {
        global.App.notify('info', '事件详情与状态更新需要设备接口支持，静态复刻页仅展示列表。');
      }
    });
  }

  /** 样本回流子页面（/admin/events/sample-uploads） */
  function renderSampleUploads() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      '<div class="panel admin-section">' +
      '<div class="sample-return-hero__copy">' +
      '<strong class="admin-section__title">样本回流</strong>' +
      '<p class="muted">把误报、漏报和低置信度样本回传云端，用于模型改进。</p>' +
      '</div>' +
      '<div class="sample-return-stats">' +
      '<div class="stats-grid stats-grid--compact">' +
      '<div class="stat-card"><span class="stat-label">回流状态筛选</span>' +
      '<span class="stat-value">0</span></div>' +
      '</div>' +
      '</div>' +
      '<div class="sample-return-filters">' +
      '<button type="button" class="btn-secondary">全部</button>' +
      '<button type="button" class="btn-secondary">待发送</button>' +
      '<button type="button" class="btn-secondary">已送达</button>' +
      '</div>' +
      UI.emptyState('暂无样本回流记录', '在告警事件详情中可将误报或漏报样本加入上传队列。') +
      '</div>' +
      '</div>'
    );
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/events'] = {
    path: '/admin/events',
    render: render,
    mount: mount,
    topActions: topActions
  };
  global.Pages['/admin/events/sample-uploads'] = {
    path: '/admin/events/sample-uploads',
    render: renderSampleUploads,
    mount: function () {}
  };
})(window);
