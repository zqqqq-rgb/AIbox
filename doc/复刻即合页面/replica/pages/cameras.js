/**
 * 摄像头管理（/admin/cameras），对应原始 CamerasView。
 * 有设备时渲染 admin-camera-proto-card 卡片网格（结构照搬原渲染树）；
 * 含“添加摄像头”弹窗（ONVIF 扫描 / 手动 RTSP 两个页签）。
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  var PAGE_SIZE = 8;

  function queryBar() {
    return (
      '<div class="panel admin-section admin-query-panel">' +
      '<div class="admin-query-bar admin-query-bar--camera-table">' +
      '<div class="field"><label>' +
      icon('filter', 14) +
      ' 摄像头名称 / IP </label>' +
      '<input type="text" placeholder="输入摄像头名称、IP 或来源" /></div>' +
      '<div class="field"><label>' +
      icon('eye', 14) +
      ' 视频状态 </label>' +
      '<select><option>全部状态</option><option>可播放</option>' +
      '<option>等待 / 加载中</option><option>不可用</option></select></div>' +
      '<div class="field"><label>' +
      icon('algorithm', 14) +
      ' AI 状态 </label>' +
      '<select><option>全部状态</option><option>运行中</option>' +
      '<option>已停用</option><option>未绑定</option></select></div>' +
      '<div class="admin-query-bar__actions">' +
      '<button type="button" class="btn-secondary">' +
      icon('offline', 16) +
      ' 重置 </button>' +
      '<button type="button" class="btn-primary">' +
      icon('filter', 16) +
      ' 查询 </button>' +
      '</div>' +
      '</div>' +
      '</div>'
    );
  }

  function filterCounts() {
    var list = MockData.CAMERAS;
    return [
      ['全部', list.length],
      ['在线', list.filter(function (c) { return c.status === 'online'; }).length],
      ['异常', list.filter(function (c) { return c.statusTone === 'danger'; }).length],
      ['离线', list.filter(function (c) { return c.status === 'offline'; }).length],
      ['未分组', list.filter(function (c) { return c.groupName === '未分组'; }).length]
    ];
  }

  function overviewFilters() {
    return (
      '<div class="admin-filter-list admin-filter-list--inline admin-filter-list--prototype admin-filter-list--overview">' +
      filterCounts()
        .map(function (row, index) {
          return (
            '<button type="button" class="admin-filter-item' +
            (index === 0 ? ' is-active' : '') +
            '"><span>' +
            esc(row[0]) +
            '</span><strong>' +
            row[1] +
            '</strong></button>'
          );
        })
        .join('') +
      '</div>'
    );
  }

  /** 单张摄像头卡片（admin-camera-proto-card） */
  function cameraCard(cam) {
    var statusIcon = cam.statusTone === 'success' ? 'online' : cam.statusTone === 'danger' ? 'alert' : 'offline';
    return (
      '<article class="admin-camera-proto-card" data-camera="' +
      cam.id +
      '">' +
      '<div class="admin-camera-proto-card__title">' +
      '<span class="admin-camera-proto-card__badge" data-tone="' +
      esc(cam.statusTone) +
      '">' +
      icon(cam.statusTone === 'success' ? 'camera' : 'alert', 18) +
      '</span>' +
      '<div class="admin-camera-proto-card__title-copy">' +
      '<strong title="' +
      esc(cam.name) +
      '">' +
      esc(cam.name) +
      '</strong>' +
      '<p>' +
      esc(cam.displayName) +
      ' · ' +
      esc(cam.ip) +
      '</p>' +
      '</div>' +
      '<span class="admin-tone-chip" data-tone="' +
      esc(cam.statusTone) +
      '">' +
      icon(statusIcon, 13) +
      ' ' +
      esc(cam.statusText) +
      '</span>' +
      '</div>' +
      '<div class="admin-camera-proto-card__summary"><span>' +
      esc(cam.summary) +
      '</span></div>' +
      '<div class="admin-camera-proto-card__facts">' +
      '<span>' +
      icon('eye', 13) +
      esc(cam.videoText) +
      '</span>' +
      '<span>' +
      icon('algorithm', 13) +
      esc(cam.aiText) +
      '</span>' +
      '<span>' +
      icon('time', 13) +
      esc(cam.lastFrame) +
      '</span>' +
      '</div>' +
      '<div class="admin-camera-proto-card__algorithms" title="' +
      esc(cam.aiDetailText) +
      '"><span>' +
      esc(cam.aiDetailText) +
      '</span></div>' +
      '<div class="admin-camera-proto-card__actions">' +
      '<button type="button" class="btn-secondary" data-action="camera-credential">' +
      icon('credential', 15) +
      '<span>凭据</span></button>' +
      '<button type="button" class="btn-secondary" data-action="camera-rules">' +
      icon('rule', 15) +
      '<span>规则</span></button>' +
      '<button type="button" class="btn-secondary" data-action="camera-edit">' +
      icon('edit', 15) +
      '<span>编辑</span></button>' +
      '</div>' +
      '</article>'
    );
  }

  function tablePanel() {
    var list = MockData.CAMERAS;
    var hasData = list.length > 0;
    var body = hasData
      ? '<div class="admin-camera-card-grid admin-scroll-region">' +
        list.map(cameraCard).join('') +
        '</div>' +
        (list.length > PAGE_SIZE
          ? '<div class="admin-camera-pagination">' +
            '<span>第 1 / 1 页，共 ' +
            list.length +
            ' 台</span>' +
            '<div><button type="button" class="btn-secondary" disabled>上一页</button>' +
            '<button type="button" class="btn-secondary" disabled>下一页</button>' +
            '<select aria-label="每页数量"><option value="8">8 台 / 页</option>' +
            '<option value="12">12 台 / 页</option><option value="20">20 台 / 页</option></select></div>' +
            '</div>'
          : '')
      : UI.emptyState('当前筛选下没有摄像头', '点击右上角“添加摄像头”，或切换筛选查看其他设备。');

    return (
      '<div class="panel admin-section admin-table-panel">' +
      '<div class="admin-section__head admin-section__head--camera-overview">' +
      '<div class="admin-section__title-line"><strong class="admin-section__title">设备总览</strong>' +
      '<span class="admin-section__subline muted">共 ' +
      list.length +
      ' 台设备</span></div>' +
      '<div class="admin-camera-overview-tools">' +
      overviewFilters() +
      '</div>' +
      '<div class="admin-camera-overview-actions">' +
      '<button type="button" class="btn-secondary" data-action="refresh-cameras">' +
      icon('online', 16) +
      '<span> 刷新 </span></button>' +
      '<button type="button" class="btn-primary" data-action="open-add-camera">' +
      icon('plus', 16) +
      '<span> 添加摄像头 </span></button>' +
      '</div>' +
      '</div>' +
      body +
      '</div>'
    );
  }

  function discoveryRows() {
    return MockData.DISCOVERED.map(function (device, index) {
      return (
        '<div class="admin-device-card admin-camera-discovery-row">' +
        '<label class="admin-device-card__check">' +
        '<input type="checkbox" data-device-index="' +
        index +
        '" /></label>' +
        '<div class="admin-device-card__body">' +
        '<div class="admin-device-card__title-row"><strong>' +
        esc(device.name) +
        '</strong></div>' +
        '<span class="muted">' +
        esc(device.kind) +
        '</span></div>' +
        '<span class="admin-camera-discovery-row__address">' +
        esc(device.address) +
        '</span>' +
        '<span class="admin-tone-chip" data-tone="success">' +
        esc(device.status) +
        '</span>' +
        '<button type="button" class="btn-secondary" data-action="add-one">添加</button>' +
        '</div>'
      );
    }).join('');
  }

  function onvifTab() {
    return (
      '<div class="admin-sheet__toolbar">' +
      '<button type="button" class="btn-primary" data-action="scan-devices">' +
      icon('scan', 15) +
      '<span>扫描设备</span></button>' +
      '<button type="button" class="btn-secondary" data-action="batch-add">批量添加</button>' +
      '</div>' +
      '<div class="admin-camera-discovery-head" aria-hidden="true">' +
      '<span>设备</span><span>地址</span><span>状态</span><span></span></div>' +
      '<div class="admin-device-list admin-device-list--camera-add admin-scroll-region">' +
      discoveryRows() +
      '</div>' +
      '<div class="monitor-dialog__footer">' +
      '<span class="monitor-dialog__footer-note">扫描结果仅展示可解析的局域网设备。</span>' +
      '<div class="monitor-dialog__footer-actions">' +
      '<button type="button" class="btn-secondary" data-close>取消</button>' +
      '<button type="button" class="btn-primary" data-action="batch-add">' +
      icon('plus', 15) +
      '<span>批量添加</span></button>' +
      '</div>' +
      '</div>'
    );
  }

  function rtspTab() {
    return (
      '<div class="admin-form-grid">' +
      '<div class="field"><label>名称</label>' +
      '<input type="text" placeholder="例如：门口摄像头" /></div>' +
      '<div class="field"><label>RTSP 地址</label>' +
      '<input type="text" placeholder="rtsp://192.168.1.10:554/stream1" /></div>' +
      '<div class="field"><label>接入用户名（可选）</label>' +
      '<input type="text" placeholder="RTSP 账号" /></div>' +
      '<div class="field"><label>接入密码（可选）</label>' +
      '<input type="password" placeholder="输入后不会明文显示" /></div>' +
      '</div>' +
      '<div class="admin-sheet__toolbar">' +
      '<label class="admin-checkbox-pill">' +
      '<input type="checkbox" /><span>添加后立即启用</span></label>' +
      '<button type="button" class="btn-primary" data-action="add-rtsp">' +
      icon('plus', 15) +
      '<span>添加</span></button>' +
      '</div>' +
      '<div class="monitor-dialog__footer">' +
      '<span class="monitor-dialog__footer-note">地址与凭据仅用于本次添加，不会在页面明文展示。</span>' +
      '<div class="monitor-dialog__footer-actions">' +
      '<button type="button" class="btn-secondary" data-close>取消</button>' +
      '<button type="button" class="btn-primary" data-action="add-rtsp">' +
      icon('plus', 15) +
      '<span>确认添加</span></button>' +
      '</div>' +
      '</div>'
    );
  }

  function addCameraDialog() {
    var dlg = UI.dialog({
      dialogClass: 'monitor-dialog admin-sheet admin-camera-add-dialog',
      html:
        '<div class="monitor-dialog__header">' +
        '<div><h3>添加摄像头</h3>' +
        '<p class="muted">通过 ONVIF 扫描或手动 RTSP 添加摄像头</p></div>' +
        '<button type="button" class="monitor-dialog__close" data-close>关闭</button>' +
        '</div>' +
        '<div class="monitor-dialog__body admin-sheet__body">' +
        UI.tabSwitch(
          [
            { key: 'onvif', label: 'ONVIF 扫描' },
            { key: 'rtsp', label: '手动 RTSP' }
          ],
          'onvif'
        ) +
        '<div data-panel="onvif">' +
        onvifTab() +
        '</div>' +
        '<div data-panel="rtsp" hidden>' +
        rtspTab() +
        '</div>' +
        '</div>'
    });

    dlg.root.addEventListener('click', function (evt) {
      var tab = evt.target.closest('[data-tab]');
      if (tab) {
        var key = tab.getAttribute('data-tab');
        dlg.root.querySelectorAll('[data-tab]').forEach(function (el) {
          el.classList.toggle('is-active', el === tab);
        });
        dlg.root.querySelectorAll('[data-panel]').forEach(function (el) {
          el.hidden = el.getAttribute('data-panel') !== key;
        });
        return;
      }
      if (evt.target.closest('[data-action="scan-devices"]')) {
        dlg.root.querySelector('.admin-camera-discovery-head').classList.add('is-visible');
        global.App.notify('success', '局域网扫描已完成');
        return;
      }
      if (evt.target.closest('[data-action="batch-add"]') || evt.target.closest('[data-action="add-rtsp"]')) {
        global.App.notify('success', '摄像头已添加');
        dlg.close();
      }
    });
  }

  function render() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      queryBar() +
      tablePanel() +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      if (evt.target.closest('[data-action="open-add-camera"]')) {
        addCameraDialog();
        return;
      }
      if (evt.target.closest('[data-action="refresh-cameras"]')) {
        global.App.notify('success', '摄像头状态已刷新');
        return;
      }
      var cardBtn = evt.target.closest('.admin-camera-proto-card__actions button');
      if (cardBtn) {
        global.App.notify('info', '该操作在真机上需要设备接口支持，静态复刻页仅展示入口。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/cameras'] = { path: '/admin/cameras', render: render, mount: mount };
})(window);
