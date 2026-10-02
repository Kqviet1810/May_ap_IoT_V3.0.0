// Real browser interaction using the same isolated account/realtime fixtures as Web QA.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('../cloudflare/node_modules/playwright');
const { setupNotes: setup } = require('./notes_browser_fixture.cjs');
const out = path.resolve(process.argv[2] || '/tmp/mayap-notes-qa'); fs.mkdirSync(out,{recursive:true});
async function main() {
  const browser = await chromium.launch({executablePath:process.env.MAYAP_CHROME,headless:true});
  const results = [];
  try {
    for (const [width,height,mobile] of [[1920,1080,false],[1366,768,false],[768,1024,true],[390,844,true],[390,360,true],[844,390,true]]) {
      const {context,page,errors} = await setup(browser,{width,height,mobile});
      const b = page.locator('#notesBubble'); await b.waitFor();
      const baseline = await page.evaluate(() => ({w:document.documentElement.scrollWidth,h:document.documentElement.scrollHeight}));
      const box = await b.boundingBox(); assert.ok(box.x>=0 && box.y>=0 && box.x+box.width<=width && box.y+box.height<=height);
      const nav = await page.locator('.nav').boundingBox();
      assert.ok(!nav || box.y + box.height<=nav.y || box.x>=nav.x+nav.width || box.x+box.width<=nav.x || box.y>=nav.y+nav.height,'Bubble avoids navigation');
      await b.click();
      await page.getByText('Chưa có ghi chú',{exact:true}).waitFor();
      await page.getByRole('button',{name:'Tạo ghi chú đầu tiên'}).click();
      assert.equal(await page.locator('.notesForm select').inputValue(),'batch','Real fixture has running batch');
      assert.equal(await page.locator('.notesForm input').getAttribute('maxlength'),'60');
      assert.equal(await page.locator('.notesForm textarea').getAttribute('maxlength'),'300');
      await page.locator('.notesForm textarea').fill('   ');
      await page.getByRole('button',{name:'Lưu',exact:true}).click();
      assert.equal(await page.locator('.notesForm').count(),1,'Whitespace cannot save');
      await page.locator('.notesForm input').fill('Soi trứng ngày 7');
      const payload = '<img src=x onerror="window.notesXss=true"> Loại 12 quả không phát triển. ' + 'Nội dung tiếng Việt dài '.repeat(7);
      await page.locator('.notesForm textarea').fill(payload);
      await page.getByRole('button',{name:'Lưu',exact:true}).click();
      await page.locator('.noteCard').waitFor();
      assert.equal(await page.locator('.noteCard img').count(),0,'User text cannot become markup');
      assert.equal(await page.evaluate(()=>Boolean(window.notesXss)),false);
      await page.getByRole('button',{name:'Sửa',exact:true}).click();
      assert.equal(await page.locator('.notesForm textarea').inputValue(),payload.trim());
      await page.locator('.notesForm textarea').fill('Kiểm tra lại vào ngày 10.');
      await page.getByRole('button',{name:'Lưu thay đổi'}).click();
      assert.equal(await page.locator('.noteCard').count(),1,'Edit keeps original record');
      await page.locator('.noteContent').getByText('Kiểm tra lại vào ngày 10.').waitFor();
      for (let i=0;i<4;i++) {
        await page.getByRole('button',{name:'+ Ghi chú mới',exact:true}).click();
        await page.locator('.notesForm textarea').fill(`Ghi chú bảo trì số ${i}`);
        await page.locator('.notesForm select').selectOption('machine');
        await page.getByRole('button',{name:'Lưu',exact:true}).click();
      }
      assert.equal(await page.locator('.noteCard').count(),4,'Quick preview bounded at four');
      await page.getByRole('button',{name:'Xem tất cả',exact:true}).click();
      assert.equal(await page.locator('.noteCard').count(),5);
      await page.getByRole('button',{name:'Mẻ',exact:true}).click(); assert.equal(await page.locator('.noteCard').count(),1);
      await page.getByRole('button',{name:'Tất cả',exact:true}).click();
      await page.getByRole('searchbox',{name:'Tìm ghi chú'}).fill('bảo trì số 1'); assert.equal(await page.locator('.noteCard').count(),1);
      await page.getByRole('button',{name:'Xóa',exact:true}).click(); await page.locator('#confirmDialog').waitFor();
      await page.locator('#confirmCancel').click(); assert.equal(await page.locator('.noteCard').count(),1);
      await page.getByRole('button',{name:'Xóa',exact:true}).click(); await page.locator('#confirmAccept').click(); assert.equal(await page.locator('.noteCard').count(),0);
      await page.getByRole('searchbox',{name:'Tìm ghi chú'}).fill('');
      await page.getByRole('button',{name:'+ Ghi chú mới',exact:true}).click();
      await page.locator('.notesForm textarea').fill('Chưa lưu');
      await page.getByRole('button',{name:'Đóng ghi chú'}).click(); await page.locator('#confirmDialog').waitFor();
      await page.locator('#confirmCancel').click(); assert.equal(await page.locator('.notesForm textarea').inputValue(),'Chưa lưu');
      await page.getByRole('button',{name:'Đóng ghi chú'}).click(); await page.locator('#confirmAccept').click();
      assert.equal(await page.locator('#notesPanel').isVisible(),false);
      await b.click(); await page.locator('.noteCard').first().waitFor();
      const layout = await page.locator('#notesPanel').evaluate(n=>{const r=n.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};});
      assert.ok(layout.x>=0 && layout.y>=0 && layout.right<=width+1 && layout.bottom<=height+1,`Panel within ${width}x${height}: ${JSON.stringify(layout)}`);
      assert.deepEqual(await page.evaluate(()=>({w:document.documentElement.scrollWidth,h:document.documentElement.scrollHeight})),baseline,'Overlay does not reflow page');
      await page.screenshot({path:path.join(out,`notes-${width}x${height}.png`)});
      await page.keyboard.press('Escape'); assert.equal(await page.locator('#notesPanel').isVisible(),false);
      // Drag then release snaps left, persists, and does not trigger open.
      const r = await b.boundingBox(); await page.mouse.move(r.x+26,r.y+26); await page.mouse.down(); await page.mouse.move(12+26,80,{steps:12}); await page.mouse.up();
      assert.equal(await page.locator('#notesPanel').isVisible(),false);
      assert.equal(Math.round((await b.boundingBox()).x),12);
      assert.equal(await page.evaluate(()=>JSON.parse(localStorage.getItem('mayap.notes.position.v1')).side),'left');
      const docked = await b.boundingBox(); await page.mouse.move(docked.x+26,docked.y+26); await page.mouse.down(); await page.mouse.move(-100,-100,{steps:6});
      const bounded = await b.boundingBox(); assert.ok(bounded.x>=0 && bounded.y>=0,'Drag cannot leave viewport'); await page.mouse.up();
      await page.reload(); await b.waitFor(); assert.equal(Math.round((await b.boundingBox()).x),12);
      await b.click(); await page.locator('.noteCard').first().waitFor();
      assert.equal(await page.locator('.notesBadge').innerText(),'4','Device notes survive browser reload');
      await page.evaluate(()=>{const d=window.__qa.state.devices[0];d.snapshot.runtime.batchRunning=false;window.__qa.renderDevice();});
      await page.getByRole('button',{name:'+ Ghi chú mới',exact:true}).click();
      assert.equal(await page.locator('.notesForm select').inputValue(),'machine'); assert.equal(await page.locator('.notesForm option[value="batch"]').evaluate(n=>n.disabled),true);
      assert.deepEqual(errors,[]); await context.close(); results.push(`${width}x${height}: cards/create/edit/delete/search/filter/dirty close/layout/drag/persistence PASS`);
    }
    // Native touch drag and mobile keyboard viewport contraction.
    const touch = await setup(browser,{width:390,height:844,mobile:true,theme:'dark'});
    const p = touch.page, cdp = await touch.context.newCDPSession(p);
    let r = await p.locator('#notesBubble').boundingBox();
    await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:r.x+26,y:r.y+26}]});
    for(let i=1;i<=8;i++) await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:r.x+26+(38-r.x-26)*i/8,y:r.y+26+(240-r.y-26)*i/8}]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    assert.equal(await p.locator('#notesPanel').isVisible(),false,'Touch drag never opens panel');
    assert.equal(Math.round((await p.locator('#notesBubble').boundingBox()).x),12);
    await p.locator('#notesBubble').tap();
    await p.getByRole('button',{name:'+ Ghi chú mới',exact:true}).tap();
    await p.locator('.notesForm textarea').fill('Bảo trì máy và kiểm tra cảm biến.');
    await cdp.send('Emulation.setDeviceMetricsOverride',{width:390,height:370,deviceScaleFactor:1,mobile:true,screenWidth:390,screenHeight:844});
    await p.waitForFunction(()=>visualViewport.height<400);
    await p.waitForFunction(()=>{const r=document.getElementById('notesPanel').getBoundingClientRect();return r.bottom<=visualViewport.offsetTop+visualViewport.height+1;});
    await p.getByRole('button',{name:'Lưu',exact:true}).tap(); await p.locator('.noteCard').waitFor();
    await p.screenshot({path:path.join(out,'notes-mobile-keyboard-dark.png')});
    assert.deepEqual(touch.errors,[]);await cdp.detach();await touch.context.close();
    results.push('Native touch snapping and contracted visual viewport: textarea/save remain usable PASS');

    const adapter = await setup(browser,{width:1366,height:768});const a=adapter.page;
    adapter.device.failLoad=true;let release;adapter.device.hold=new Promise(resolve=>release=resolve);
    await a.locator('#notesBubble').click();await a.getByRole('status',{name:'Đang tải ghi chú'}).waitFor();
    release();adapter.device.hold=null;await a.getByText('Không thể tải ghi chú. Thử lại.',{exact:true}).waitFor();
    adapter.device.failLoad=false;await a.getByRole('button',{name:'Thử lại',exact:true}).click();
    await a.getByRole('button',{name:'Tạo ghi chú đầu tiên'}).click();await a.locator('.notesForm textarea').fill('Giữ bản nháp nếu EEPROM lỗi.');
    adapter.device.failSave=true;await a.getByRole('button',{name:'Lưu',exact:true}).click();await a.getByText('Không thể lưu ghi chú. Thử lại.',{exact:true}).waitFor();
    assert.equal(await a.locator('.notesForm textarea').inputValue(),'Giữ bản nháp nếu EEPROM lỗi.');
    const id=adapter.device.calls.at(-1).body.note.id;
    adapter.device.failSave=false;await a.getByRole('button',{name:'Lưu',exact:true}).click();await a.locator('.noteCard').waitFor();
    assert.equal(adapter.device.calls.at(-1).body.note.id,id,'Retry preserves draft ID');
    const original=adapter.device.rows.get('MAP-1234567890AB')[0];
    await a.getByRole('button',{name:'Sửa',exact:true}).click();await a.locator('.notesForm input').fill('Cập nhật');await a.getByRole('button',{name:'Lưu thay đổi'}).click();await a.getByRole('button',{name:'Sửa',exact:true}).waitFor();
    const edited=adapter.device.rows.get('MAP-1234567890AB')[0];assert.equal(edited.id,original.id);assert.equal(edited.createdAt,original.createdAt);assert.equal(edited.version,2);
    adapter.device.failDelete=true;await a.getByRole('button',{name:'Xóa',exact:true}).click();await a.locator('#confirmAccept').click();await a.getByText('Không thể xóa ghi chú. Thử lại.',{exact:true}).waitFor();assert.equal(await a.locator('.noteCard').count(),1);
    adapter.device.failDelete=false;await a.getByRole('button',{name:'Xóa',exact:true}).click();await a.locator('#confirmAccept').click();await a.getByText('Chưa có ghi chú',{exact:true}).waitFor();
    assert.deepEqual(adapter.errors,[]);await adapter.context.close();results.push('Signed device bridge: loading/retry/errors, stable draft ID and stored versions PASS');
    console.log(results.join('\n'));
  } finally { await browser.close(); }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
