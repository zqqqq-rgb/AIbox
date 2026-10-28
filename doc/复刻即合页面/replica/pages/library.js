/**
 * 底库管理（/admin/base-library），对应原始 BaseLibraryView / library-* 样式族。
 * 真实结构（取自渲染树）：
 *   div.library-page
 *     div.library-tabs[role=tablist][aria-label=底库类型] > button[role=tab][aria-selected] ×4
 *     div.library-filters > input ×2 + div.library-filters__actions（重置 / 查询）
 *     div.library-panel
 *       div.library-panel__head > 标题 + 数量 + div.library-panel__actions
 *       div.library-table-wrap > table.library-table
 *         thead: th.check-cell + th ×8
 *         tbody: 空态为单行 td[colspan=9] > div.library-empty
 *       div.library-pagination > __summary / __controls / __size
 */
(function (global) {
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  var TABS = [
    {
      key: 'person',
      label: '人脸底库',
      icon: 'user',
      keyword: { label: '关键词', placeholder: '人员ID / 姓名' },
      tag: { label: '标签', placeholder: '输入标签' },
      columns: ['人员ID', '图片', '姓名', '名单 / 标签', '特征', '状态', '创建时间', '操作'],
      tip: '人员底库仅支持 JPG 或 PNG 图片'
    },
    {
      key: 'vehicle',
      label: '车辆底库',
      icon: 'vehicle',
      keyword: { label: '关键词', placeholder: '车牌号 / 车辆名称' },
      tag: { label: '标签', placeholder: '输入标签' },
      columns: ['车牌号', '图片', '车辆名称', '标签', '状态', '创建时间', '操作'],
      tip: '完整车牌号建议按实际号牌填写，便于比对'
    },
    {
      key: 'workwear',
      label: '工服底库',
      icon: 'workwear',
      keyword: { label: '关键词', placeholder: '工号 / 姓名' },
      tag: { label: '标签', placeholder: '输入标签' },
      columns: ['工号', '图片', '姓名', '部门', '状态', '创建时间', '操作'],
      tip: '工号或唯一编号用于区分同一部门的不同人员'
    },
    {
      key: 'ppe',
      label: 'PPE底库',
      icon: 'ppe',
      keyword: { label: '关键词', placeholder: '编号 / 名称' },
      tag: { label: '标签', placeholder: '输入标签' },
      columns: ['编号', '图片', '名称', 'PPE 类型', '状态', '创建时间', '操作'],
      tip: '仅支持 JPG、PNG 或 WebP 图片，单张不超过 5MB'
    }
  ];

  var activeTab = 'person';

  function tabOf(key) {
    return TABS.filter(function (t) {
      return t.key === key;
    })[0] || TABS[0];
  }

  function tabsHtml() {
    return (
      '<div class="library-tabs" role="tablist" aria-label="底库类型">' +
      TABS.map(function (tab) {
        var active = tab.key === activeTab;
        return (
          '<button type="button" class="library-tab' +
          (active ? ' is-active' : '') +
          '" role="tab" aria-selected="' +
          (active ? 'true' : 'false') +
          '" data-library-tab="' +
          esc(tab.key) +
          '">' +
          icon(tab.icon, 17) +
          '<span>' +
          esc(tab.label) +
          '</span></button>'
        );
      }).join('') +
      '</div>'
    );
  }

  function filtersHtml(tab) {
    return (
      '<div class="library-filters">' +
      '<label><span>' +
      esc(tab.keyword.label) +
      '</span><input type="search" placeholder="' +
      esc(tab.keyword.placeholder) +
      '" /></label>' +
      '<label><span>' +
      esc(tab.tag.label) +
      '</span><input type="search" placeholder="' +
      esc(tab.tag.placeholder) +
      '" /></label>' +
      '<div class="library-filters__actions">' +
      '<button type="button" class="btn-secondary">' +
      icon('offline', 16) +
      ' 重置 </button>' +
      '<button type="button" class="btn-primary">' +
      icon('filter', 16) +
      ' 查询 </button>' +
      '</div>' +
      '</div>'
    );
  }

  function panelHtml(tab) {
    var span = tab.columns.length + 1;
    return (
      '<div class="library-panel">' +
      '<div class="library-panel__head">' +
      '<strong class="admin-section__title">' +
      esc(tab.label) +
      '</strong>' +
      '<span class="muted">共 0 条</span>' +
      '<div class="library-panel__actions">' +
      '<button type="button" class="btn-primary" data-action="library-add">' +
      icon('plus', 16) +
      ' 新增 </button>' +
      '<button type="button" class="btn-secondary" data-action="library-remove">' +
      icon('trash', 16) +
      ' 批量删除 </button>' +
      '<button type="button" class="btn-secondary" data-action="library-merge">合并为同一人</button>' +
      '</div>' +
      '</div>' +
      '<div class="library-table-wrap">' +
      '<table class="library-table">' +
      '<thead><tr>' +
      '<th class="check-cell"><input type="checkbox" aria-label="选择全部" /></th>' +
      tab.columns
        .map(function (col) {
          return '<th>' + esc(col) + '</th>';
        })
        .join('') +
      '</tr></thead>' +
      '<tbody><tr><td colspan="' +
      span +
      '"><div class="library-empty">' +
      icon(tab.icon, 28) +
      '<strong>暂无' +
      esc(tab.label) +
      '条目</strong></div></td></tr></tbody>' +
      '</table>' +
      '</div>' +
      '<div class="library-pagination">' +
      '<div class="library-pagination__summary">0/0 共 0 条</div>' +
      '<div class="library-pagination__controls">' +
      '<button type="button" class="btn-secondary" disabled>上一页</button>' +
      '<button type="button" class="btn-secondary" disabled>下一页</button>' +
      '</div>' +
      '<label class="library-pagination__size" aria-label="每页条数">每页' +
      '<select><option>20</option><option>50</option><option>100</option></select>条</label>' +
      '</div>' +
      '</div>'
    );
  }

  function render() {
    var tab = tabOf(activeTab);
    return (
      '<section class="library-page">' +
      tabsHtml() +
      '<div data-slot="library-filters">' +
      filtersHtml(tab) +
      '</div>' +
      '<div data-slot="library-panel">' +
      panelHtml(tab) +
      '</div>' +
      '</section>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      var tabBtn = evt.target.closest('[data-library-tab]');
      if (tabBtn) {
        activeTab = tabBtn.getAttribute('data-library-tab');
        var tab = tabOf(activeTab);
        root.querySelectorAll('[data-library-tab]').forEach(function (el) {
          var selected = el === tabBtn;
          el.classList.toggle('is-active', selected);
          el.setAttribute('aria-selected', selected ? 'true' : 'false');
        });
        root.querySelector('[data-slot="library-filters"]').innerHTML = filtersHtml(tab);
        root.querySelector('[data-slot="library-panel"]').innerHTML = panelHtml(tab);
        return;
      }
      if (evt.target.closest('[data-action="library-add"]')) {
        global.App.notify('info', '请先上传图片，系统会自动检测人脸并提取身份特征。');
        return;
      }
      if (evt.target.closest('[data-action="library-merge"]')) {
        global.App.notify('info', '请先选择需要合并的底库条目。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/base-library'] = { path: '/admin/base-library', render: render, mount: mount };
})(window);
