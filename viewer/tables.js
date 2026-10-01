/* Lookup tables taken from the game data and from docs/.
 * Everything here is a fact about Imperivm, not a viewer preference. */
(function (global) {
  'use strict';

  /* DATA\CONST.INI, section [PlayerColors], in Packs/data.pak.
     Twelve player colours plus id13, the neutral grey the engine uses for
     unowned objects. The names are descriptive labels for the UI; the INI
     gives numbers only. */
  var PLAYER_COLORS = [
    { id: 1,  name: 'Red',        rgb: [255, 23, 23] },
    { id: 2,  name: 'Yellow',     rgb: [255, 255, 0] },
    { id: 3,  name: 'Green',      rgb: [0, 255, 0] },
    { id: 4,  name: 'Cyan',       rgb: [0, 232, 232] },
    { id: 5,  name: 'Pink',       rgb: [237, 160, 255] },
    { id: 6,  name: 'Pale green', rgb: [182, 230, 134] },
    { id: 7,  name: 'Dark red',   rgb: [174, 0, 0] },
    { id: 8,  name: 'Orange',     rgb: [249, 155, 32] },
    { id: 9,  name: 'Forest',     rgb: [16, 155, 18] },
    { id: 10, name: 'Blue',       rgb: [0, 0, 248] },
    { id: 11, name: 'Magenta',    rgb: [196, 13, 198] },
    { id: 12, name: 'Olive',      rgb: [159, 154, 0] },
    { id: 13, name: 'Neutral',    rgb: [128, 128, 128] }
  ];

  /* docs/data-model.md: eight faction races, keyed by class-name prefix. */
  var FACTIONS = [
    { code: 'B', name: 'Britain' },
    { code: 'C', name: 'Carthage' },
    { code: 'E', name: 'Egypt' },
    { code: 'G', name: 'Gaul' },
    { code: 'I', name: 'Iberia' },
    { code: 'R', name: 'Republican Rome' },
    { code: 'M', name: 'Imperial Rome' },
    { code: 'T', name: 'Teuton' }
  ];

  /* docs/formats/ent-xml.md: animations are addressed by fixed numeric slot.
     The mapping is empirical - the authoritative table is not in the data. */
  var ANIM_SLOTS = {
    0: '(script only)',
    1: 'walk',
    2: 'anim2',
    3: 'anim3',
    5: 'attack',
    9: 'die',
    13: 'idle',
    14: 'idle 2',
    16: '(script only)',
    17: 'heal / carry',
    18: 'toidle',
    19: 'toattack',
    20: 'taunt / work',
    21: 'defence',
    22: 'talk',
    23: 'work',
    24: 'rotating'
  };

  /* Eight facings, one per sheet column. Column 0 is taken to be the first
     facing in the sheet; the engine's absolute compass origin is not pinned
     down in docs/formats/rle.md, so these are labels for orientation only. */
  var FACING_LABELS = ['0', '1', '2', '3', '4', '5', '6', '7'];

  /* Where a path's first segment puts it in the class tree. */
  var KIND_BY_ROOT = {
    units: 'unit',
    unit: 'unit',
    buildings: 'building',
    building: 'building',
    mapobjects: 'decor',
    decor: 'decor',
    additionalart: 'decor',
    visuals: 'effect',
    effects: 'effect',
    outlines: 'effect',
    ui: 'ui'
  };

  var KINDS = ['unit', 'building', 'decor', 'effect', 'ui', 'other'];

  function classify(path) {
    var parts = String(path || '').split('/');
    var root = (parts[0] || '').toLowerCase();
    return KIND_BY_ROOT[root] || 'other';
  }

  /* Faction is the first letter of the entity directory: units/bbowman/... */
  function factionOf(path) {
    var parts = String(path || '').split('/');
    if (parts.length < 3) return null;
    var code = (parts[1] || '').charAt(0).toUpperCase();
    for (var i = 0; i < FACTIONS.length; i++) {
      if (FACTIONS[i].code === code) return FACTIONS[i].code;
    }
    return null;
  }

  global.ImviewTables = {
    PLAYER_COLORS: PLAYER_COLORS,
    FACTIONS: FACTIONS,
    ANIM_SLOTS: ANIM_SLOTS,
    FACING_LABELS: FACING_LABELS,
    KINDS: KINDS,
    classify: classify,
    factionOf: factionOf
  };
})(window);
