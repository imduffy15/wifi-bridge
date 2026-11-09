// SPDX-License-Identifier: Apache-2.0
// Browser regressions for setup, settings preservation, and update recovery.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { chromium } from 'playwright';
const readRecovery=await readFile('safeboot/main/index.html','utf8');
const port = 18187, base = `http://127.0.0.1:${port}`;
const server = spawn('python3', ['tools/mock_server.py', '--mode', 'bridge', '--port', String(port)], {stdio:'ignore'});
let browser;
try {
  for(let i=0;;i++) { try { await fetch(base); break; } catch { if(i>50)throw Error('Mock did not start'); await new Promise(r=>setTimeout(r,100)); } }
  browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH||undefined});
  const page = await browser.newPage({viewport:{width:390,height:844}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>localStorage.setItem('wbHist','["Password1 old-secret"]'));
  await page.goto(base+'/#wifi');
  await page.waitForFunction(()=>document.querySelector('#t-wifi fieldset')?.disabled===false);
  assert.equal(await page.evaluate(()=>localStorage.getItem('wbHist')),null);
  console.log('PASS legacy console history is removed from browser storage');
  await page.locator('#f_wifi_ssid1').fill(' Network with spaces ');
  await page.locator('#tb-sys').click();
  await page.locator('#f_device_name').fill('Desk Bridge');
  await page.locator('#t-sys button').filter({hasText:/^Save$/}).click();
  await page.waitForFunction(()=>document.querySelector('#t-sys [data-feedback]').textContent.startsWith('Saved.'));
  assert.equal(await page.locator('#f_wifi_ssid1').inputValue(),' Network with spaces ');
  await page.locator('#t-sys [data-undo]').click();
  assert.equal(await page.locator('#f_wifi_ssid1').inputValue(),' Network with spaces ');
  await page.locator('#tb-wifi').click();
  const request=page.waitForRequest(r=>r.url().endsWith('/api/config')&&r.method()==='POST');
  await page.locator('#t-wifi button').filter({hasText:/^Save$/}).click();
  assert.equal((await request).postDataJSON().wifi.ssid1,' Network with spaces ');
  await page.waitForFunction(()=>document.querySelector('#t-wifi [data-feedback]').textContent.startsWith('Saved.'));
  console.log('PASS saving and undo affect only their own tab; SSIDs preserve spaces');

  await page.route('**/api/config',route=>route.request().method()==='POST'?route.fulfill({status:500,json:{error:'storage unavailable'}}):route.continue());
  await page.locator('#f_wifi_ssid1').fill('Keep my edits');
  await page.locator('#t-wifi button').filter({hasText:/^Save$/}).click();
  await page.waitForFunction(()=>document.querySelector('#t-wifi [data-feedback]').textContent.includes('storage unavailable'));
  assert.equal(await page.locator('#f_wifi_ssid1').inputValue(),'Keep my edits');
  assert.equal(await page.locator('#t-wifi fieldset').isDisabled(),false);
  await page.unroute('**/api/config');
  await page.locator('#t-wifi [data-undo]').click();
  console.log('PASS failed saves preserve editable entries and persistent error feedback');

  await page.locator('#tb-mnt').click();await page.locator('#fA').click();
  assert.equal(await page.locator(':focus').textContent(),'Cancel');
  await page.keyboard.press('Shift+Tab');assert.equal(await page.locator(':focus').textContent(),'Erase everything');
  await page.keyboard.press('Tab');assert.equal(await page.locator(':focus').textContent(),'Cancel');
  await page.keyboard.press('Escape');assert.equal(await page.locator(':focus').getAttribute('id'),'fA');
  assert.equal(await page.locator('main').evaluate(el=>el.inert),false);
  console.log('PASS destructive dialogs default to cancel, contain focus, and restore focus');

  // Exercise the actual setup form without rebooting the shared mock.
  await page.evaluate(()=>{Object.assign(W,{ssid:'HomeNet',pw:'testpassword',secure:true});wizNet({ip:'192.168.1.20',suggested_ip:'192.168.1.250',netmask:'255.255.255.0',gateway:'192.168.1.1',dns:'192.168.1.1'});document.getElementById('app').hidden=true;document.getElementById('wiz').hidden=false});
  await page.locator('#wA').fill(' password ');await page.locator('#wA2').fill('password');
  await page.locator('#wF button').filter({hasText:'Save and restart'}).click();
  assert.match(await page.locator('#wE').textContent(),/don’t match/);
  console.log('PASS setup password confirmation compares exact characters');

  await page.goto(base+'/?update-test#fw');
  await page.locator('#fwUrl').fill('http://example.test/firmware.bin');
  await page.route('**/api/ota/url',route=>route.fulfill({json:{ok:true}}));
  await page.route('**/api/ota',route=>route.abort());
  // Keep time realistic except for the 60-second recovery deadline.
  await page.clock.install();
  await page.locator('#fwUB').click();await page.locator('[data-r="1"]').click();
  for(let i=0;i<15;i++){await page.clock.runFor(5000);await page.waitForTimeout(10)}
  assert.match(await page.locator('#fwT').textContent(),/result is unknown/);
  assert.equal(await page.locator('#fwT a').count(),1);
  assert.equal(await page.locator('#fwU').isDisabled(),true);
  console.log('PASS interrupted update has bounded recovery and does not offer a duplicate upload');
  await page.close();

  const retry=await browser.newPage();
  retry.on('pageerror',e=>errors.push(e.message));
  await retry.route('**/api/config',r=>r.fulfill({status:503,json:{error:'temporarily unavailable'}}));
  await retry.goto(base+'/#sys');
  await retry.locator('#t-sys [data-retry]').waitFor({state:'visible'});
  assert.equal(await retry.locator('#f_device_name').isDisabled(),true);
  await retry.unroute('**/api/config');await retry.locator('#t-sys [data-retry]').click();
  await retry.waitForFunction(()=>!document.querySelector('#t-sys fieldset').disabled);
  console.log('PASS settings stay disabled until loaded and recover through Retry');
  for(const dark of [false,true]){
    await retry.emulateMedia({colorScheme:dark?'dark':'light'});
    await retry.setViewportSize({width:390,height:844});
    for(const tab of ['dash','wifi','net','eth','mqtt','sys','fw','con','mnt']){
      await retry.locator('#tb-'+tab).click();
      assert.equal(await retry.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true,`${tab} overflows mobile viewport`);
    }
  }
  console.log('PASS all tabs fit a phone viewport in light and dark modes');

  // A reboot may happen before the browser sees "done". An idle status must
  // not be presented as proof that installation failed.
  await retry.locator('#tb-fw').click();
  await retry.route('**/api/ota/url',r=>r.fulfill({json:{ok:true}}));
  await retry.route('**/api/ota',r=>r.fulfill({json:{state:'idle',bytes:0}}));
  await retry.locator('#fwUrl').fill('http://example.test/firmware.bin');
  await retry.locator('#fwUB').click();await retry.locator('[data-r="1"]').click();
  await retry.waitForFunction(()=>document.querySelector('#fwT').textContent.includes('result is unknown'));
  console.log('PASS missed update completion reports uncertainty instead of failure');

  const recovery=await browser.newPage();
  recovery.on('pageerror',e=>errors.push(e.message));
  await recovery.route('**/recovery-preview',r=>r.fulfill({contentType:'text/html',body:readRecovery}));
  await recovery.route('**/api/status',r=>r.fulfill({json:{mode:'safeboot',device:{name:'Test Bridge',firmware_version:'3.1.0'},wifi:{state:'connected'},ethernet:{state:'up'}}}));
  await recovery.route('**/api/ota/url',r=>r.fulfill({json:{ok:true}}));
  await recovery.route('**/api/ota',r=>r.abort());
  await recovery.goto(base+'/recovery-preview');await recovery.clock.install();
  await recovery.locator('#url').fill('http://example.test/firmware.bin');await recovery.locator('#ub').click();
  for(let i=0;i<15;i++){await recovery.clock.runFor(5000);await recovery.waitForTimeout(10)}
  assert.match(await recovery.locator('#pt').textContent(),/stopped reporting update progress/);
  assert.equal(await recovery.locator('#pt a').count(),1);
  console.log('PASS safeboot also provides bounded recovery after a lost connection');
  assert.deepEqual(errors,[],'browser runtime errors');
} finally { await browser?.close();server.kill(); }
