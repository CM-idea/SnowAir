const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');
const pngToIco = require('png-to-ico').default;

const root = path.join(__dirname);
const svgPath = path.join(root, 'icons', 'snowair-tray.svg');
const svg = fs.readFileSync(svgPath);

async function main() {
  const sizes = [16, 32, 48, 64, 128, 256];
  const pngBuffers = [];
  for (const size of sizes) {
    const resvg = new Resvg(svg, {
      fitTo: { mode: 'width', value: size },
      background: 'rgba(0,0,0,0)',
    });
    const png = resvg.render().asPng();
    const out = path.join(root, `logo-${size}.png`);
    fs.writeFileSync(out, png);
    pngBuffers.push(png);
    console.log('png', size, png.length);
  }
  const ico = await pngToIco(pngBuffers);
  fs.writeFileSync(path.join(root, 'logo.ico'), ico);
  console.log('wrote logo.ico', ico.length);
  for (const size of sizes) {
    try { fs.unlinkSync(path.join(root, `logo-${size}.png`)); } catch {}
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
