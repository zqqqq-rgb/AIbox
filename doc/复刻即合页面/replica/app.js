/**
 * 应用外壳与路由。
 * 结构对齐原始构建产物中的 AppShell 组件：
 *   div.app-layout > aside.app-sidebar + main.app-main
 * 监控大屏（/）为独立全屏页面，不走该外壳。
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;

  /** 当前路由：优先读 hash，其次读 pathname（便于 http 方式部署） */
  function currentRoute() {
    var raw = global.location.hash.replace(/^#/, '');
    if (!raw) {
      raw = global.location.pathname.indexOf('index.html') >= 0 ? '/' : global.location.pathname;
    }
    if (!raw || raw === '/index.html') raw = '/';
    if (raw.length > 1) raw = raw.replace(/\/$/, '');
    return raw || '/';
  }

  /** 侧边栏菜单项 */
  function navList(activePath) {
    return MockData.NAV.map(function (item) {
      var active =
        item.to === '/'
          ? activePath === '/'
          : activePath === item.to || activePath.indexOf(item.to + '/') === 0;
      return (
        '<a class="nav-link' +
        (active ? ' is-active' : '') +
        '" href="#' +
        item.to +
        '"' +
        (active ? ' aria-current="page"' : '') +
        '>' +
        '<span class="nav-link__icon">' +
        icon(item.icon, 18) +
        '</span>' +
        '<span class="nav-link__body"><span class="nav-link__label">' +
        esc(item.label) +
        '</span></span>' +
        '</a>'
      );
    }).join('');
  }

  /** 外壳 HTML */
  function shellHtml(path, page) {
    var nav = MockData.NAV;
    var current = nav.filter(function (item) {
      return item.to === path;
    })[0] || nav[1];
    return (
      '<div class="app-layout">' +
      '<aside class="app-sidebar">' +
      '<div class="app-sidebar__brand">' +
      '<div class="app-sidebar__brand-mark">' +
      icon('algorithm', 22) +
      '</div>' +
      '<div class="app-sidebar__brand-copy"><strong>即合视觉AI</strong><span>管理后台</span></div>' +
      '</div>' +
      '<nav class="nav-list" aria-label="主导航">' +
      navList(path, current) +
      '</nav>' +
      '<div class="app-sidebar__footer">' +
      '<div class="app-sidebar__identity"><span>当前账号</span><strong>admin</strong></div>' +
      '<button type="button" class="btn-secondary app-sidebar__logout" aria-label="退出登录" data-action="logout">' +
      icon('offline', 16) +
      '<span>退出</span>' +
      '</button>' +
      '</div>' +
      '</aside>' +
      '<main class="app-main">' +
      '<header class="app-main__topbar">' +
      '<div class="app-main__topbar-card">' +
      '<div class="app-main__title">' +
      '<div class="app-main__heading"><strong>' +
      esc(current.label) +
      '</strong><span>' +
      esc(current.note) +
      '</span></div>' +
      '</div>' +
      '<div class="app-main__top-actions">' +
      (page.topActions ? page.topActions() : '') +
      '</div>' +
      '</div>' +
      '</header>' +
      '<div class="app-main__content">' +
      page.render() +
      '</div>' +
      '</main>' +
      '</div>'
    );
  }

  /** 轻量通知（对应原 app-notifications / app-notification 结构） */
  function notify(tone, message) {
    var host = document.querySelector('.app-notifications');
    if (!host) {
      host = document.createElement('div');
      host.className = 'app-notifications';
      host.setAttribute('aria-live', 'polite');
      host.setAttribute('aria-atomic', 'false');
      document.body.appendChild(host);
    }
    var titleMap = { success: '操作成功', error: '操作失败', warning: '需要留意', info: '提示' };
    var el = document.createElement('div');
    el.className = 'app-notification';
    el.setAttribute('data-tone', tone);
    el.innerHTML =
      '<div class="app-notification__content"><strong>' +
      esc(titleMap[tone] || '提示') +
      '</strong><p>' +
      esc(message) +
      '</p></div>' +
      '<button type="button" class="app-notification__close" aria-label="关闭">' +
      icon('close', 14) +
      '</button>';
    host.appendChild(el);
    el.querySelector('.app-notification__close').addEventListener('click', function () {
      el.remove();
    });
    global.setTimeout(function () {
      el.remove();
    }, tone === 'error' ? 7000 : 4500);
  }

  function render() {
    var path = currentRoute();
    var page = (global.Pages || {})[path] || (global.Pages || {})['/admin/cameras'];
    if (!page) return;
    var host = document.getElementById('app');
    host.innerHTML = page.full ? page.render() : shellHtml(path, page);
    if (page.mount) page.mount(host, path);
  }

  document.addEventListener('click', function (evt) {
    var target = evt.target.closest('[data-action="logout"]');
    if (!target) return;
    if (global.confirm('确认退出登录吗？')) {
      global.location.hash = '#/';
      render();
      notify('success', '已退出当前账号');
    }
  });

  global.addEventListener('hashchange', render);
  global.App = { render: render, notify: notify, currentRoute: currentRoute };
  render();
})(window);
