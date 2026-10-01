/* Imperivm Reforged asset viewer.
 *
 * Input contract: src/imperivm/manifest.py. Everything drawn here comes from
 * `manifest.json` plus the PNGs beside it. The frame boxes matter: a sprite
 * frame is positioned on a canvas shared by every frame of the sheet, so each
 * frame is blitted at (left, top). Re-centring frames would break the motion.
 */
(function () {
  'use strict';

  var T = window.ImviewTables;
  var PNG = window.ImviewPng;

  var boot;
  try {
    boot = JSON.parse(document.getElementById('imview-data').textContent);
  } catch (e) {
    boot = { assetBase: '.', manifest: null, inlineAssets: {}, exportPath: '', generated: '' };
  }
  boot.inlineAssets = boot.inlineAssets || {};

  var state = {
    manifest: null,
    section: 'sprites',
    query: '',
    factions: {},      // code -> true
    kinds: {},         // kind -> true
    flags: { pc: false, anim: false, paired: false, shadows: false },
    items: [],         // current filtered list
    limit: 200,
    stageBg: 'checker',
    selected: null,
    spriteIndex: {},   // path -> sprite entry
    player: null       // live sprite player, if any
  };

  var $ = function (id) { return document.getElementById(id); };

  /* ------------------------------------------------------------ plumbing */

  function assetUrl(path) {
    if (Object.prototype.hasOwnProperty.call(boot.inlineAssets, path)) return boot.inlineAssets[path];
    var base = boot.assetBase || '.';
    if (base === '.' || base === '') return path;
    return base.replace(/\/+$/, '') + '/' + path;
  }

  function isInlined(path) {
    return Object.prototype.hasOwnProperty.call(boot.inlineAssets, path);
  }

  /* Can we read the raw bytes of this asset? Needed for indexed team colour
     and for keying shadow masks. Data URIs are always readable; a plain
     file:// sibling is not, because fetch refuses cross-origin file reads. */
  function canDecode(path) {
    if (!PNG.hasInflate()) return false;
    if (isInlined(path)) return true;
    return location.protocol !== 'file:';
  }

  var imgCache = new Map();
  function loadImage(path) {
    if (imgCache.has(path)) return imgCache.get(path);
    var p = new Promise(function (resolve, reject) {
      var img = new Image();
      img.onload = function () { resolve(img); };
      img.onerror = function () { reject(new Error('could not load ' + path)); };
      img.src = assetUrl(path);
    });
    imgCache.set(path, p);
    return p;
  }

  var decodeCache = new Map();
  function loadDecoded(path) {
    if (decodeCache.has(path)) return decodeCache.get(path);
    var p = fetch(assetUrl(path)).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + path);
      return r.arrayBuffer();
    }).then(function (buf) {
      return PNG.decode(new Uint8Array(buf));
    });
    decodeCache.set(path, p);
    return p;
  }

  function makeCanvas(w, h) {
    var c = document.createElement('canvas');
    c.width = Math.max(1, w);
    c.height = Math.max(1, h);
    return c;
  }

  function canvasFromImageData(data) {
    var c = makeCanvas(data.width, data.height);
    c.getContext('2d').putImageData(data, 0, 0);
    return c;
  }

  /* Stage backdrop. This art is dark-edged and its shadows are pure black, so
     no single backdrop shows everything: a black shadow is invisible on a dark
     checker, and the sprites' own dark outlines disappear on white. The choice
     is therefore the user's, and it sticks across selections. */
  var STAGE_BACKDROPS = [
    { id: 'checker', label: 'checker' },
    { id: 'dark', label: 'dark' },
    { id: 'mid', label: 'mid' },
    { id: 'light', label: 'light' },
    { id: 'field', label: 'field' }
  ];

  function applyStageBg(canvas) {
    canvas.setAttribute('data-bg', state.stageBg);
  }

  /* A default zoom that keeps the whole canvas on screen. Buildings reach
     1990x1515, which at a fixed 2x would push everything below it off the
     page. Integer factors only, so pixels stay square and crisp. */
  function fitZoom(w, h) {
    var budgetW = 620, budgetH = 480;
    if (w <= 0 || h <= 0) return 2;
    var z = Math.floor(Math.min(budgetW / w, budgetH / h));
    return Math.max(1, Math.min(8, z || 1));
  }

  function stagePanel(canvases) {
    var panel = el('div', { class: 'panel' }, [el('h2', { text: 'Backdrop' })]);
    var row = el('div', { class: 'chips' });
    var buttons = [];
    STAGE_BACKDROPS.forEach(function (b) {
      var btn = el('button', {
        type: 'button',
        class: 'chip',
        'aria-pressed': state.stageBg === b.id ? 'true' : 'false',
        text: b.label
      });
      btn.addEventListener('click', function () {
        state.stageBg = b.id;
        buttons.forEach(function (o) {
          o.setAttribute('aria-pressed', o === btn ? 'true' : 'false');
        });
        canvases.forEach(applyStageBg);
      });
      buttons.push(btn);
      row.appendChild(btn);
    });
    panel.appendChild(row);
    return panel;
  }

  var toastTimer = null;
  function toast(message) {
    var el = $('toast');
    el.textContent = message;
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.hidden = true; }, 6000);
  }

  function el(tag, attrs, children) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === 'class') node.className = attrs[k];
        else if (k === 'text') node.textContent = attrs[k];
        else if (k === 'html') node.innerHTML = attrs[k];
        else if (attrs[k] !== null && attrs[k] !== undefined && attrs[k] !== false) node.setAttribute(k, attrs[k]);
      });
    }
    (children || []).forEach(function (c) { if (c) node.appendChild(c); });
    return node;
  }

  function propList(pairs) {
    var dl = el('dl', { class: 'props' });
    pairs.forEach(function (p) {
      if (p[1] === null || p[1] === undefined) return;
      dl.appendChild(el('div', null, [
        el('dt', { text: p[0] }),
        el('dd', { text: String(p[1]) })
      ]));
    });
    return dl;
  }

  function baseName(path) {
    var parts = String(path).split('/');
    return parts[parts.length - 1].replace(/\.png$/i, '');
  }

  function shortName(path) {
    var parts = String(path).split('/');
    if (parts.length >= 2) return parts.slice(-2).join('/').replace(/\.png$/i, '');
    return baseName(path);
  }

  /* ------------------------------------------------------------- indexing */

  function indexManifest(m) {
    state.manifest = m;
    state.spriteIndex = {};
    (m.sprites || []).forEach(function (s) {
      s._kind = T.classify(s.path);
      s._faction = T.factionOf(s.path);
      s._isShadowSheet = /_shadow\.png$/i.test(s.path) || (s.shadow && /shadow/i.test(s.path));
      s._name = shortName(s.path);
      s._search = (s.path + ' ' + (s.source || '')).toLowerCase();
      s._frameMap = null;
      state.spriteIndex[s.path] = s;
    });
    /* Two shadow-naming conventions live in the retail data. Units carry a
       per-animation `NAME_shadow.png` beside `NAME.png`. Buildings carry one
       bare `shadow.png` for the whole entity, shared by every layer sheet in
       the directory. Both composite by their own boxes on the shared canvas
       (docs/formats/rle.md), so either can be drawn under the body as-is. */
    (m.sprites || []).forEach(function (s) {
      s._shadowPath = null;
      if (s._isShadowSheet) return;
      var candidate = s.path.replace(/\.png$/i, '_shadow.png');
      if (state.spriteIndex[candidate]) {
        s._shadowPath = candidate;
        return;
      }
      var slash = s.path.lastIndexOf('/');
      if (slash > 0) {
        var sibling = s.path.slice(0, slash) + '/shadow.png';
        if (sibling !== s.path && state.spriteIndex[sibling]) {
          s._shadowPath = sibling;
          s._shadowIsShared = true;
        }
      }
    });
    (m.terrain || []).forEach(function (t) {
      t._name = shortName(t.path);
      t._search = (t.path + ' ' + (t.source || '')).toLowerCase();
    });
    (m.fonts || []).forEach(function (f) {
      f._name = shortName(f.path);
      f._search = (f.path + ' ' + (f.source || '') + ' ' + (f.face || '')).toLowerCase();
    });
    (m.masks || []).forEach(function (k) {
      k._name = shortName(k.path);
      k._search = (k.path + ' ' + (k.source || '')).toLowerCase();
    });
  }

  function frameMap(entry) {
    if (!entry._frameMap) {
      var map = {};
      (entry.frames || []).forEach(function (f) { map[f.row + ':' + f.column] = f; });
      entry._frameMap = map;
    }
    return entry._frameMap;
  }

  function sectionItems(section) {
    var m = state.manifest;
    if (!m) return [];
    if (section === 'sprites') return m.sprites || [];
    if (section === 'terrain') return m.terrain || [];
    if (section === 'fonts') return m.fonts || [];
    if (section === 'masks') return m.masks || [];
    return [];
  }

  function matchesQuery(item) {
    if (!state.query) return true;
    var terms = state.query.toLowerCase().split(/\s+/).filter(Boolean);
    for (var i = 0; i < terms.length; i++) {
      if (item._search.indexOf(terms[i]) === -1) return false;
    }
    return true;
  }

  function activeKeys(obj) {
    return Object.keys(obj).filter(function (k) { return obj[k]; });
  }

  function filterItems() {
    var items = sectionItems(state.section).filter(matchesQuery);
    if (state.section === 'sprites') {
      var factions = activeKeys(state.factions);
      var kinds = activeKeys(state.kinds);
      items = items.filter(function (s) {
        if (!state.flags.shadows && s._isShadowSheet) return false;
        if (factions.length && factions.indexOf(s._faction) === -1) return false;
        if (kinds.length && kinds.indexOf(s._kind) === -1) return false;
        if (state.flags.pc && !s.player_color) return false;
        if (state.flags.anim && !(s.rows > 1)) return false;
        if (state.flags.paired && !s._shadowPath) return false;
        return true;
      });
    }
    return items;
  }

  /* ------------------------------------------------------------------ UI */

  function renderTabs() {
    var m = state.manifest || {};
    var counts = {
      sprites: (m.sprites || []).length,
      terrain: (m.terrain || []).length,
      fonts: (m.fonts || []).length,
      masks: (m.masks || []).length
    };
    Array.prototype.forEach.call(document.querySelectorAll('[data-count]'), function (n) {
      n.textContent = counts[n.getAttribute('data-count')];
    });
    Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (t) {
      t.setAttribute('aria-selected', t.getAttribute('data-section') === state.section ? 'true' : 'false');
    });
    var spriteOnly = state.section === 'sprites';
    $('faction-filter').hidden = !spriteOnly;
    $('kind-filter').hidden = !spriteOnly;
    $('flag-filter').hidden = !spriteOnly;
  }

  function chip(label, count, pressed, onToggle) {
    var b = el('button', { type: 'button', class: 'chip', 'aria-pressed': pressed ? 'true' : 'false' }, [
      document.createTextNode(label)
    ]);
    if (count !== null && count !== undefined) {
      b.appendChild(el('span', { class: 'chip-n', text: String(count) }));
    }
    b.addEventListener('click', function () {
      var next = b.getAttribute('aria-pressed') !== 'true';
      b.setAttribute('aria-pressed', next ? 'true' : 'false');
      onToggle(next);
      refreshList();
    });
    return b;
  }

  function renderFilters() {
    var sprites = (state.manifest && state.manifest.sprites) || [];
    var byFaction = {}, byKind = {};
    sprites.forEach(function (s) {
      if (s._isShadowSheet && !state.flags.shadows) return;
      if (s._faction) byFaction[s._faction] = (byFaction[s._faction] || 0) + 1;
      byKind[s._kind] = (byKind[s._kind] || 0) + 1;
    });

    var fc = $('faction-chips');
    fc.textContent = '';
    T.FACTIONS.forEach(function (f) {
      fc.appendChild(chip(f.code + ' ' + f.name, byFaction[f.code] || 0, !!state.factions[f.code], function (on) {
        state.factions[f.code] = on;
      }));
    });

    var kc = $('kind-chips');
    kc.textContent = '';
    T.KINDS.forEach(function (k) {
      if (!byKind[k]) return;
      kc.appendChild(chip(k, byKind[k], !!state.kinds[k], function (on) { state.kinds[k] = on; }));
    });

    var gc = $('flag-chips');
    gc.textContent = '';
    gc.appendChild(chip('player colour', null, state.flags.pc, function (on) { state.flags.pc = on; }));
    gc.appendChild(chip('animated', null, state.flags.anim, function (on) { state.flags.anim = on; }));
    gc.appendChild(chip('has shadow', null, state.flags.paired, function (on) { state.flags.paired = on; }));
    gc.appendChild(chip('incl. shadow sheets', null, state.flags.shadows, function (on) {
      state.flags.shadows = on;
      renderFilters();
    }));
  }

  function itemBadges(item) {
    var out = [];
    if (state.section === 'sprites') {
      if (item.player_color) out.push(['pc', 'badge-pc']);
      if (item._shadowPath) out.push(['shadow', 'badge-shadow']);
      if (item._isShadowSheet) out.push(['mask', 'badge-shadow']);
      if (item.rows > 1) out.push([item.rows + '×' + item.columns, 'badge-anim']);
    } else if (state.section === 'terrain') {
      if (item.frames > 1) out.push([item.frames + ' frames', 'badge-anim']);
    }
    return out;
  }

  function refreshList() {
    state.items = filterItems();
    var list = $('asset-list');
    list.textContent = '';
    var shown = state.items.slice(0, state.limit);
    shown.forEach(function (item, i) {
      var badges = itemBadges(item).map(function (b) {
        return el('span', { class: 'badge ' + b[1], text: b[0] });
      });
      var sub = el('span', { class: 'asset-sub' }, [
        el('span', { text: item._kind || item.path.split('/')[0] })
      ].concat(badges));
      var btn = el('button', {
        type: 'button',
        class: 'asset-item',
        role: 'option',
        'aria-current': state.selected === item ? 'true' : 'false'
      }, [
        el('span', { class: 'asset-name', text: item._name }),
        sub
      ]);
      btn.addEventListener('click', function () { select(item); });
      var li = el('li', null, [btn]);
      li._index = i;
      list.appendChild(li);
    });
    $('list-summary').textContent = state.items.length + ' item' + (state.items.length === 1 ? '' : 's') +
      (state.items.length > shown.length ? ' (' + shown.length + ' shown)' : '');
    $('load-more').hidden = state.items.length <= state.limit;
  }

  function select(item) {
    stopPlayer();
    state.selected = item;
    Array.prototype.forEach.call(document.querySelectorAll('.asset-item'), function (b) {
      b.setAttribute('aria-current', 'false');
    });
    var idx = state.items.indexOf(item);
    var nodes = document.querySelectorAll('.asset-item');
    if (idx >= 0 && nodes[idx]) {
      nodes[idx].setAttribute('aria-current', 'true');
      nodes[idx].scrollIntoView({ block: 'nearest' });
    }
    if (state.section === 'sprites') renderSprite(item);
    else if (state.section === 'terrain') renderTerrain(item);
    else if (state.section === 'fonts') renderFont(item);
    else renderMask(item);
  }

  function stepSelection(delta) {
    if (!state.items.length) return;
    var idx = state.items.indexOf(state.selected);
    idx = Math.max(0, Math.min(state.items.length - 1, (idx < 0 ? 0 : idx + delta)));
    if (idx >= state.limit) { state.limit += 200; refreshList(); }
    select(state.items[idx]);
  }

  function detailHead(item, extraBadges) {
    var badges = el('div', { class: 'detail-badges' });
    (extraBadges || []).forEach(function (b) {
      badges.appendChild(el('span', { class: 'badge ' + (b[1] || ''), text: b[0] }));
    });
    return el('div', { class: 'detail-head' }, [
      el('h1', { class: 'detail-title', text: item.path }),
      el('div', { class: 'detail-source', text: 'source: ' + (item.source || 'unknown') }),
      badges
    ]);
  }

  /* -------------------------------------------------------- sprite player */

  function stopPlayer() {
    if (state.player && state.player.raf) {
      cancelAnimationFrame(state.player.raf);
      state.player.raf = null;
    }
    state.player = null;
  }

  /* Tint one neutral palette colour with a player colour, keeping the
     neutral's shading. The engine's real 64-entry player ramps are not in any
     shipped file (docs/formats/rle.md), so this is an approximation: a
     hard-light of the player colour over the neutral's luminance. */
  function tint(r, g, b, player) {
    var t = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
    var out = [0, 0, 0];
    for (var i = 0; i < 3; i++) {
      var p = player[i];
      out[i] = t < 0.5 ? p * 2 * t : p + (255 - p) * (2 * t - 1);
    }
    return out;
  }

  function recolouredCanvas(decoded, colorId, slots) {
    if (decoded.colorType !== 3 || !decoded.palette) return null;
    var override = decoded.palette.slice();
    if (colorId) {
      var entry = null;
      T.PLAYER_COLORS.forEach(function (c) { if (c.id === colorId) entry = c; });
      if (entry) {
        var n = Math.min(slots || 64, decoded.palette.length);
        for (var i = 0; i < n; i++) {
          var rgb = tint(decoded.palette[i * 3], decoded.palette[i * 3 + 1], decoded.palette[i * 3 + 2], entry.rgb);
          override[i * 3] = rgb[0] | 0;
          override[i * 3 + 1] = rgb[1] | 0;
          override[i * 3 + 2] = rgb[2] | 0;
        }
      }
    }
    return canvasFromImageData(PNG.toRGBA(decoded, override));
  }

  function renderSprite(entry) {
    var detail = $('detail');
    detail.textContent = '';

    var badges = [];
    if (entry.player_color) badges.push(['player colour (' + (entry.player_color_slots || 64) + ' slots)', 'badge-pc']);
    if (entry.indexed) badges.push(['indexed png', '']);
    else badges.push(['flattened rgba', '']);
    if (entry._shadowPath) badges.push(['shadow pair', 'badge-shadow']);
    if (entry._isShadowSheet) badges.push(['shadow mask', 'badge-shadow']);
    detail.appendChild(detailHead(entry, badges));

    var nonEmpty = (entry.frames || []).filter(function (f) { return !f.empty; }).length;
    detail.appendChild(propList([
      ['grid', entry.rows + ' rows × ' + entry.columns + ' columns'],
      ['frames', (entry.frames || []).length + ' (' + nonEmpty + ' non-empty)'],
      ['canvas', entry.canvas_width + ' × ' + entry.canvas_height],
      ['pixel format', entry.pixel_format],
      ['row = ', 'animation frame'],
      ['column = ', 'facing direction']
    ]));

    /* The shadow shares the body's canvas origin but is bounded by its own
       frames, so it is often taller or wider than the body sheet - 556 of the
       888 shadow pairs in the retail export are. Size the stage to the union
       or the shadow's feet get cut off. */
    var shadowEntry = entry._shadowPath ? state.spriteIndex[entry._shadowPath] : null;
    var stageW = Math.max(entry.canvas_width, shadowEntry ? shadowEntry.canvas_width : 0);
    var stageH = Math.max(entry.canvas_height, shadowEntry ? shadowEntry.canvas_height : 0);

    var stage = makeCanvas(stageW, stageH);
    stage.className = 'stage';
    stage.setAttribute('role', 'img');
    stage.setAttribute('aria-label', 'sprite preview for ' + entry.path);
    applyStageBg(stage);
    var stageWrap = el('div', { class: 'stage-wrap' }, [stage]);

    var controls = el('div', null, []);
    var sheetPanel = el('div', { class: 'panel' }, [el('h2', { text: 'Frame grid' })]);
    var framePanel = el('div', { class: 'panel' }, [el('h2', { text: 'Frame boxes (current column)' })]);

    var workspace = el('div', { class: 'workspace' }, [
      el('div', null, [stageWrap, sheetPanel, framePanel]),
      controls
    ]);
    detail.appendChild(workspace);

    var p = {
      entry: entry,
      row: 0,
      col: entry.columns > 1 ? Math.min(0, entry.columns - 1) : 0,
      playing: entry.rows > 1,
      fps: 12,
      zoom: fitZoom(stageW, stageH),
      axis: 'row',
      shadow: true,
      box: false,
      colorId: null,
      shadowOpacity: 0.55,
      bodySource: null,      // canvas or <img>
      shadowSource: null,
      decoded: null,
      last: 0,
      raf: null,
      stage: stage,
      hueFallback: false
    };
    state.player = p;

    /* ---- controls ---- */

    var transport = el('div', { class: 'panel' }, [el('h2', { text: 'Transport' })]);
    var playBtn = el('button', { type: 'button', class: 'btn', 'aria-pressed': p.playing ? 'true' : 'false', text: p.playing ? 'Pause' : 'Play' });
    playBtn.addEventListener('click', function () { setPlaying(!p.playing); });

    var fpsOut = el('span', { class: 'ctl-val', text: p.fps + ' fps' });
    var fps = el('input', { type: 'range', min: '1', max: '60', value: String(p.fps), id: 'ctl-fps' });
    fps.addEventListener('input', function () { p.fps = +fps.value; fpsOut.textContent = p.fps + ' fps'; });

    var rowOut = el('span', { class: 'ctl-val', text: '0 / ' + (entry.rows - 1) });
    var rowCtl = el('input', { type: 'range', min: '0', max: String(Math.max(0, entry.rows - 1)), value: '0', id: 'ctl-row' });
    rowCtl.addEventListener('input', function () { p.row = +rowCtl.value; setPlaying(false); draw(); });

    var colOut = el('span', { class: 'ctl-val', text: '0 / ' + (entry.columns - 1) });
    var colCtl = el('input', { type: 'range', min: '0', max: String(Math.max(0, entry.columns - 1)), value: '0', id: 'ctl-col' });
    colCtl.addEventListener('input', function () { p.col = +colCtl.value; draw(); });

    var axisSel = el('select', { id: 'ctl-axis' }, [
      el('option', { value: 'row', text: 'rows (animation frames)' }),
      el('option', { value: 'column', text: 'columns (facings)' })
    ]);
    axisSel.value = p.axis;
    axisSel.addEventListener('change', function () { p.axis = axisSel.value; });

    var zoomOut = el('span', { class: 'ctl-val', text: p.zoom + '×' });
    var zoom = el('input', { type: 'range', min: '1', max: '8', step: '1', value: String(p.zoom), id: 'ctl-zoom' });
    zoom.addEventListener('input', function () { setZoom(+zoom.value); });

    transport.appendChild(el('div', { class: 'ctl-row' }, [
      playBtn,
      el('label', { for: 'ctl-fps', text: 'rate' }), fps, fpsOut
    ]));
    transport.appendChild(el('div', { class: 'ctl-row' }, [
      el('label', { for: 'ctl-axis', text: 'play along' }), axisSel
    ]));
    transport.appendChild(el('div', { class: 'ctl-row' }, [
      el('label', { for: 'ctl-row', text: 'frame' }), rowCtl, rowOut
    ]));
    transport.appendChild(el('div', { class: 'ctl-row' }, [
      el('label', { for: 'ctl-col', text: 'facing' }), colCtl, colOut
    ]));
    transport.appendChild(el('div', { class: 'ctl-row' }, [
      el('label', { for: 'ctl-zoom', text: 'zoom' }), zoom, zoomOut
    ]));
    var slotHint = T.ANIM_SLOTS;
    transport.appendChild(el('p', {
      class: 'note',
      text: 'Animations are addressed by numeric slot elsewhere in the data ' +
            '(13 ' + slotHint[13] + ', 9 ' + slotHint[9] + ', 5 ' + slotHint[5] + ', 19 ' + slotHint[19] +
            '). A sheet holds one animation; the slot lives in the entity XML, not in the manifest.'
    }));
    controls.appendChild(transport);

    /* direction dial */
    if (entry.columns === 8) {
      var dialPanel = el('div', { class: 'panel' }, [el('h2', { text: 'Facing' })]);
      var dial = el('div', { class: 'dial' });
      var order = [3, 4, 5, 2, null, 6, 1, 0, 7]; // NW N NE / W . E / SW S SE
      var dialButtons = {};
      order.forEach(function (c) {
        if (c === null) {
          var centre = el('button', { type: 'button', class: 'dial-centre', disabled: 'disabled', text: 'col' });
          dial.appendChild(centre);
          p.dialCentre = centre;
          return;
        }
        var b = el('button', { type: 'button', text: String(c), 'aria-pressed': 'false', 'aria-label': 'facing column ' + c });
        b.addEventListener('click', function () { p.col = c; colCtl.value = String(c); draw(); });
        dialButtons[c] = b;
        dial.appendChild(b);
      });
      p.dialButtons = dialButtons;
      dialPanel.appendChild(dial);
      dialPanel.appendChild(el('p', {
        class: 'note',
        text: 'Columns are the eight facings. Column 0 is the heading (0, +1) \u2014 towards the ' +
              'viewer, down the screen, which is the dir=(0,1) every map object is authored with \u2014 ' +
              'and the index increases towards world -x, so 2 is west, 4 is north and 6 is east. ' +
              'Read off the walk sheets of BOAR and CNUMIDIANRIDER, which show the animal head-on at 0, ' +
              'in left profile a quarter of the way round, and from behind at the halfway column.'
      }));
      controls.appendChild(dialPanel);
    }

    /* layers */
    var layers = el('div', { class: 'panel' }, [el('h2', { text: 'Layers' })]);
    var toggles = el('div', { class: 'ctl-toggles' });
    function toggle(labelText, initial, onChange) {
      var input = el('input', { type: 'checkbox' });
      input.checked = initial;
      input.addEventListener('change', function () { onChange(input.checked); draw(); });
      return el('label', null, [input, document.createTextNode(labelText)]);
    }
    var shadowToggle = toggle('shadow', p.shadow, function (v) { p.shadow = v; });
    if (!entry._shadowPath) {
      shadowToggle.querySelector('input').disabled = true;
      shadowToggle.querySelector('input').checked = false;
      p.shadow = false;
    }
    toggles.appendChild(shadowToggle);
    toggles.appendChild(toggle('frame box', p.box, function (v) { p.box = v; }));
    toggles.appendChild(toggle('canvas edge', true, function (v) { p.edge = v; }));
    p.edge = true;
    layers.appendChild(toggles);
    if (entry._shadowPath) {
      var opOut = el('span', { class: 'ctl-val', text: Math.round(p.shadowOpacity * 100) + '%' });
      var opCtl = el('input', {
        type: 'range', min: '0', max: '100', value: String(Math.round(p.shadowOpacity * 100)), id: 'ctl-shadow-op'
      });
      opCtl.addEventListener('input', function () {
        p.shadowOpacity = +opCtl.value / 100;
        opOut.textContent = opCtl.value + '%';
        draw();
      });
      layers.appendChild(el('div', { class: 'ctl-row' }, [
        el('label', { for: 'ctl-shadow-op', text: 'shadow' }), opCtl, opOut
      ]));
      layers.appendChild(el('p', {
        class: 'note',
        text: 'shadow sheet: ' + entry._shadowPath +
          (entry._shadowIsShared
            ? ' — one shadow shared by every layer of this building, not a per-sheet pair'
            : '')
      }));
    } else if (!entry._isShadowSheet) {
      layers.appendChild(el('p', { class: 'note', text: 'no shadow sheet for this one: neither a _shadow twin nor a shadow.png beside it' }));
    }
    controls.appendChild(layers);
    controls.appendChild(stagePanel([stage]));

    /* team colour */
    if (entry.player_color) {
      var tc = el('div', { class: 'panel' }, [el('h2', { text: 'Team colour' })]);
      var sw = el('div', { class: 'swatches' });
      var none = el('button', { type: 'button', class: 'swatch swatch-none', 'aria-pressed': 'true', text: 'neutral (as shipped)' });
      var swatchButtons = [none];
      none.addEventListener('click', function () { setColor(null); });
      sw.appendChild(none);
      T.PLAYER_COLORS.forEach(function (c) {
        var b = el('button', {
          type: 'button',
          class: 'swatch',
          'aria-pressed': 'false',
          title: 'id' + c.id + ' — ' + c.name + ' rgb(' + c.rgb.join(', ') + ')',
          'aria-label': 'player colour ' + c.id + ' ' + c.name
        });
        b.style.background = 'rgb(' + c.rgb.join(',') + ')';
        b.addEventListener('click', function () { setColor(c.id); });
        swatchButtons.push(b);
        sw.appendChild(b);
      });
      p.swatchButtons = swatchButtons;
      tc.appendChild(sw);
      tc.appendChild(el('p', { class: 'note', text: 'DATA\\CONST.INI [PlayerColors]: twelve player ids plus id13, the neutral grey.' }));
      p.colourNote = el('p', { class: 'note', text: '' });
      tc.appendChild(p.colourNote);
      controls.appendChild(tc);
    }

    function setColor(id) {
      p.colorId = id;
      if (p.swatchButtons) {
        p.swatchButtons[0].setAttribute('aria-pressed', id === null ? 'true' : 'false');
        T.PLAYER_COLORS.forEach(function (c, i) {
          p.swatchButtons[i + 1].setAttribute('aria-pressed', c.id === id ? 'true' : 'false');
        });
      }
      applyColour();
    }

    function setZoom(z) {
      p.zoom = Math.max(1, Math.min(8, z));
      zoom.value = String(p.zoom);
      zoomOut.textContent = p.zoom + '×';
      stage.style.width = (stageW * p.zoom) + 'px';
      stage.style.height = (stageH * p.zoom) + 'px';
    }

    function setPlaying(v) {
      p.playing = v;
      playBtn.textContent = v ? 'Pause' : 'Play';
      playBtn.setAttribute('aria-pressed', v ? 'true' : 'false');
      if (v) { p.last = 0; tick(0); }
      else if (p.raf) { cancelAnimationFrame(p.raf); p.raf = null; }
    }
    p.setPlaying = setPlaying;
    p.setZoom = setZoom;
    p.rowCtl = rowCtl;
    p.colCtl = colCtl;
    p.rowOut = rowOut;
    p.colOut = colOut;

    setZoom(p.zoom);

    /* ---- sources ---- */

    function applyColour() {
      if (!p.decoded) {
        p.hueFallback = true;
        if (p.colourNote) {
          p.colourNote.className = 'note note-warn';
          p.colourNote.textContent = !entry.indexed
            ? 'This sheet was exported as flattened RGBA, so palette indices 0–63 are gone. ' +
              'Falling back to a hue rotation over the whole sprite, which is not what the engine does.'
            : 'Pixel data could not be read (' + (PNG.hasInflate() ? 'file:// blocks reading sibling files; use imview serve or --inline' : 'this browser has no DecompressionStream') + '). ' +
              'Falling back to a hue rotation over the whole sprite.';
        }
        draw();
        return;
      }
      p.hueFallback = false;
      var c = recolouredCanvas(p.decoded, p.colorId, entry.player_color_slots || 64);
      if (c) {
        p.bodySource = c;
        if (p.colourNote) {
          p.colourNote.className = 'note';
          p.colourNote.textContent = 'Indices 0–' + ((entry.player_color_slots || 64) - 1) +
            ' remapped on the palette, exactly the swap the engine performs. Shading is taken from the shipped neutral entries; ' +
            'the engine’s real per-player ramps are not stored in any asset file.';
        }
      }
      draw();
    }

    var wantDecode = canDecode(entry.path) && entry.indexed;
    var bodyReady;
    if (wantDecode) {
      bodyReady = loadDecoded(entry.path).then(function (dec) {
        p.decoded = dec;
        p.bodySource = recolouredCanvas(dec, null, entry.player_color_slots || 64) || canvasFromImageData(PNG.toRGBA(dec));
      }).catch(function (err) {
        toast('Falling back to <img> for ' + entry.path + ': ' + err.message);
        return loadImage(entry.path).then(function (img) { p.bodySource = img; });
      });
    } else {
      bodyReady = loadImage(entry.path).then(function (img) { p.bodySource = img; });
    }

    var shadowReady = Promise.resolve();
    if (entry._shadowPath) {
      var sp = entry._shadowPath;
      if (canDecode(sp)) {
        shadowReady = loadDecoded(sp).then(function (dec) {
          p.shadowSource = canvasFromImageData(PNG.shadowToRGBA(dec, [0, 0, 0]));
          p.shadowKeyed = true;
        }).catch(function () {
          return loadImage(sp).then(function (img) { p.shadowSource = img; p.shadowKeyed = false; });
        });
      } else {
        shadowReady = loadImage(sp).then(function (img) { p.shadowSource = img; p.shadowKeyed = false; });
      }
    }

    Promise.all([bodyReady, shadowReady]).then(function () {
      if (state.player !== p) return;
      if (entry.player_color) applyColour();
      renderSheet(p, sheetPanel);
      renderFrameTable(p, framePanel);
      draw();
      if (p.playing) setPlaying(true);
    }).catch(function (err) {
      toast('Could not display ' + entry.path + ': ' + err.message);
    });

    /* ---- drawing ---- */

    function blit(ctx, source, frame, srcEntry) {
      if (!source || !frame || frame.empty || !frame.width || !frame.height) return;
      ctx.drawImage(source, frame.x, frame.y, frame.width, frame.height,
                    frame.left, frame.top, frame.width, frame.height);
    }

    function draw() {
      if (state.player !== p) return;
      var ctx = stage.getContext('2d');
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, stage.width, stage.height);
      ctx.imageSmoothingEnabled = false;

      var bodyFrame = frameMap(entry)[p.row + ':' + p.col];

      if (p.shadow && p.shadowSource && entry._shadowPath) {
        var se = state.spriteIndex[entry._shadowPath];
        /* A building's single shared shadow has a 1x1 grid while its body
           sheet may animate, so clamp rather than miss the frame entirely. */
        var sr = se.rows > 0 ? Math.min(p.row, se.rows - 1) : 0;
        var sc = se.columns > 0 ? Math.min(p.col, se.columns - 1) : 0;
        var sf = frameMap(se)[sr + ':' + sc];
        ctx.save();
        ctx.globalAlpha = p.shadowKeyed ? p.shadowOpacity : 0.35;
        if (!p.shadowKeyed) ctx.globalCompositeOperation = 'multiply';
        blit(ctx, p.shadowSource, sf, se);
        ctx.restore();
      }

      ctx.save();
      if (p.hueFallback && p.colorId) {
        var pc = null;
        T.PLAYER_COLORS.forEach(function (c) { if (c.id === p.colorId) pc = c; });
        if (pc) {
          var hue = Math.round(rgbToHue(pc.rgb));
          ctx.filter = 'hue-rotate(' + hue + 'deg) saturate(1.4)';
        }
      }
      blit(ctx, p.bodySource, bodyFrame, entry);
      ctx.restore();

      if (p.box && bodyFrame && !bodyFrame.empty) {
        ctx.strokeStyle = 'rgba(224,165,74,0.9)';
        ctx.lineWidth = 1;
        ctx.strokeRect(bodyFrame.left + 0.5, bodyFrame.top + 0.5, bodyFrame.width - 1, bodyFrame.height - 1);
      }
      if (p.edge) {
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'rgba(120,130,145,0.45)';
        ctx.strokeRect(0.5, 0.5, entry.canvas_width - 1, entry.canvas_height - 1);
        if (shadowEntry && (shadowEntry.canvas_width !== entry.canvas_width ||
                            shadowEntry.canvas_height !== entry.canvas_height)) {
          ctx.strokeStyle = 'rgba(111,182,232,0.35)';
          ctx.strokeRect(0.5, 0.5, shadowEntry.canvas_width - 1, shadowEntry.canvas_height - 1);
        }
      }

      p.rowOut.textContent = p.row + ' / ' + (entry.rows - 1);
      p.colOut.textContent = p.col + ' / ' + (entry.columns - 1);
      p.rowCtl.value = String(p.row);
      p.colCtl.value = String(p.col);
      if (p.dialButtons) {
        Object.keys(p.dialButtons).forEach(function (k) {
          p.dialButtons[k].setAttribute('aria-pressed', (+k === p.col) ? 'true' : 'false');
        });
        if (p.dialCentre) p.dialCentre.textContent = String(p.col);
      }
      highlightFrameRow(p);
      highlightSheet(p);
    }
    p.draw = draw;

    function tick(now) {
      if (state.player !== p || !p.playing) return;
      p.raf = requestAnimationFrame(tick);
      if (!p.last) p.last = now;
      var interval = 1000 / p.fps;
      if (now - p.last < interval) return;
      p.last = now - ((now - p.last) % interval);
      if (p.axis === 'row') p.row = (p.row + 1) % Math.max(1, entry.rows);
      else p.col = (p.col + 1) % Math.max(1, entry.columns);
      draw();
    }
    p.tick = tick;
  }

  function rgbToHue(rgb) {
    var r = rgb[0] / 255, g = rgb[1] / 255, b = rgb[2] / 255;
    var max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    if (!d) return 0;
    var h;
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    return h * 60;
  }

  /* contact sheet of every frame, each drawn at its own box position so the
     motion across the canvas is visible in the grid itself */
  function renderSheet(p, panel) {
    var entry = p.entry;
    var cells = entry.rows * entry.columns;
    var cap = 400;
    var cell = 56;
    var scale = Math.min(cell / Math.max(1, entry.canvas_width), cell / Math.max(1, entry.canvas_height));
    var rows = Math.min(entry.rows, Math.ceil(cap / Math.max(1, entry.columns)));
    var c = makeCanvas(entry.columns * cell, rows * cell);
    c.className = 'sheet';
    var ctx = c.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    var map = frameMap(entry);
    for (var r = 0; r < rows; r++) {
      for (var col = 0; col < entry.columns; col++) {
        var ox = col * cell, oy = r * cell;
        ctx.fillStyle = ((r + col) % 2) ? '#14161a' : '#101216';
        ctx.fillRect(ox, oy, cell, cell);
        var f = map[r + ':' + col];
        if (!f || f.empty || !f.width) continue;
        ctx.save();
        ctx.translate(ox, oy);
        ctx.scale(scale, scale);
        ctx.drawImage(p.bodySource, f.x, f.y, f.width, f.height, f.left, f.top, f.width, f.height);
        ctx.restore();
      }
    }
    p.sheetCanvas = c;
    p.sheetCell = cell;
    p.sheetRows = rows;

    var overlay = makeCanvas(c.width, c.height);
    overlay.className = 'sheet';
    overlay.style.position = 'absolute';
    overlay.style.left = '0';
    overlay.style.top = '0';
    overlay.style.pointerEvents = 'none';
    p.sheetOverlay = overlay;

    var holder = el('div', { class: 'sheet-wrap' });
    var inner = el('div');
    inner.style.position = 'relative';
    inner.style.width = c.width + 'px';
    inner.style.height = c.height + 'px';
    inner.appendChild(c);
    inner.appendChild(overlay);
    holder.appendChild(inner);
    c.addEventListener('click', function (ev) {
      var rect = c.getBoundingClientRect();
      var col = Math.floor((ev.clientX - rect.left) / cell);
      var row = Math.floor((ev.clientY - rect.top) / cell);
      if (col < 0 || col >= entry.columns || row < 0 || row >= rows) return;
      p.row = row; p.col = col;
      p.setPlaying(false);
      p.draw();
    });
    panel.appendChild(holder);
    if (rows < entry.rows) {
      panel.appendChild(el('p', { class: 'note', text: 'showing the first ' + rows + ' of ' + entry.rows + ' rows' }));
    }
    panel.appendChild(el('p', { class: 'note', text: 'Each cell draws the frame at its own bounding box inside the shared canvas, so drift across the grid is real motion. Click a cell to jump to it.' }));
  }

  function highlightSheet(p) {
    if (!p.sheetOverlay) return;
    var ctx = p.sheetOverlay.getContext('2d');
    ctx.clearRect(0, 0, p.sheetOverlay.width, p.sheetOverlay.height);
    if (p.row >= p.sheetRows) return;
    ctx.strokeStyle = '#e0a54a';
    ctx.lineWidth = 2;
    ctx.strokeRect(p.col * p.sheetCell + 1, p.row * p.sheetCell + 1, p.sheetCell - 2, p.sheetCell - 2);
  }

  function renderFrameTable(p, panel) {
    var wrap = el('div', { class: 'sheet-wrap' });
    var table = el('table', { class: 'frame-table' });
    var head = el('tr', null, [
      el('th', { text: 'row' }), el('th', { text: 'left' }), el('th', { text: 'top' }),
      el('th', { text: 'w' }), el('th', { text: 'h' }), el('th', { text: 'sheet x' }), el('th', { text: 'sheet y' })
    ]);
    table.appendChild(el('thead', null, [head]));
    var body = el('tbody');
    table.appendChild(body);
    wrap.appendChild(table);
    panel.appendChild(wrap);
    panel.appendChild(el('p', { class: 'note', text: 'left/top are absolute coordinates on the shared canvas; sheet x/y locate the pixels inside the exported PNG.' }));
    p.frameTableBody = body;
    p.frameTableCol = -1;
  }

  function highlightFrameRow(p) {
    if (!p.frameTableBody) return;
    var entry = p.entry;
    if (p.frameTableCol !== p.col) {
      p.frameTableCol = p.col;
      p.frameTableBody.textContent = '';
      var map = frameMap(entry);
      for (var r = 0; r < entry.rows; r++) {
        var f = map[r + ':' + p.col];
        var tr = el('tr', null, [
          el('td', { text: String(r) }),
          el('td', { text: f ? String(f.left) : '—' }),
          el('td', { text: f ? String(f.top) : '—' }),
          el('td', { text: f ? String(f.width) : '—' }),
          el('td', { text: f ? String(f.height) : '—' }),
          el('td', { text: f ? String(f.x) : '—' }),
          el('td', { text: f ? String(f.y) : '—' })
        ]);
        if (f && f.empty) tr.className = 'empty-frame';
        p.frameTableBody.appendChild(tr);
      }
    }
    Array.prototype.forEach.call(p.frameTableBody.children, function (tr, i) {
      tr.classList.toggle('is-current', i === p.row);
    });
  }

  /* ------------------------------------------------------------- terrain */

  function renderTerrain(entry) {
    var detail = $('detail');
    detail.textContent = '';
    var animated = (entry.frames || 1) > 1;
    detail.appendChild(detailHead(entry, animated ? [[entry.frames + ' frames', 'badge-anim']] : []));
    var fh = entry.frame_height || (animated ? Math.floor(entry.height / entry.frames) : entry.height);
    detail.appendChild(propList([
      ['size', entry.width + ' × ' + entry.height],
      ['frames', entry.frames || 1],
      ['frame height', animated ? fh : '—'],
      ['strip', animated ? 'vertical filmstrip' : 'single tile']
    ]));

    var stage = makeCanvas(64, 64);
    stage.className = 'stage';
    stage.setAttribute('role', 'img');
    stage.setAttribute('aria-label', 'tiled terrain preview for ' + entry.path);
    applyStageBg(stage);
    var controls = el('div');
    detail.appendChild(el('div', { class: 'workspace' }, [
      el('div', { class: 'stage-wrap' }, [stage]),
      controls
    ]));

    var st = { tiles: 2, zoom: 1, frame: 0, fps: 10, playing: animated, raf: null, img: null };
    state.player = st;

    var panel = el('div', { class: 'panel' }, [el('h2', { text: 'Tiling' })]);
    var tilesOut = el('span', { class: 'ctl-val', text: '2 × 2' });
    var tiles = el('input', { type: 'range', min: '1', max: '6', value: '2', id: 'ctl-tiles' });
    tiles.addEventListener('input', function () {
      st.tiles = +tiles.value;
      tilesOut.textContent = st.tiles + ' × ' + st.tiles;
      resize(); paint();
    });
    var zoomOut = el('span', { class: 'ctl-val', text: '100%' });
    var zoom = el('input', { type: 'range', min: '25', max: '200', step: '25', value: '100', id: 'ctl-tzoom' });
    zoom.addEventListener('input', function () {
      st.zoom = +zoom.value / 100;
      zoomOut.textContent = zoom.value + '%';
      resize(); paint();
    });
    panel.appendChild(el('div', { class: 'ctl-row' }, [el('label', { for: 'ctl-tiles', text: 'repeat' }), tiles, tilesOut]));
    panel.appendChild(el('div', { class: 'ctl-row' }, [el('label', { for: 'ctl-tzoom', text: 'zoom' }), zoom, zoomOut]));
    controls.appendChild(panel);
    controls.appendChild(stagePanel([stage]));

    if (animated) {
      var apanel = el('div', { class: 'panel' }, [el('h2', { text: 'Animation' })]);
      var playBtn = el('button', { type: 'button', class: 'btn', 'aria-pressed': 'true', text: 'Pause' });
      var fpsOut = el('span', { class: 'ctl-val', text: '10 fps' });
      var fps = el('input', { type: 'range', min: '1', max: '30', value: '10', id: 'ctl-tfps' });
      fps.addEventListener('input', function () { st.fps = +fps.value; fpsOut.textContent = st.fps + ' fps'; });
      var frameOut = el('span', { class: 'ctl-val', text: '0 / ' + (entry.frames - 1) });
      var frameCtl = el('input', { type: 'range', min: '0', max: String(entry.frames - 1), value: '0', id: 'ctl-tframe' });
      frameCtl.addEventListener('input', function () { st.frame = +frameCtl.value; setPlay(false); paint(); });
      function setPlay(v) {
        st.playing = v;
        playBtn.textContent = v ? 'Pause' : 'Play';
        playBtn.setAttribute('aria-pressed', v ? 'true' : 'false');
        if (v) { st.last = 0; tick(0); } else if (st.raf) { cancelAnimationFrame(st.raf); st.raf = null; }
      }
      playBtn.addEventListener('click', function () { setPlay(!st.playing); });
      st.setPlaying = setPlay;
      apanel.appendChild(el('div', { class: 'ctl-row' }, [playBtn, el('label', { for: 'ctl-tfps', text: 'rate' }), fps, fpsOut]));
      apanel.appendChild(el('div', { class: 'ctl-row' }, [el('label', { for: 'ctl-tframe', text: 'frame' }), frameCtl, frameOut]));
      apanel.appendChild(el('p', { class: 'note', text: 'Animated water is a vertical filmstrip: ' + entry.frames + ' frames of ' + entry.width + ' × ' + fh + '. The manifest carries the frame count and height; the playback rate is yours to choose.' }));
      controls.appendChild(apanel);
      st.frameOut = frameOut;
      st.frameCtl = frameCtl;
      function tick(now) {
        if (state.player !== st || !st.playing) return;
        st.raf = requestAnimationFrame(tick);
        if (!st.last) st.last = now;
        var interval = 1000 / st.fps;
        if (now - st.last < interval) return;
        st.last = now - ((now - st.last) % interval);
        st.frame = (st.frame + 1) % entry.frames;
        paint();
      }
      st.tick = tick;
    }

    function resize() {
      stage.width = Math.max(1, Math.round(entry.width * st.tiles * st.zoom));
      stage.height = Math.max(1, Math.round(fh * st.tiles * st.zoom));
      stage.style.width = stage.width + 'px';
      stage.style.height = stage.height + 'px';
    }

    function paint() {
      if (state.player !== st || !st.img) return;
      var ctx = stage.getContext('2d');
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, stage.width, stage.height);
      ctx.imageSmoothingEnabled = false;
      ctx.scale(st.zoom, st.zoom);
      var sy = animated ? st.frame * fh : 0;
      for (var ty = 0; ty < st.tiles; ty++) {
        for (var tx = 0; tx < st.tiles; tx++) {
          ctx.drawImage(st.img, 0, sy, entry.width, fh, tx * entry.width, ty * fh, entry.width, fh);
        }
      }
      if (st.frameOut) {
        st.frameOut.textContent = st.frame + ' / ' + (entry.frames - 1);
        st.frameCtl.value = String(st.frame);
      }
    }

    resize();
    loadImage(entry.path).then(function (img) {
      if (state.player !== st) return;
      st.img = img;
      paint();
      if (st.playing && st.tick) st.tick(0);
    }).catch(function (err) { toast(err.message); });
  }

  /* --------------------------------------------------------------- fonts */

  function renderFont(entry) {
    var detail = $('detail');
    detail.textContent = '';
    detail.appendChild(detailHead(entry, [[entry.face || 'font', '']]));
    detail.appendChild(propList([
      ['face', entry.face],
      ['point size', entry.point_size],
      ['line height', entry.height],
      ['ascent', entry.ascent],
      ['descent', entry.descent],
      ['glyphs', entry.glyphs]
    ]));

    var stage = makeCanvas(16, 16);
    stage.className = 'stage';
    stage.setAttribute('role', 'img');
    stage.setAttribute('aria-label', 'font sheet for ' + entry.path);
    applyStageBg(stage);
    var controls = el('div');
    detail.appendChild(el('div', { class: 'workspace' }, [
      el('div', { class: 'stage-wrap' }, [stage]),
      controls
    ]));

    var st = { zoom: 2, img: null, guides: false };
    state.player = st;

    var panel = el('div', { class: 'panel' }, [el('h2', { text: 'View' })]);
    var zoomOut = el('span', { class: 'ctl-val', text: '2×' });
    var zoom = el('input', { type: 'range', min: '1', max: '8', value: '2', id: 'ctl-fzoom' });
    zoom.addEventListener('input', function () { st.zoom = +zoom.value; zoomOut.textContent = st.zoom + '×'; paint(); });
    panel.appendChild(el('div', { class: 'ctl-row' }, [el('label', { for: 'ctl-fzoom', text: 'zoom' }), zoom, zoomOut]));
    var guideInput = el('input', { type: 'checkbox' });
    guideInput.addEventListener('change', function () { st.guides = guideInput.checked; paint(); });
    panel.appendChild(el('div', { class: 'ctl-toggles' }, [
      el('label', null, [guideInput, document.createTextNode('baseline guide')])
    ]));
    panel.appendChild(el('p', {
      class: 'note',
      text: 'The manifest gives the metrics but no per-glyph boxes, so the sheet is shown as exported. ' +
            'The baseline guide simply rules a line every ' + entry.height + ' px with the ascent marked, which only lines up if the exporter packed one glyph row per line height.'
    }));
    controls.appendChild(panel);
    controls.appendChild(stagePanel([stage]));

    function paint() {
      if (state.player !== st || !st.img) return;
      stage.width = st.img.naturalWidth;
      stage.height = st.img.naturalHeight;
      stage.style.width = (stage.width * st.zoom) + 'px';
      stage.style.height = (stage.height * st.zoom) + 'px';
      var ctx = stage.getContext('2d');
      ctx.clearRect(0, 0, stage.width, stage.height);
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(st.img, 0, 0);
      if (st.guides && entry.height > 0) {
        for (var y = 0; y < stage.height; y += entry.height) {
          ctx.fillStyle = 'rgba(111,182,232,0.35)';
          ctx.fillRect(0, y, stage.width, 1);
          if (entry.ascent) {
            ctx.fillStyle = 'rgba(224,165,74,0.35)';
            ctx.fillRect(0, y + entry.ascent, stage.width, 1);
          }
        }
      }
    }
    loadImage(entry.path).then(function (img) {
      if (state.player !== st) return;
      st.img = img; paint();
    }).catch(function (err) { toast(err.message); });
  }

  /* --------------------------------------------------------------- masks */

  function renderMask(entry) {
    var detail = $('detail');
    detail.textContent = '';
    detail.appendChild(detailHead(entry, [[entry.bits_per_cell + ' bit/cell', '']]));
    detail.appendChild(propList([
      ['cells', entry.cells_x + ' × ' + entry.cells_y],
      ['cell size', entry.cell_size + ' px'],
      ['bits per cell', entry.bits_per_cell],
      ['world size', (entry.cells_x * entry.cell_size) + ' × ' + (entry.cells_y * entry.cell_size) + ' px']
    ]));

    var stage = makeCanvas(16, 16);
    stage.className = 'stage';
    stage.setAttribute('role', 'img');
    stage.setAttribute('aria-label', 'passability mask for ' + entry.path);
    applyStageBg(stage);
    var controls = el('div');
    detail.appendChild(el('div', { class: 'workspace' }, [
      el('div', { class: 'stage-wrap' }, [stage]),
      controls
    ]));

    var st = { zoom: 4, img: null, grid: false };
    state.player = st;

    var panel = el('div', { class: 'panel' }, [el('h2', { text: 'View' })]);
    var zoomOut = el('span', { class: 'ctl-val', text: '4×' });
    var zoom = el('input', { type: 'range', min: '1', max: '16', value: '4', id: 'ctl-mzoom' });
    zoom.addEventListener('input', function () { st.zoom = +zoom.value; zoomOut.textContent = st.zoom + '×'; paint(); });
    panel.appendChild(el('div', { class: 'ctl-row' }, [el('label', { for: 'ctl-mzoom', text: 'zoom' }), zoom, zoomOut]));
    var gridInput = el('input', { type: 'checkbox' });
    gridInput.addEventListener('change', function () { st.grid = gridInput.checked; paint(); });
    panel.appendChild(el('div', { class: 'ctl-toggles' }, [
      el('label', null, [gridInput, document.createTextNode('cell grid')])
    ]));
    panel.appendChild(el('p', { class: 'note', text: 'One image pixel per mask cell; each cell covers ' + entry.cell_size + ' world pixels.' }));
    controls.appendChild(panel);
    controls.appendChild(stagePanel([stage]));

    function paint() {
      if (state.player !== st || !st.img) return;
      var w = st.img.naturalWidth, h = st.img.naturalHeight;
      stage.width = w * st.zoom;
      stage.height = h * st.zoom;
      stage.style.width = stage.width + 'px';
      stage.style.height = stage.height + 'px';
      var ctx = stage.getContext('2d');
      ctx.clearRect(0, 0, stage.width, stage.height);
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(st.img, 0, 0, w, h, 0, 0, stage.width, stage.height);
      if (st.grid && st.zoom >= 4) {
        ctx.strokeStyle = 'rgba(111,182,232,0.25)';
        ctx.lineWidth = 1;
        for (var x = 0; x <= w; x++) {
          ctx.beginPath(); ctx.moveTo(x * st.zoom + 0.5, 0); ctx.lineTo(x * st.zoom + 0.5, stage.height); ctx.stroke();
        }
        for (var y = 0; y <= h; y++) {
          ctx.beginPath(); ctx.moveTo(0, y * st.zoom + 0.5); ctx.lineTo(stage.width, y * st.zoom + 0.5); ctx.stroke();
        }
      }
    }
    loadImage(entry.path).then(function (img) {
      if (state.player !== st) return;
      st.img = img; paint();
    }).catch(function (err) { toast(err.message); });
  }

  /* ------------------------------------------------------------ keyboard */

  function onKey(ev) {
    var tag = (ev.target.tagName || '').toLowerCase();
    var typing = tag === 'input' || tag === 'select' || tag === 'textarea';
    if (ev.key === '/' && !typing) { ev.preventDefault(); $('search').focus(); return; }
    if (ev.key === 'Escape' && typing) { ev.target.blur(); return; }
    if (typing) return;

    var p = state.player;
    if (ev.key === ' ') {
      ev.preventDefault();
      if (p && p.setPlaying) p.setPlaying(!p.playing);
      return;
    }
    if (ev.key === 'j') { ev.preventDefault(); stepSelection(1); return; }
    if (ev.key === 'k') { ev.preventDefault(); stepSelection(-1); return; }
    if ((ev.key === '+' || ev.key === '=') && p && p.setZoom) { ev.preventDefault(); p.setZoom(p.zoom + 1); p.draw && p.draw(); return; }
    if (ev.key === '-' && p && p.setZoom) { ev.preventDefault(); p.setZoom(p.zoom - 1); p.draw && p.draw(); return; }
    if (!p || !p.entry) return;
    var e = p.entry;
    if (ev.key === 'ArrowLeft') { ev.preventDefault(); p.col = (p.col + e.columns - 1) % e.columns; p.draw(); }
    else if (ev.key === 'ArrowRight') { ev.preventDefault(); p.col = (p.col + 1) % e.columns; p.draw(); }
    else if (ev.key === 'ArrowUp') { ev.preventDefault(); p.setPlaying(false); p.row = (p.row + e.rows - 1) % e.rows; p.draw(); }
    else if (ev.key === 'ArrowDown') { ev.preventDefault(); p.setPlaying(false); p.row = (p.row + 1) % e.rows; p.draw(); }
  }

  /* ---------------------------------------------------------------- boot */

  function wireChrome() {
    Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (t) {
      t.addEventListener('click', function () {
        state.section = t.getAttribute('data-section');
        state.limit = 200;
        state.selected = null;
        stopPlayer();
        renderTabs();
        refreshList();
      });
    });
    $('search').addEventListener('input', function (ev) {
      state.query = ev.target.value.trim();
      state.limit = 200;
      refreshList();
    });
    $('load-more').addEventListener('click', function () { state.limit += 200; refreshList(); });
    $('clear-filters').addEventListener('click', function () {
      state.query = '';
      $('search').value = '';
      state.factions = {}; state.kinds = {};
      state.flags = { pc: false, anim: false, paired: false, shadows: false };
      state.limit = 200;
      renderFilters();
      refreshList();
    });
    $('help-toggle').addEventListener('click', function () {
      var panel = $('help-panel');
      panel.hidden = !panel.hidden;
      $('help-toggle').setAttribute('aria-expanded', panel.hidden ? 'false' : 'true');
    });
    document.addEventListener('keydown', onKey);
  }

  function capabilityMessage() {
    var bits = [];
    if (!PNG.hasInflate()) {
      bits.push('This browser has no DecompressionStream, so indexed PNGs cannot be decoded: team colour will fall back to a hue rotation.');
    } else if (location.protocol === 'file:' && !Object.keys(boot.inlineAssets).length) {
      bits.push('Opened from file:// without inlined assets. The browser will not let the page read sibling files, ' +
                'so team colour and keyed shadow masks are unavailable. Run "imview serve" or build with --inline for the full feature set.');
    } else {
      bits.push('Pixel data is readable: indexed team-colour remapping and keyed shadow masks are available.');
    }
    return bits.join(' ');
  }

  function start(manifest) {
    indexManifest(manifest);
    $('export-meta').textContent =
      (boot.exportPath ? boot.exportPath + '  —  ' : '') +
      (manifest.game || '') + '  —  manifest v' + manifest.version +
      (boot.generated ? '  —  built ' + boot.generated : '') +
      (Object.keys(boot.inlineAssets).length ? '  —  ' + Object.keys(boot.inlineAssets).length + ' assets inlined' : '');
    $('capability-note').textContent = capabilityMessage();
    renderTabs();
    renderFilters();
    refreshList();
  }

  wireChrome();

  if (boot.manifest) {
    start(boot.manifest);
  } else {
    fetch('manifest.json').then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(start).catch(function (err) {
      $('placeholder').appendChild(el('p', {
        class: 'note note-warn',
        text: 'No manifest is embedded in this page and manifest.json could not be fetched (' + err.message +
              '). Build the viewer with "imview build <export-dir>" or run "imview serve <export-dir>".'
      }));
    });
  }
})();
