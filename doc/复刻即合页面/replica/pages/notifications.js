/**
 * 通知中心（/admin/notifications），对应原始 NotificationsView。
 * 指标卡：可用渠道 / 启用路由 / 投递中 / 发送失败；页签：通知渠道 / 告警路由 / 投递记录。
 */
(function (global) {
  var icon = global.icon;
  var UI = global.UI;

  var TABS = [
    { key: 'channels', label: '通知渠道', empty: '尚未配置通知渠道' },
    { key: 'routes', label: '告警路由', empty: '尚未配置告警路由' },
    { key: 'deliveries', label: '投递记录', empty: '暂无通知投递记录' }
  ];

  function summary() {
    var rows = [
      ['可用渠道', 0],
      ['启用路由', 0],
      ['投递中', 0],
      ['发送失败', 0]
    ];
    return rows
      .map(function (row) {
        return (
          '<div class="notification-summary__item">' +
          '<span class="muted">' +
          row[0] +
          '</span><strong>' +
          row[1] +
          '</strong></div>'
        );
      })
      .join('');
  }

  function render() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      '<div class="panel admin-section notification-summary" aria-label="硬件联动概览">' +
      summary() +
      '</div>' +
      '<div class="notification-toolbar">' +
      UI.tabSwitch(TABS, 'channels', 'notification-tab') +
      '<button type="button" class="btn-primary" data-action="new-channel">' +
      icon('plus', 15) +
      '<span>新增渠道</span></button>' +
      '</div>' +
      '<div class="panel notification-table-panel">' +
      '<div class="admin-empty-state">' +
      '<strong>尚未配置通知渠道</strong>' +
      '<p class="muted">配置渠道与路由后，告警会按规则推送到企业微信、钉钉、飞书或邮件。</p>' +
      '</div>' +
      '</div>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      var tabBtn = evt.target.closest('[data-notification-tab]');
      if (tabBtn) {
        var key = tabBtn.getAttribute('data-notification-tab');
        var tab = TABS.filter(function (item) {
          return item.key === key;
        })[0];
        root.querySelectorAll('[data-notification-tab]').forEach(function (el) {
          el.classList.toggle('is-active', el === tabBtn);
        });
        root.querySelector('.admin-empty-state strong').textContent = tab.empty;
        return;
      }
      if (evt.target.closest('[data-action="new-channel"]')) {
        global.App.notify('info', '支持企业微信机器人、钉钉机器人、飞书机器人与 SMTP 邮件渠道。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/notifications'] = {
    path: '/admin/notifications',
    render: render,
    mount: mount
  };
})(window);
