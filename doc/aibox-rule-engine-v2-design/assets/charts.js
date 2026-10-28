// charts.js —— 图 1-1 能力承载机制分布（ECharts，主题色取自 CSS 变量）
(function () {
  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var bg2 = style.getPropertyValue('--bg2').trim();
  var ok = style.getPropertyValue('--ok').trim();

  var el = document.getElementById('chart-dist');
  if (!el || typeof echarts === 'undefined') return;
  var chart = echarts.init(el, null, { renderer: 'svg' });

  chart.setOption({
    animation: false,
    tooltip: { appendToBody: true, trigger: 'item', formatter: '{b}<br/>{c} 项（{d}%）' },
    legend: {
      bottom: 0, textStyle: { color: muted, fontSize: 12 },
      itemWidth: 12, itemHeight: 8
    },
    series: [
      {
        name: '承载机制',
        type: 'pie',
        radius: ['42%', '66%'],
        center: ['50%', '44%'],
        label: { color: ink, fontSize: 13, formatter: '{b}\n{c} 项' },
        labelLine: { lineStyle: { color: rule } },
        data: [
          { value: 31, name: '纯检测/现有规则', itemStyle: { color: ok } },
          { value: 47, name: '规则基元 v2（8 种）', itemStyle: { color: accent2 } },
          { value: 15, name: '级联 + 新后处理', itemStyle: { color: accent } },
          { value: 10, name: '业务侧承载', itemStyle: { color: muted } }
        ]
      }
    ]
  });
  window.addEventListener('resize', function () { chart.resize(); });
})();
