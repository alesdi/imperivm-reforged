/* Minimal PNG decoder for the asset viewer.
 *
 * Why this exists: team colour in this game is a palette swap over indices
 * 0..63, and in a player-colour sheet those entries are a byte-identical copy
 * of 64..127. Reading pixels back through <img> + getImageData therefore
 * CANNOT tell a team-colour pixel from its neutral twin - the RGB is the same.
 * The only way to recolour correctly is to read the actual palette indices out
 * of the PNG, which means decoding it ourselves.
 *
 * Mirrors src/imperivm/png.py: 8-bit depth, no interlacing, colour types
 * 0 (grey), 2 (rgb), 3 (indexed), 4 (grey+alpha), 6 (rgba).
 *
 * Inflate uses DecompressionStream('deflate'), which every current browser
 * ships and which needs no network. If it is missing, decoding is reported as
 * unavailable and the viewer falls back to <img> rendering.
 */
(function (global) {
  'use strict';

  var SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  var CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

  function hasInflate() {
    return typeof global.DecompressionStream === 'function';
  }

  function inflate(bytes) {
    if (!hasInflate()) {
      return Promise.reject(new Error('DecompressionStream is unavailable in this browser'));
    }
    var stream = new global.DecompressionStream('deflate');
    var writer = stream.writable.getWriter();
    writer.write(bytes);
    writer.close();
    return new Response(stream.readable).arrayBuffer().then(function (buf) {
      return new Uint8Array(buf);
    });
  }

  function paeth(a, b, c) {
    var p = a + b - c;
    var pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
  }

  function unfilter(raw, width, height, channels) {
    var stride = width * channels;
    var out = new Uint8Array(stride * height);
    var prev = new Uint8Array(stride);
    var src = 0;
    for (var y = 0; y < height; y++) {
      var ftype = raw[src++];
      var row = out.subarray(y * stride, (y + 1) * stride);
      row.set(raw.subarray(src, src + stride));
      src += stride;
      var i;
      if (ftype === 1) {
        for (i = channels; i < stride; i++) row[i] = (row[i] + row[i - channels]) & 0xff;
      } else if (ftype === 2) {
        for (i = 0; i < stride; i++) row[i] = (row[i] + prev[i]) & 0xff;
      } else if (ftype === 3) {
        for (i = 0; i < stride; i++) {
          var left = i >= channels ? row[i - channels] : 0;
          row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xff;
        }
      } else if (ftype === 4) {
        for (i = 0; i < stride; i++) {
          var a = i >= channels ? row[i - channels] : 0;
          var c = i >= channels ? prev[i - channels] : 0;
          row[i] = (row[i] + paeth(a, prev[i], c)) & 0xff;
        }
      } else if (ftype !== 0) {
        throw new Error('unknown PNG filter type ' + ftype);
      }
      prev = row;
    }
    return out;
  }

  /* Decode PNG bytes into {width, height, colorType, channels, pixels,
     palette (Uint8Array rgb triples) , transparency (Uint8Array)}. */
  function decode(bytes) {
    for (var i = 0; i < 8; i++) {
      if (bytes[i] !== SIG[i]) return Promise.reject(new Error('not a PNG file'));
    }
    var view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    var pos = 8;
    var header = null, palette = null, transparency = null;
    var idat = [], idatLen = 0;

    while (pos + 8 <= bytes.length) {
      var length = view.getUint32(pos);
      var tag = String.fromCharCode(bytes[pos + 4], bytes[pos + 5], bytes[pos + 6], bytes[pos + 7]);
      var body = bytes.subarray(pos + 8, pos + 8 + length);
      pos += 12 + length;
      if (tag === 'IHDR') {
        header = {
          width: ((body[0] << 24) | (body[1] << 16) | (body[2] << 8) | body[3]) >>> 0,
          height: ((body[4] << 24) | (body[5] << 16) | (body[6] << 8) | body[7]) >>> 0,
          depth: body[8],
          colorType: body[9],
          interlace: body[12]
        };
      } else if (tag === 'PLTE') {
        palette = body.slice();
      } else if (tag === 'tRNS') {
        transparency = body.slice();
      } else if (tag === 'IDAT') {
        idat.push(body);
        idatLen += body.length;
      } else if (tag === 'IEND') {
        break;
      }
    }

    if (!header) return Promise.reject(new Error('no IHDR chunk'));
    if (header.depth !== 8) return Promise.reject(new Error('bit depth ' + header.depth + ' is not supported'));
    if (header.interlace) return Promise.reject(new Error('interlaced PNG is not supported'));
    var channels = CHANNELS[header.colorType];
    if (!channels) return Promise.reject(new Error('colour type ' + header.colorType + ' is not supported'));

    var joined = new Uint8Array(idatLen);
    var at = 0;
    for (var k = 0; k < idat.length; k++) { joined.set(idat[k], at); at += idat[k].length; }

    return inflate(joined).then(function (raw) {
      var expected = (header.width * channels + 1) * header.height;
      if (raw.length !== expected) {
        throw new Error('decompressed size ' + raw.length + ' != expected ' + expected);
      }
      return {
        width: header.width,
        height: header.height,
        colorType: header.colorType,
        channels: channels,
        pixels: unfilter(raw, header.width, header.height, channels),
        palette: palette,
        transparency: transparency
      };
    });
  }

  /* Expand a decoded image to RGBA, optionally remapping palette entries.
     `paletteOverride` is a Uint8Array of rgb triples used instead of the
     file's own PLTE (same length semantics). */
  function toRGBA(img, paletteOverride) {
    var n = img.width * img.height;
    var out = new Uint8ClampedArray(n * 4);
    var px = img.pixels;
    var i, j;
    if (img.colorType === 3) {
      var pal = paletteOverride || img.palette;
      var trns = img.transparency;
      for (i = 0; i < n; i++) {
        var idx = px[i];
        j = i * 4;
        out[j] = pal[idx * 3];
        out[j + 1] = pal[idx * 3 + 1];
        out[j + 2] = pal[idx * 3 + 2];
        out[j + 3] = trns && idx < trns.length ? trns[idx] : 255;
      }
    } else if (img.colorType === 0) {
      for (i = 0; i < n; i++) { j = i * 4; out[j] = out[j + 1] = out[j + 2] = px[i]; out[j + 3] = 255; }
    } else if (img.colorType === 4) {
      for (i = 0; i < n; i++) { j = i * 4; out[j] = out[j + 1] = out[j + 2] = px[i * 2]; out[j + 3] = px[i * 2 + 1]; }
    } else if (img.colorType === 2) {
      for (i = 0; i < n; i++) { j = i * 4; out[j] = px[i * 3]; out[j + 1] = px[i * 3 + 1]; out[j + 2] = px[i * 3 + 2]; out[j + 3] = 255; }
    } else {
      out.set(px);
    }
    return new ImageData(out, img.width, img.height);
  }

  /* A shadow sheet is a 1-bit coverage mask exported as a greyscale (or
     grey+alpha) PNG. Turn coverage into alpha over solid black so it can be
     composited under the body. */
  function shadowToRGBA(img, rgb) {
    var n = img.width * img.height;
    var out = new Uint8ClampedArray(n * 4);
    var px = img.pixels;
    var r = rgb ? rgb[0] : 0, g = rgb ? rgb[1] : 0, b = rgb ? rgb[2] : 0;
    for (var i = 0; i < n; i++) {
      var cover;
      if (img.colorType === 0) cover = px[i];
      else if (img.colorType === 4) cover = Math.max(px[i * 2], px[i * 2 + 1]);
      else if (img.colorType === 6) cover = px[i * 4 + 3];
      else if (img.colorType === 3) {
        var idx = px[i];
        if (img.transparency && idx < img.transparency.length) cover = img.transparency[idx];
        else cover = img.palette ? Math.max(img.palette[idx * 3], img.palette[idx * 3 + 1], img.palette[idx * 3 + 2]) : 255;
      } else cover = Math.max(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
      var j = i * 4;
      out[j] = r; out[j + 1] = g; out[j + 2] = b; out[j + 3] = cover;
    }
    return new ImageData(out, img.width, img.height);
  }

  global.ImviewPng = {
    decode: decode,
    toRGBA: toRGBA,
    shadowToRGBA: shadowToRGBA,
    hasInflate: hasInflate
  };
})(window);
