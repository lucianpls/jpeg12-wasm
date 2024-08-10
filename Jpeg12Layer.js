/**
 * Represents a custom layer for displaying 12bit jpeg encoded tiles on a Leaflet map.
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
      uintview[i] = ALPHA | 0x10101 * c;
      // uintview[i] = ALPHA | (red * c) | ((green * c) << 8) | ((blue * c) << 16);
    }

    ctx.putImageData(imageData, 0, 0);
  },
  
})
