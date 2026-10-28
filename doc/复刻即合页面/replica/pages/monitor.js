/**
 * 监控大屏（路由 /），对应原始 MonitorHomeView。
 * 独立全屏布局：monitor-shell-topbar + monitor-shell-grid（左目录 / 中视频墙 / 右告警）。
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;

  function summaryCard(item) {
    return (
      '<div class="monitor-summary-card" data-tone="' +
      esc(item.tone) +
      '">' +
      '<div class="monitor-summary-card__icon">' +
      icon(item.icon, 16) +
      '</div>' +
      '<div class="monitor-summary-card__label">' +
      esc(item.label) +
      '</div>' +
      '<div class="monitor-summary-card__value">' +
      esc(item.value) +
      '</div>' +
      '<div class="monitor-summary-card__hint">' +
      esc(item.hint) +
      '</div>' +
      '</div>'
    );
  }

  function layoutTabs() {
    var m = MockData.MONITOR;
    return (
      '<div class="monitor-wall-toolbar">' +
      '<span class="monitor-layout-tabs__mark">' +
      icon('rule', 15) +
      '</span>' +
      '<div class="monitor-layout-tabs">' +
      m.layouts
        .filter(function (item) {
          return item.key !== 'smart';
        })
        .map(function (item) {
          return (
            '<button type="button" class="monitor-layout-tabs__item' +
            (item.key === m.activeLayout ? ' is-active' : '') +
            '" data-layout="' +
            esc(item.key) +
            '">' +
            esc(item.label) +
            '</button>'
          );
        })
        .join('') +
      '</div>' +
      '<button type="button" class="monitor-chip monitor-chip--strong">智能预览</button>' +
      '</div>'
    );
  }

  function tile(index) {
    return (
      '<article class="monitor-video-tile is-waiting">' +
      '<div class="monitor-wall-tile__bar">' +
      '<span>视频' +
      index +
      '</span>' +
      '<span>等待视频源</span>' +
      '</div>' +
      '<div class="monitor-video-tile__media">' +
      '<div class="monitor-video-tile__placeholder is-overlay">' +
      '<span class="monitor-video-tile__icon">' +
      icon('camera', 18) +
      '</span>' +
      '<strong>摄像头</strong>' +
      '<p>未接入实时视频源</p>' +
      '<p>请先在管理后台接入可用通道</p>' +
      '</div>' +
      '</div>' +
      '</article>'
    );
  }

  /** 已接入通道的画面格（真机 1 路：测试 · 可播放） */
  function wiredTile(cam, index) {
    return (
      '<article class="monitor-video-tile is-online" data-camera="' +
      cam.id +
      '">' +
      '<div class="monitor-wall-tile__bar">' +
      '<span>视频' +
      index +
      '</span>' +
      '<span>' +
      esc(cam.videoText) +
      '</span>' +
      '</div>' +
      '<div class="monitor-video-tile__media">' +
      '<div class="monitor-video-tile__placeholder is-overlay">' +
      '<span class="monitor-video-tile__icon">' +
      icon('camera', 18) +
      '</span>' +
      '<strong>' +
      esc(cam.name) +
      '</strong>' +
      '<p>' +
      esc(cam.sourceLabel) +
      '</p>' +
      '<p>' +
      esc(cam.stream.resolution + ' · ' + cam.stream.codec + ' · ' + cam.videoText) +
      '</p>' +
      '</div>' +
      '</div>' +
      '</article>'
    );
  }

  function render() {
    var m = MockData.MONITOR;
    var cameras = MockData.CAMERAS;
    var tiles = '';
    for (var i = 1; i <= m.tiles; i += 1) {
      tiles += cameras[i - 1] ? wiredTile(cameras[i - 1], i) : tile(i);
    }
    var deviceTree = cameras.length
      ? cameras
          .map(function (cam) {
            return (
              '<div class="monitor-device-group">' +
              '<div class="monitor-device-group__title">' +
              esc(cam.groupName) +
              '</div>' +
              '<div class="monitor-device-item">' +
              '<span class="monitor-device-item__main"><strong>' +
              esc(cam.name) +
              '</strong><span class="muted">' +
              esc(cam.ip + ' · ' + cam.displayName) +
              '</span></span>' +
              '<span class="monitor-device-item__status is-online">' +
              icon('camera', 16) +
              '</span>' +
              '</div>' +
              '</div>'
            );
          })
          .join('')
      : '<div class="monitor-device-group">' +
        '<div class="monitor-device-group__title">设备目录</div>' +
        '<div class="monitor-device-empty">暂无设备</div>' +
        '</div>';

    var alertItems = MockData.EVENTS.length
      ? '<div class="monitor-alert-timeline">' +
        MockData.EVENTS.map(function (evt) {
          return (
            '<div class="monitor-alert-timeline__item">' +
            '<span class="monitor-alert-timeline__dot" data-tone="' +
            esc(evt.statusTone) +
            '"></span>' +
            '<div><strong>' +
            esc(evt.eventLabel) +
            '</strong><span class="muted">' +
            esc(evt.cameraName + ' · ' + evt.classLabel + ' · ' + evt.createdAtLocal.slice(11)) +
            '</span></div>' +
            '<span class="monitor-alert-timeline__tag">' +
            esc(evt.statusText) +
            '</span>' +
            '</div>'
          );
        }).join('') +
        '</div>'
      : '<div class="monitor-alert-rail__empty">' +
        '<strong>当前没有正式告警</strong>' +
        '<p class="muted"><code>REST /events</code>：首页会优先显示 REST <code>/events</code> 返回的最近正式事件。</p>' +
        '</div>';
    return (
      '<div class="monitor-home-v3">' +
      '<header class="monitor-shell-topbar">' +
      '<div class="monitor-shell-topbar__date">' +
      '<span class="monitor-shell-topbar__day">' +
      esc(m.date) +
      '</span>' +
      '<span class="monitor-shell-topbar__status">' +
      esc(m.statusLine) +
      '</span>' +
      '</div>' +
      '<div class="monitor-shell-topbar__title">即合视觉 AI</div>' +
      '<div class="monitor-shell-topbar__actions">' +
      '<a class="monitor-admin-button" href="#/admin/cameras">管理后台</a>' +
      '</div>' +
      '</header>' +
      '<div class="monitor-shell-grid">' +
      /* 左侧：全局总览 + 设备目录 */
      '<aside class="monitor-sidebar monitor-sidebar--left">' +
      '<section class="monitor-card">' +
      '<div class="monitor-card__header"><strong>全局总览</strong></div>' +
      '<div class="monitor-summary-grid">' +
      m.overview.map(summaryCard).join('') +
      '</div>' +
      '</section>' +
      '<section class="monitor-card monitor-card--catalog">' +
      '<div class="monitor-card__header"><strong>设备目录</strong>' +
      '<a class="monitor-link-button" href="#/admin/cameras">设备管理</a></div>' +
      '<label class="monitor-search">' +
      icon('search', 15) +
      '<input type="search" placeholder="搜索设备 / IPC" />' +
      '</label>' +
      '<div class="monitor-device-tree">' +
      deviceTree +
      '</div>' +
      '</section>' +
      '</aside>' +
      /* 中间：视频墙 */
      '<main class="monitor-main">' +
      '<section class="monitor-card monitor-card--wall">' +
      '<div class="monitor-card__header monitor-card__header--wall">' +
      '<div class="monitor-card__meta"><strong>实时监控</strong>' +
      '<span class="monitor-clock">' +
      esc(m.clock) +
      '</span><span class="muted">' +
      esc(m.clockHint) +
      '</span></div>' +
      layoutTabs() +
      '</div>' +
      '<div class="monitor-video-wall is-layout-9">' +
      tiles +
      '</div>' +
      '</section>' +
      '</main>' +
      /* 右侧：告警时间线 */
      '<aside class="monitor-sidebar monitor-sidebar--right">' +
      '<section class="monitor-card monitor-card--alerts">' +
      '<div class="monitor-card__header"><div><strong>告警事件</strong>' +
      '<span class="muted">近实时告警时间线</span></div>' +
      '<span class="monitor-status-dot" data-tone="success"></span></div>' +
      '<div class="monitor-alert-stats">' +
      '<div class="monitor-summary-card">' +
      '<div class="monitor-summary-card__icon">' +
      icon('alert', 16) +
      '</div>' +
      '<div class="monitor-summary-card__label">今日告警</div>' +
      '<div class="monitor-summary-card__value">' +
      esc(m.alerts.today) +
      '</div>' +
      '</div>' +
      '<div class="monitor-summary-card">' +
      '<div class="monitor-summary-card__icon">' +
      icon('time', 16) +
      '</div>' +
      '<div class="monitor-summary-card__label">昨日告警</div>' +
      '<div class="monitor-summary-card__value">' +
      esc(m.alerts.yesterday) +
      '</div>' +
      '</div>' +
      '</div>' +
      alertItems +
      '</section>' +
      '</aside>' +
      '</div>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      var btn = evt.target.closest('[data-layout]');
      if (!btn) return;
      root.querySelectorAll('[data-layout]').forEach(function (el) {
        el.classList.toggle('is-active', el === btn);
      });
      var key = btn.getAttribute('data-layout');
      var wall = root.querySelector('.monitor-video-wall');
      if (wall) wall.className = 'monitor-video-wall is-layout-' + key;
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/'] = { path: '/', full: true, render: render, mount: mount };
})(window);
