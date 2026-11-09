// SPDX-License-Identifier: Apache-2.0
// Regenerate the README screenshots in docs/images/ from the mock server (no hardware needed).
//
//   npm ci && npx playwright install chromium && node tools/screenshots.mjs
import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { chromium } from 'playwright';

const OUT = 'docs/images';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function mock(port, args) {
  return spawn('python3', ['tools/mock_server.py', '--port', String(port), ...args], { stdio: 'ignore' });
}
async function up(base) {
  for (let i = 0; i < 50; i++) {
    try { await fetch(base); return; } catch { await sleep(100); }
  }
  throw new Error('mock server did not start');
}

await mkdir(OUT, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const servers = [mock(18301, ['--mode', 'bridge']), mock(18302, ['--mode', 'setup'])];
const bridge = 'http://127.0.0.1:18301', setup = 'http://127.0.0.1:18302';
try {
  await up(bridge);
  await up(setup);
  const shot = async (url, file, viewport, prepare, before) => {
    const page = await browser.newPage({ viewport, deviceScaleFactor: 1.5, colorScheme: 'light' });
    if (before) await before(page);
    await page.goto(url);
    await page.waitForSelector('#app:not([hidden]), #wiz:not([hidden])');
    await sleep(4500);                         // two status polls, so traffic rates are filled in
    if (prepare) await prepare(page);
    await page.screenshot({ path: `${OUT}/${file}` });
    await page.close();
    console.log(`${OUT}/${file}`);
  };
  const desktop = { width: 1180, height: 760 }, phone = { width: 390, height: 780 };

  await shot(bridge + '/#dash', 'dashboard.png', desktop);
  await shot(bridge + '/#dash', 'dashboard-phone.png', phone);
  await shot(bridge + '/#wifi', 'settings-phone.png', phone);
  await shot(bridge + '/#fw', 'firmware.png', { width: 1180, height: 640 });
  await shot(setup + '/', 'setup-phone.png', phone, async page => {
    await page.waitForSelector('.net');
  }, async page => {
    // the mock's scan list is made for escaping tests: show ordinary networks instead
    const nets = [['HomeNet', 92, 6, 'WPA2'], ['HomeNet-Guest', 78, 6, 'WPA2'], ['Upstairs', 61, 11, 'WPA2/WPA3'],
                  ['Printer-Direct', 44, 1, 'WPA2'], ['CoffeeShop', 30, 13, 'OPEN']]
      .map(([ssid, quality, channel, auth]) => ({ ssid, quality, channel, auth, rssi: quality / 2 - 100,
                                                  secure: auth != 'OPEN' }));
    await page.route('**/api/scan', r => r.fulfill({ json: { scanning: false, networks: nets } }));
  });
} finally {
  await browser.close();
  servers.forEach(s => s.kill());
}
