/**
 * AI研判（/admin/ai-review），对应原始 AiReviewView。
 * 研判任务为空（新装状态），页面同时展示云端协同策略说明。
 */
(function (global) {
  var icon = global.icon;
  var UI = global.UI;

  function render() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      '<section class="panel admin-section admin-ai-review-main-panel">' +
      '<div class="admin-section__head">' +
      '<div class="admin-section__title-line">' +
      '<h2 class="admin-section__title">研判任务列表</h2>' +
      '<p class="admin-section__subline">共 0 条，统一记录告警研判、云端巡检和语义规则任务。</p>' +
      '</div>' +
      '<div class="admin-ai-review-panel-actions">' +
      UI.toneChip('未启用', 'info') +
      '<button type="button" class="btn-secondary" data-action="refresh-review">' +
      icon('export', 16) +
      ' 刷新 </button>' +
      '<button type="button" class="btn-primary" data-action="review-config">' +
      icon('algorithm', 16) +
      ' AI服务配置 </button>' +
      '</div>' +
      '</div>' +
      UI.emptyState(
        '暂无研判任务',
        '进入“告警事件”详情，点击“AI研判”后会在这里显示。后续云端巡检和语义规则也会进入同一列表。',
        'admin-empty-state--compact'
      ) +
      '</section>' +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      if (evt.target.closest('[data-action="refresh-review"]')) {
        global.App.notify('success', '研判任务已刷新');
        return;
      }
      if (evt.target.closest('[data-action="review-config"]')) {
        global.App.notify('info', '未开启自动策略前，不会自动发送事件图片。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/ai-review'] = { path: '/admin/ai-review', render: render, mount: mount };
})(window);
