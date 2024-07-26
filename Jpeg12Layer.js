/**
 * Represents a custom layer for displaying 12bit encoded tiles on a Leaflet map.
 * @class
 * @extends L.GridLayer
 */
var Jpeg12Layer = L.GridLayer.extend({
  createTile: function (coords, done) {
    var error;
    var tile = L.DomUtil.create('canvas', 'leaflet-tile');
    tile.width = this.options.tileSize;
    tile.height = this.options.tileSize;
    tile.zoom = coords.z;

    let xhr = new XMLHttpRequest();
    xhr.responseType = "arraybuffer";
    let url = 'https://astro.arcgis.com/arcgis/rest/services/OnMars/HiRISE/raw/tile/';
    url += coords.z + '/' + coords.y + '/' + coords.x;

    xhr.open("Get", url, true);
    xhr.send();

    xhr.onreadystatechange = function (evt) {
      if (evt.target.readyState == 4 && evt.target.status == 200) {
        tile.raw = new Uint8Array(xhr.response);
        if (tile.raw)
          this.draw(tile);
        else
          error = "Unrecognized data";
        done(error, tile);
      }
    }.bind(this);
    
    return tile;
  },

  redraw: function () {
    for (let key in this._tiles) {
      let tile = this._tiles[key];
      this.draw(tile.el);
    }
  },

  draw: function (tile) {
    // Decode the tile, tell it what we expect
    if (!tile.raw) return;
    let width = tile.width;
    let height = tile.height;
    let image = JPEG12.decode(tile.raw, { width: width, height: height, numComponents: 1});

    let min = +slider.noUiSlider.get()[0];
    let max = +slider.noUiSlider.get()[1];

    // display the RGBA image
    let ctx = tile.getContext('2d');
    let imageData = ctx.createImageData(width, height);
    let uintview = new Uint32Array(imageData.data.buffer, 0, width * height);
    let scale = 256 / (max - min);
    let ALPHA = 0xff000000; // Opaque
    let red = 1;
    let green = 0.5;
    let blue = 0.3;
    for (let i = 0; i < width * height; i++) {
      let c = Math.max(0, Math.min(255, (image.data[i] - min) * scale)) |0;
      // uintview[i] = ALPHA | 0x10101 * c;
      uintview[i] = ALPHA | (red * c) | ((green * c) << 8) | ((blue * c) << 16);
    }

    ctx.putImageData(imageData, 0, 0);
  },

  // Build a palette by linear interpolation beween points
  // buildPalette : function(points)
  // {
  //   let palette = new Uint32Array(256);

  //   let j = -1;
  //   let slope = 0;
  //   for (let i = 0; i < 256; i++) {
  //     if (i == 0 || points.index[j + 1] < i) {
  //       j++;
  //       let f = 1 / (points.index[j + 1] - points.index[j]);
  //       slope = {
  //         red :   f * ((points.values[j+1] & 0xff)         - (points.values[j] & 0xff)),
  //         green : f * (((points.values[j+1] >>  8) & 0xff) - ((points.values[j] >>  8) & 0xff)),
  //         blue :  f * (((points.values[j+1] >> 16) & 0xff) - ((points.values[j] >> 16) & 0xff)),
  //         alpha : f * (((points.values[j+1] >> 24) & 0xff) - ((points.values[j] >> 24) & 0xff)),
  //       }
  //     }

  //     // i is between j and j+1
  //     let l = i - points.index[j];
  //     let v = points.values[j];
  //     red   = 0xff & ((v & 0xff) + l * slope.red);
  //     green = 0xff & (((v >>  8) & 0xff) + l * slope.green);
  //     blue  = 0xff & (((v >> 16) & 0xff) + l * slope.blue);
  //     alpha = 0xff & (((v >> 24) & 0xff) + l * slope.alpha);
  //     palette[i] = (alpha << 24) | (blue << 16) | (green << 8) | red;
  //   };
  //   return palette;
  // }
  
})
