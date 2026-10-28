// caps.js —— 机制分布图（ECharts）+ 表格筛选（vanilla）
(function () {
  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var ok = style.getPropertyValue('--ok').trim();

  // ---- 图 7-1 机制分布（与表格行 data-mech 一致）----
  var el = document.getElementById('chart-mech');
  if (el && typeof echarts !== 'undefined') {
    var chart = echarts.init(el, null, { renderer: 'svg' });
    var data = [
      { value: 19, name: 'R8 基准几何', itemStyle: { color: accent2 } },
      { value: 17, name: 'R6 复合', itemStyle: { color: '#7b52a8' } },
      { value: 12, name: 'R2 计数', itemStyle: { color: '#3f8fb0' } },
      { value: 11, name: '级联', itemStyle: { color: accent } },
      { value: 10, name: '检测直出', itemStyle: { color: ok } },
      { value: 10, name: '现有规则', itemStyle: { color: '#4a6b8a' } },
      { value: 7,  name: 'R3 缺位', itemStyle: { color: '#b0733f' } },
      { value: 6,  name: 'R5 靠近', itemStyle: { color: '#5a9a4e' } },
      { value: 5,  name: 'R1 越线', itemStyle: { color: '#8a6d3b' } },
      { value: 4,  name: '业务层', itemStyle: { color: muted } },
      { value: 1,  name: 'R4 徘徊', itemStyle: { color: '#a84a6d' } },
      { value: 1,  name: 'R7 测速', itemStyle: { color: '#6a8a2b' } }
    ];
    chart.setOption({
      animation: false,
      tooltip: { appendToBody: true, trigger: 'item', formatter: '{b}<br/>{c} 项（{d}%）' },
      legend: { bottom: 0, textStyle: { color: muted, fontSize: 12 }, itemWidth: 12, itemHeight: 8, type: 'scroll' },
      series: [{
        name: '触发机制', type: 'pie', radius: ['40%', '66%'], center: ['50%', '44%'],
        label: { color: ink, fontSize: 12.5, formatter: '{b} {c}' },
        labelLine: { lineStyle: { color: rule } },
        data: data
      }]
    });
    window.addEventListener('resize', function () { chart.resize(); });
  }

  // ---- 表格筛选 ----
  var bar = document.getElementById('filterBar');
  var count = document.getElementById('filterCount');
  if (!bar) return;
  var rows = Array.prototype.slice.call(document.querySelectorAll('tr[data-mech]'));
  var sections = Array.prototype.slice.call(document.querySelectorAll('section[id^="s"]:not(#s0):not(#s7)'));

  function apply(mech) {
    var shown = 0;
    rows.forEach(function (tr) {
      var hit = (mech === 'all') || (tr.getAttribute('data-mech') === mech);
      tr.style.display = hit ? '' : 'none';
      if (hit) shown++;
    });
    // 空分类隐藏整节
    sections.forEach(function (sec) {
      var vis = sec.querySelectorAll('tr[data-mech]').length;
      var any = Array.prototype.some.call(sec.querySelectorAll('tr[data-mech]'), function (tr) {
        return tr.style.display !== 'none';
      });
      sec.style.display = (vis > 0 && !any) ? 'none' : '';
    });
    if (count) count.textContent = '显示 ' + shown + ' / 103 项';
  }

  bar.addEventListener('click', function (e) {
    var btn = e.target.closest('.fbtn');
    if (!btn) return;
    bar.querySelectorAll('.fbtn').forEach(function (b) { b.classList.remove('on'); });
    btn.classList.add('on');
    apply(btn.getAttribute('data-mech'));
  });
  apply('all');
})();
