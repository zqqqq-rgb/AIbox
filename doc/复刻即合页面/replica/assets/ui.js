/**
 * 通用 UI 片段与交互（对话框、页签、状态胶囊、空态、分页）。
 * 样式类名与原始构建产物保持一致，直接复用原 CSS。
 */
(function (global) {
  /** HTML 转义 */
  function esc(value) {
    if (value == null) return '';
    return String(value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  /** 空态块 */
  function emptyState(title, note, extraClass) {
    return (
      '<div class="admin-empty-state' +
      (extraClass ? ' ' + extraClass : '') +
      '"><strong>' +
      esc(title) +
      '</strong>' +
      (note ? '<p class="muted">' + esc(note) + '</p>' : '') +
      '</div>'
    );
  }

  /** 状态胶囊（admin-tone-chip，带 data-tone 以匹配原 CSS 配色） */
  function toneChip(label, tone, extraClass) {
    var cls = 'admin-tone-chip' + (extraClass ? ' ' + extraClass : '');
    return '<span class="' + cls + '" data-tone="' + esc(tone || 'info') + '">' + esc(label) + '</span>';
  }

  /** 页签切换器：label 文本 + data-tab 值 */
  function tabSwitch(items, active, attr) {
    return (
      '<div class="admin-tab-switch">' +
      items
        .map(function (item) {
          var isActive = item.key === active;
          return (
            '<button type="button" class="' +
            (isActive ? 'is-active' : '') +
            '" data-' +
            (attr || 'tab') +
            '="' +
            esc(item.key) +
            '">' +
            esc(item.label) +
            '</button>'
          );
        })
        .join('') +
      '</div>'
    );
  }

  /** 对话框容器（monitor-dialog-backdrop + monitor-dialog） */
  function dialog(options) {
    var backdrop = document.createElement('div');
    backdrop.className = 'monitor-dialog-backdrop';
    backdrop.innerHTML =
      '<div class="' +
      (options.dialogClass || 'monitor-dialog admin-sheet') +
      '" role="dialog" aria-modal="true">' +
      options.html +
      '</div>';
    document.body.appendChild(backdrop);

    function close() {
      backdrop.remove();
      document.removeEventListener('keydown', onKey);
    }
    function onKey(evt) {
      if (evt.key === 'Escape') close();
    }
    document.addEventListener('keydown', onKey);
    backdrop.addEventListener('click', function (evt) {
      var target = evt.target;
      if (target === backdrop) close();
      var closer = target.closest('[data-close]');
      if (closer) close();
    });
    return { root: backdrop, close: close };
  }

  /** 分页摘要（右侧：每页数量 + 上一页 / 下一页） */
  function pagination(total, page, size, sizeOptions) {
    var sizes = sizeOptions || [20, 50, 100];
    return (
      '<div class="admin-camera-pagination">' +
      '<span class="muted">共 ' +
      total +
      ' 条</span>' +
      '<label class="muted">每页数量' +
      '<select>' +
      sizes
        .map(function (s) {
          return '<option' + (s === size ? ' selected' : '') + '>' + s + '</option>';
        })
        .join('') +
      '</select></label>' +
      '<button type="button" class="btn-secondary" disabled>上一页</button>' +
      '<button type="button" class="btn-secondary" disabled>下一页</button>' +
      '</div>'
    );
  }

  /** 面板头（左侧标题 + 说明，右侧工具区） */
  function sectionHead(title, subline, tools, modifier) {
    return (
      '<div class="admin-section__head' +
      (modifier ? ' ' + modifier : '') +
      '"><div><div class="admin-section__title-line"><strong class="admin-section__title">' +
      esc(title) +
      '</strong></div>' +
      (subline ? '<div class="admin-section__subline muted">' + esc(subline) + '</div>' : '') +
      '</div>' +
      (tools ? '<div class="admin-section__tools">' + tools + '</div>' : '') +
      '</div>'
    );
  }

  global.UI = {
    esc: esc,
    emptyState: emptyState,
    toneChip: toneChip,
    tabSwitch: tabSwitch,
    dialog: dialog,
    pagination: pagination,
    sectionHead: sectionHead
  };
})(window);
