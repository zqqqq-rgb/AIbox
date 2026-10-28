/**
 * 算法仓库（/admin/algorithm-warehouse），对应原始 AlgorithmWarehouseView。
 * 结构严格对齐原渲染树：
 *   div.admin-page.warehouse-page
 *     section.admin-page__header.admin-page__header--slim.warehouse-heading（原版被 .admin-page__header{display:none} 隐藏）
 *     section.panel.warehouse-overview > warehouse-metrics + warehouse-overview__actions
 *     section.panel.warehouse-toolbar > warehouse-search + warehouse-filters
 *     section.warehouse-categories > article.panel.warehouse-category（纵向堆叠，点头部展开）
 *       └ div.warehouse-list > button.warehouse-row（6 栅格：状态点 / 标题 / 组件 / 版本 / 徽标 / 箭头）
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  var READY_RE = /^person_detector_|^hyperlpr3_/;

  var state = {
    keyword: '',
    filter: 'all',
    expanded: {},
    dialogCap: null
  };

  function capabilitiesOf(catId) {
    return MockData.CAPABILITIES.filter(function (cap) {
      return cap.category === catId;
    });
  }

  function isReady(cap) {
    return cap.state === '已接入';
  }

  /** 检测参数建议：仅人员检测与车牌识别链路给出（与原实现 A() 判断一致） */
  function hasThresholds(cap) {
    return !!cap.algo && READY_RE.test(cap.algo);
  }

  var THRESHOLD_ROWS = [
    {
      title: '检测置信度下限',
      value: '0.50 / 0.65',
      note: '人员检测与车牌识别链路推荐 0.50，其余能力 0.65。推荐值是当前模型经过验证的检测下限；更低数值不会增加有效检出。'
    }
  ];

  function metrics() {
    var s = MockData.SUMMARY;
    var blocks = [
      { tone: '', value: s.all, label: '全部能力' },
      { tone: ' ready', value: s.ready, label: '当前可用' },
      { tone: '', value: s.planned, label: '规划接入' },
      { tone: '', value: s.categories, label: '能力分类' }
    ];
    return (
      '<div class="warehouse-metrics">' +
      blocks
        .map(function (block, index) {
          return (
            (index ? '<i></i>' : '') +
            '<div' +
            (block.tone ? ' data-tone="ready"' : '') +
            '><strong>' +
            block.value +
            '</strong><span>' +
            block.label +
            '</span></div>'
          );
        })
        .join('') +
      '</div>'
    );
  }

  function overview() {
    return (
      '<section class="panel warehouse-overview" aria-label="算法仓库概览">' +
      metrics() +
      '<div class="warehouse-overview__actions">' +
      '<div class="warehouse-online-mode">' +
      '<span>' +
      icon('upload', 17) +
      '</span>' +
      '<div><strong>在线更新</strong><span>云端统一发布与校验</span></div>' +
      '</div>' +
      '<button type="button" class="warehouse-refresh" data-action="refresh-warehouse">' +
      icon('export', 15) +
      '<span>刷新</span></button>' +
      '</div>' +
      '</section>'
    );
  }

  function toolbar() {
    return (
      '<section class="panel warehouse-toolbar">' +
      '<label class="warehouse-search">' +
      '<span>' +
      icon('search', 15) +
      '</span>' +
      '<input type="search" data-warehouse-keyword placeholder="搜索能力，例如：安全帽、叉车、人脸" />' +
      '</label>' +
      '<div class="warehouse-filters" aria-label="状态筛选">' +
      '<button type="button" class="is-active" data-warehouse-filter="all">全部</button>' +
      '<button type="button" data-warehouse-filter="available">当前可用</button>' +
      '<button type="button" data-warehouse-filter="pending">规划接入</button>' +
      '</div>' +
      '</section>'
    );
  }

  function rowHtml(cap) {
    var ready = isReady(cap);
    var tags = (cap.tags || [])
      .map(function (tag) {
        return '<em>' + esc(tag) + '</em>';
      })
      .join('');
    return (
      '<button type="button" class="warehouse-row' +
      (ready ? '' : ' warehouse-row--pending') +
      '" data-cap="' +
      esc(cap.id) +
      '">' +
      '<span class="warehouse-row__state"><i' +
      (ready ? ' class="is-ready"' : '') +
      '></i></span>' +
      '<span class="warehouse-row__body">' +
      '<span class="warehouse-row__title"><strong>' +
      esc(cap.name) +
      '</strong>' +
      tags +
      '</span>' +
      '<small>' +
      esc(cap.desc) +
      '</small>' +
      '</span>' +
      '<span class="warehouse-row__component"><small>' +
      (cap.bound ? '当前组件' : '接入状态') +
      '</small><strong>' +
      esc(cap.bound && cap.component ? cap.component : cap.state) +
      '</strong></span>' +
      '<span class="warehouse-row__version"><small>' +
      (cap.version ? '当前版本' : '更新方式') +
      '</small><strong>' +
      esc(cap.version || '待云端发布') +
      '</strong></span>' +
      '<span class="warehouse-row__badge' +
      (ready ? ' is-ready' : '') +
      '">' +
      esc(cap.state) +
      '</span>' +
      '<span class="warehouse-row__arrow">' +
      icon('chevron-right', 16) +
      '</span>' +
      '</button>'
    );
  }

  function visibleCategories() {
    var keyword = state.keyword.trim().toLowerCase();
    return MockData.CATEGORIES.map(function (cat) {
      var items = capabilitiesOf(cat.id).filter(function (cap) {
        var passState =
          state.filter === 'all' ||
          (state.filter === 'available' ? isReady(cap) : !isReady(cap));
        var haystack = [cap.name, cap.desc].concat(cap.tags || []).join(' ').toLowerCase();
        return passState && (!keyword || haystack.indexOf(keyword) >= 0);
      });
      return { cat: cat, items: items };
    }).filter(function (entry) {
      return entry.items.length > 0;
    });
  }

  function categoriesHtml() {
    var entries = visibleCategories();
    if (!entries.length) {
      return (
        '<div class="panel warehouse-empty">' +
        icon('search', 26) +
        '<strong>没有符合条件的能力</strong>' +
        '<span>请调整搜索词或状态筛选。</span>' +
        '</div>'
      );
    }
    return (
      '<section class="warehouse-categories" aria-busy="false">' +
      entries
        .map(function (entry) {
          var cat = entry.cat;
          var ready = entry.items.filter(isReady).length;
          var open = !!state.expanded[cat.id];
          return (
            '<article class="panel warehouse-category" data-category="' +
            esc(cat.id) +
            '">' +
            '<button type="button" class="warehouse-category__head" data-category-head="' +
            esc(cat.id) +
            '">' +
            '<span class="warehouse-category__icon">' +
            icon(cat.icon, 20) +
            '</span>' +
            '<span class="warehouse-category__copy"><strong>' +
            esc(cat.name) +
            '</strong><small>' +
            esc(cat.desc) +
            '</small></span>' +
            '<span class="warehouse-category__progress"><b>' +
            ready +
            '</b><span>/ ' +
            entry.items.length +
            ' 可用</span></span>' +
            icon('chevron-right', 18, 1.9).replace(
              'class="admin-icon"',
              'class="admin-icon warehouse-category__chevron' + (open ? ' is-open' : '') + '"'
            ) +
            '</button>' +
            (open
              ? '<div class="warehouse-list">' + entry.items.map(rowHtml).join('') + '</div>'
              : '') +
            '</article>'
          );
        })
        .join('') +
      '</section>'
    );
  }

  function bodyHtml() {
    return overview() + toolbar() + categoriesHtml();
  }

  function render() {
    return (
      '<div class="admin-page warehouse-page">' +
      '<section class="admin-page__header admin-page__header--slim warehouse-heading">' +
      '<div><h1 class="admin-page__single-title">算法仓库</h1>' +
      '<p class="admin-page__subtitle">查看设备可用和规划中的视觉能力。算法版本由云端统一发布，任务运行仍在“算法任务”中配置。</p></div>' +
      '</section>' +
      '<div data-slot="warehouse-body">' +
      bodyHtml() +
      '</div>' +
      '</div>'
    );
  }

  function detailDialog(cap) {
    var ready = isReady(cap);
    var thresholds = hasThresholds(cap)
      ? '<section><h3>检测参数建议</h3>' +
        '<div class="warehouse-threshold-grid">' +
        THRESHOLD_ROWS.map(function (row) {
          return (
            '<div><strong>' +
            esc(row.title) +
            '</strong><span>' +
            esc(row.value) +
            '</span>' +
            '<p class="warehouse-threshold-note">' +
            esc(row.note) +
            '</p></div>'
          );
        }).join('') +
        '</div></section>'
      : '';
    var componentSection = cap.bound
      ? '<section><h3>当前组件</h3>' +
        '<div class="warehouse-detail-version"><span>' +
        esc(cap.component) +
        '</span><strong>' +
        esc(cap.version || '版本待确认') +
        '</strong></div>' +
        '<p class="warehouse-threshold-note">算法由云端统一发布、校验和分配，设备端不接受本地文件上传。</p>' +
        '</section>'
      : '';

    var dlg = UI.dialog({
      dialogClass: 'monitor-dialog warehouse-dialog',
      html:
        '<header class="monitor-dialog__header">' +
        '<div><span>能力说明</span><h2>' +
        esc(cap.name) +
        '</h2></div>' +
        '<button type="button" class="monitor-dialog__close" data-close>关闭</button>' +
        '</header>' +
        '<div class="monitor-dialog__body warehouse-dialog__body">' +
        '<div class="warehouse-detail-status' +
        (ready ? ' is-ready' : '') +
        '">' +
        icon(ready ? 'online' : 'time', 20) +
        '<div><strong>' +
        esc(cap.state) +
        '</strong><span>' +
        esc(MockData.STATE_DESC[cap.state] || '') +
        '</span></div>' +
        '</div>' +
        '<section><h3>能力用途</h3><p>' +
        esc(cap.desc) +
        '</p></section>' +
        componentSection +
        thresholds +
        '<section><h3>链路说明</h3>' +
        '<p>算法仓库只管理能力目录；摄像头绑定、识别参数和告警规则请在“算法任务”中设置。</p></section>' +
        '</div>' +
        '<div class="monitor-dialog__footer">' +
        '<span class="monitor-dialog__footer-note">分类：' +
        esc(cap.categoryName) +
        '</span>' +
        '<div class="monitor-dialog__footer-actions">' +
        '<button type="button" class="btn-primary" data-close>知道了</button>' +
        '</div>' +
        '</div>'
    });
    state.dialogCap = cap.id;
    return dlg;
  }

  /** 只重绘分类区（概览指标与工具栏保持不重建，避免丢失输入焦点） */
  function repaintCategories(root) {
    var slot = root.querySelector('[data-slot="warehouse-body"]');
    if (!slot) return;
    var old = slot.querySelector('.warehouse-categories, .warehouse-empty');
    if (!old) return;
    var tmp = document.createElement('div');
    tmp.innerHTML = categoriesHtml();
    old.replaceWith(tmp.firstElementChild);
  }

  function mount(root) {
    root.addEventListener('input', function (evt) {
      if (evt.target.matches('[data-warehouse-keyword]')) {
        state.keyword = evt.target.value;
        repaintCategories(root);
      }
    });

    root.addEventListener('click', function (evt) {
      var filterBtn = evt.target.closest('[data-warehouse-filter]');
      if (filterBtn) {
        state.filter = filterBtn.getAttribute('data-warehouse-filter');
        root.querySelectorAll('[data-warehouse-filter]').forEach(function (el) {
          el.classList.toggle('is-active', el === filterBtn);
        });
        repaintCategories(root);
        return;
      }

      var head = evt.target.closest('[data-category-head]');
      if (head) {
        var catId = head.getAttribute('data-category-head');
        state.expanded[catId] = !state.expanded[catId];
        repaintCategories(root);
        return;
      }

      var row = evt.target.closest('[data-cap]');
      if (row) {
        var capId = row.getAttribute('data-cap');
        var cap = MockData.CAPABILITIES.filter(function (item) {
          return item.id === capId;
        })[0];
        if (cap) detailDialog(cap);
        return;
      }

      if (evt.target.closest('[data-action="refresh-warehouse"]')) {
        global.App.notify('success', '算法仓库状态已刷新');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/algorithm-warehouse'] = {
    path: '/admin/algorithm-warehouse',
    render: render,
    mount: mount
  };
})(window);
