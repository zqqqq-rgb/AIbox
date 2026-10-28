/**
 * 图标集：与原始构建产物中的 AdminIcon 组件完全一致（path / circle 数据逐条对齐）。
 * 原始组件签名：props { name, size=18, strokeWidth=1.9 }
 * 渲染结果：<svg class="admin-icon" width height viewBox="0 0 24 24" fill="none"
 *            stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"
 *            stroke-width aria-hidden="true">
 */
(function (global) {
  var ICONS = {
    home: { paths: ['M4 11 12 4l8 7', 'M6.5 10.5V20h11V10.5', 'M10 20v-5h4v5'] },
    camera: {
      paths: ['M4 8h5l1.5-2h3L15 8h5v11H4z', 'M9 13.5a3 3 0 1 0 6 0a3 3 0 1 0 -6 0']
    },
    scan: {
      paths: ['M4 8V5h3', 'M20 8V5h-3', 'M4 16v3h3', 'M20 16v3h-3', 'M8 12h8', 'M12 8v8']
    },
    online: { paths: ['M5 12.5l4.2 4L19 7.8'] },
    offline: { paths: ['M7 7l10 10', 'M17 7 7 17'] },
    alert: { paths: ['M12 4 21 19H3z', 'M12 9v4', 'M12 17h.01'] },
    rule: {
      paths: ['M5 7h11', 'M5 12h14', 'M5 17h9', 'M17 5v4', 'M11 10v4', 'M15 15v4'],
      circles: [
        { cx: 17, cy: 7, r: 1.2 },
        { cx: 11, cy: 12, r: 1.2 },
        { cx: 15, cy: 17, r: 1.2 }
      ]
    },
    algorithm: {
      paths: [
        'M12 4v4',
        'M12 16v4',
        'M4 12h4',
        'M16 12h4',
        'M7 7l2.5 2.5',
        'M14.5 14.5 17 17',
        'M17 7l-2.5 2.5',
        'M7 17l2.5-2.5'
      ],
      circles: [{ cx: 12, cy: 12, r: 3.2 }]
    },
    system: {
      paths: [
        'M12 3.8v2.4',
        'M12 17.8v2.4',
        'M4.8 7l2.1 1.2',
        'M17.1 15.8 19.2 17',
        'M4.8 17l2.1-1.2',
        'M17.1 8.2 19.2 7',
        'M3.8 12h2.4',
        'M17.8 12h2.4'
      ],
      circles: [{ cx: 12, cy: 12, r: 3.5 }]
    },
    log: { paths: ['M7 4h8l3 3v13H7z', 'M15 4v4h4', 'M9.5 12h5', 'M9.5 16h5'] },
    gateway: {
      paths: ['M4 8h16', 'M6 8V5h12v3', 'M7 16h4', 'M13 16h4', 'M6 8v11', 'M18 8v11'],
      circles: [
        { cx: 9, cy: 12, r: 1.1 },
        { cx: 15, cy: 12, r: 1.1 }
      ]
    },
    model: { paths: ['M12 4 20 8.5v7L12 20 4 15.5v-7z', 'M12 4v7', 'M20 8.5l-8 4.5-8-4.5', 'M12 13v7'] },
    plus: { paths: ['M12 5v14', 'M5 12h14'] },
    health: {
      paths: [
        'M12 20s-7-4.4-7-10a4 4 0 0 1 7-2.4 4 4 0 0 1 7 2.4c0 5.6-7 10-7 10z',
        'M9.5 12h5',
        'M12 9.5v5'
      ]
    },
    edit: { paths: ['M5 19h4l9.5-9.5a2.1 2.1 0 0 0-3-3L6 16z', 'M14.5 7.5l3 3', 'M4 21h16'] },
    time: {
      paths: ['M12 7v5l3 2', 'M5 5.8A9 9 0 1 0 18.2 5.8'],
      circles: [{ cx: 12, cy: 12, r: 8 }]
    },
    image: { paths: ['M5 5h14v14H5z', 'M8 15l3-3 2 2 3-4 3 5', 'M8.5 8.5h.01'] },
    threshold: { paths: ['M5 18h14', 'M6 15l4-4 3 3 5-7', 'M6 7h3', 'M15 17h3'] },
    roi: {
      paths: ['M5 7V5h2', 'M17 5h2v2', 'M19 17v2h-2', 'M7 19H5v-2', 'M8 12h8', 'M12 8v8'],
      circles: [
        { cx: 8, cy: 8, r: 1 },
        { cx: 16, cy: 8, r: 1 },
        { cx: 16, cy: 16, r: 1 },
        { cx: 8, cy: 16, r: 1 }
      ]
    },
    credential: { paths: ['M7 11V8a5 5 0 0 1 10 0v3', 'M6 11h12v9H6z', 'M10 15h4'] },
    eye: {
      paths: ['M3.5 12s3.2-5.5 8.5-5.5S20.5 12 20.5 12s-3.2 5.5-8.5 5.5S3.5 12 3.5 12z'],
      circles: [{ cx: 12, cy: 12, r: 2.5 }]
    },
    filter: { paths: ['M4 6h16', 'M7 12h10', 'M10 18h4'] },
    export: { paths: ['M12 4v10', 'M8.5 10.5 12 14l3.5-3.5', 'M5 20h14'] },
    database: {
      paths: [
        'M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3z',
        'M4 6v6c0 1.7 3.6 3 8 3s8-1.3 8-3V6',
        'M4 12v6c0 1.7 3.6 3 8 3s8-1.3 8-3v-6'
      ]
    },
    search: { paths: ['M20 20l-4.2-4.2'], circles: [{ cx: 10.5, cy: 10.5, r: 6.5 }] },
    trash: { paths: ['M5 7h14', 'M9 7V4h6v3', 'M7 7l1 13h8l1-13', 'M10 11v5', 'M14 11v5'] },
    upload: { paths: ['M12 16V5', 'M8 9l4-4 4 4', 'M5 19h14'] },
    user: { paths: ['M5 20c.6-4 3-6 7-6s6.4 2 7 6'], circles: [{ cx: 12, cy: 8, r: 4 }] },
    vehicle: {
      paths: ['M4 15V10l2-4h12l2 4v5', 'M4 12h16', 'M7 15v2', 'M17 15v2'],
      circles: [
        { cx: 7, cy: 13, r: 1 },
        { cx: 17, cy: 13, r: 1 }
      ]
    },
    workwear: { paths: ['M8 4 4 7l2 4 2-1v10h8V10l2 1 2-4-4-3-2 3h-4z'] },
    ppe: { paths: ['M4 13h16', 'M6 13v-2a6 6 0 0 1 12 0v2', 'M9 11V6', 'M15 11V6', 'M5 16h14'] },
    close: { paths: ['M6 6l12 12', 'M18 6 6 18'] },
    'chevron-left': { paths: ['M15 5l-7 7 7 7'] },
    'chevron-right': { paths: ['M9 5l7 7-7 7'] },
    'chevron-down': { paths: ['M5 9l7 7 7-7'] }
  };

  /** 生成与 AdminIcon 输出一致的 SVG 标记 */
  function icon(name, size, strokeWidth) {
    var def = ICONS[name] || ICONS.system;
    var s = size == null ? 18 : size;
    var sw = strokeWidth == null ? 1.9 : strokeWidth;
    var out =
      '<svg class="admin-icon" width="' +
      s +
      '" height="' +
      s +
      '" viewBox="' +
      (def.viewBox || '0 0 24 24') +
      '" fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="' +
      sw +
      '" aria-hidden="true">';
    (def.paths || []).forEach(function (d) {
      out += '<path d="' + d + '"></path>';
    });
    (def.circles || []).forEach(function (c) {
      out += '<circle cx="' + c.cx + '" cy="' + c.cy + '" r="' + c.r + '"></circle>';
    });
    (def.rects || []).forEach(function (r) {
      out +=
        '<rect x="' + r.x + '" y="' + r.y + '" width="' + r.width + '" height="' + r.height + '"></rect>';
    });
    return out + '</svg>';
  }

  global.ICONS = ICONS;
  global.icon = icon;
})(window);
