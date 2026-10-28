/**
 * 算法任务（/admin/rules），对应原始 RulesView。
 * 有任务时渲染 admin-task-table（4 列：摄像头与任务 / 算法与告警条件 / 状态 / 操作）。
 */
(function (global) {
  var MockData = global.MockData;
  var icon = global.icon;
  var esc = global.UI.esc;
  var UI = global.UI;

  function queryBar() {
    return (
      '<div class="panel admin-section admin-task-query-panel admin-query-panel">' +
      '<div class="admin-query-bar admin-query-bar--tasks">' +
      '<div class="field"><label>' +
      icon('filter', 14) +
      ' 任务名称 </label>' +
      '<input type="text" placeholder="输入任务名称、设备名或分析能力" /></div>' +
      '<div class="field"><label>' +
      icon('health', 14) +
      ' 任务状态 </label>' +
      '<select><option>全部状态</option><option>启用中</option>' +
      '<option>已停用</option></select></div>' +
      '<div class="admin-query-bar__actions">' +
      '<button type="button" class="btn-secondary" data-action="reset-rules">' +
      icon('offline', 16) +
      '<span> 重置 </span></button>' +
      '<button type="button" class="btn-primary">' +
      icon('filter', 16) +
      '<span> 查询 </span></button>' +
      '</div>' +
      '</div>' +
      '</div>'
    );
  }

  /** 摄像头维度统计：已启用 / 已停止 / 未配置 */
  function cameraCounters() {
    var cameras = MockData.CAMERAS;
    var enabled = 0;
    var stopped = 0;
    var unconfigured = 0;
    cameras.forEach(function (cam) {
      var tasks = cam.tasks || [];
      if (!tasks.length) unconfigured += 1;
      else if (tasks.some(function (t) { return t.runtimeStatus === 'running'; })) enabled += 1;
      else stopped += 1;
    });
    return { enabled: enabled, stopped: stopped, unconfigured: unconfigured };
  }

  function alertLabels(task) {
    return [MockData.EVENT_LABELS[task.eventType] || task.eventType];
  }

  /** 按摄像头聚合：真机一行代表一台摄像头的全部算法任务（多个算法芯片同行） */
  function cameraRows() {
    return MockData.CAMERAS.map(function (cam) {
      var tasks = cam.tasks || [];
      return {
        camera: cam,
        taskName: tasks.length ? tasks[0].taskName : '未配置算法任务',
        chips: tasks.map(function (task) {
          return {
            id: task.algorithmId,
            name: MockData.algoDisplayName(task.algorithmId),
            enabled: task.runtimeStatus === 'running',
            ruleId: task.ruleId,
            eventType: task.eventType
          };
        }),
        labels: tasks.reduce(function (acc, task) {
          return acc.concat(alertLabels(task));
        }, []),
        running: tasks.filter(function (task) {
          return task.runtimeStatus === 'running';
        }).length,
        ruleIds: tasks.map(function (task) {
          return task.ruleId;
        })
      };
    });
  }

  function taskRow(row) {
    var cam = row.camera;
    var labels = row.labels;
    return (
      '<article class="admin-task-table__row" data-camera="' +
      cam.id +
      '">' +
      '<div class="admin-task-identity-cell">' +
      '<strong title="' +
      esc(cam.name) +
      '">' +
      esc(cam.name) +
      '</strong>' +
      '<span title="' +
      esc(row.taskName) +
      '">' +
      esc(row.taskName) +
      '</span>' +
      '</div>' +
      '<div class="admin-task-config-cell">' +
      '<span class="admin-algorithm-chip-list">' +
      row.chips
        .map(function (chip) {
          return (
            '<span class="admin-algorithm-chip" data-enabled="' +
            (chip.enabled ? 'true' : 'false') +
            '" data-tone="' +
            (chip.enabled ? 'success' : 'warning') +
            '" title="' +
            (chip.enabled ? '运行中：' : '未运行：') +
            esc(chip.name) +
            '">' +
            esc(chip.name) +
            '</span>'
          );
        })
        .join('') +
      '</span>' +
      '<small class="admin-task-rule-summary"> 告警条件：' +
      esc(labels.length ? labels.join(' / ') : '未选择正式告警条件') +
      '</small>' +
      '</div>' +
      '<div class="admin-task-status-cell">' +
      '<span class="admin-tone-chip admin-tone-chip--task-state" data-tone="' +
      (row.running ? 'success' : 'warning') +
      '" title="' +
      row.running +
      ' / ' +
      row.chips.length +
      ' 个算法运行中">' +
      (row.running ? '运行中' : '已停止') +
      '</span>' +
      '<small>' +
      (row.chips.length ? row.chips.length + ' 个算法参与检测' : '等待添加分析能力') +
      '</small>' +
      '</div>' +
      '<div class="admin-task-table__actions" title="任务 ' +
      esc(row.chips.map(function (chip) { return chip.ruleId; }).join(',')) +
      ' / 规则 ' +
      esc(row.ruleIds.join(',')) +
      '">' +
      '<button type="button" class="admin-text-action" data-action="task-toggle">停止</button>' +
      '<button type="button" class="admin-text-action" data-action="task-events"> 查看 </button>' +
      '<button type="button" class="admin-text-action" data-action="task-edit">编辑</button>' +
      '<button type="button" class="admin-text-action admin-text-action--danger" data-action="task-delete">删除</button>' +
      '</div>' +
      '</article>'
    );
  }

  function taskPanel() {
    var rows = cameraRows();
    var counters = cameraCounters();
    var runningCameras = MockData.CAMERAS.filter(function (cam) {
      return (cam.tasks || []).some(function (t) { return t.runtimeStatus === 'running'; });
    }).length;
    return (
      '<div class="panel admin-section admin-table-panel">' +
      '<div class="admin-section__head admin-section__head--task-list">' +
      '<div class="admin-section__title-line">' +
      '<h2 class="admin-section__title">任务列表</h2>' +
      '<p class="admin-section__subline"> 当前 ' +
      runningCameras +
      ' / ' +
      MockData.CAMERAS.length +
      ' 个摄像头 </p>' +
      '</div>' +
      '<div class="admin-task-overview" aria-label="任务概览">' +
      '<span data-tone="success">已启用 ' +
      counters.enabled +
      '</span>' +
      '<span data-tone="neutral">已停止 ' +
      counters.stopped +
      '</span>' +
      '<span data-tone="warning">未配置 ' +
      counters.unconfigured +
      '</span>' +
      '</div>' +
      '<div class="admin-task-list-actions">' +
      '<button type="button" class="btn-secondary" data-action="refresh-tasks">' +
      icon('online', 16) +
      '<span> 刷新 </span></button>' +
      '<button type="button" class="btn-primary" data-action="new-task">' +
      icon('plus', 16) +
      '<span> 新建任务 </span></button>' +
      '<button type="button" class="btn-secondary" data-action="view-events">' +
      icon('alert', 16) +
      '<span> 查看告警事件 </span></button>' +
      '</div>' +
      '</div>' +
      (rows.length
        ? '<div class="admin-task-table admin-scroll-region">' +
          '<div class="admin-task-table__head">' +
          '<span>摄像头与任务</span><span>算法与告警条件</span><span>状态</span><span>操作</span>' +
          '</div>' +
          rows.map(taskRow).join('') +
          '</div>'
        : UI.emptyState('当前筛选下暂无任务', ' 可以调整查询条件，或直接创建新的算法任务。 ')) +
      '</div>'
    );
  }

  function render() {
    return (
      '<div class="admin-workbench admin-workbench--single-title">' +
      queryBar() +
      taskPanel() +
      '</div>'
    );
  }

  function mount(root) {
    root.addEventListener('click', function (evt) {
      if (evt.target.closest('[data-action="view-events"]')) {
        global.location.hash = '#/admin/events';
        return;
      }
      if (evt.target.closest('[data-action="refresh-tasks"]')) {
        global.App.notify('success', '任务状态已刷新');
        return;
      }
      if (evt.target.closest('[data-action="new-task"]')) {
        global.App.notify('info', '新建任务需要设备接口支持，静态复刻页仅展示入口。');
        return;
      }
      if (evt.target.closest('.admin-task-table__actions button')) {
        global.App.notify('info', '该操作在真机上需要设备接口支持，静态复刻页仅展示入口。');
      }
    });
  }

  global.Pages = global.Pages || {};
  global.Pages['/admin/rules'] = { path: '/admin/rules', render: render, mount: mount };
})(window);
