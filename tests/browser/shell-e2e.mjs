// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * Real-disc validation of the browser build through its shell page.
 *
 *   python3 tools/browser/serve.py --port 5190 &
 *   MELEE_ISO=/path/to/GALE01.iso PLAYWRIGHT_MODULE=/path/to/playwright \
 *     node tests/browser/shell-e2e.mjs [case ...]
 *
 * Scenes are reached with MELEE_BOOT_SCENE (docs/testing.md), not synthetic
 * menu navigation. Each case must hold 60 fps: that, not "it rendered", is the
 * bar for this platform. Headed Chrome, because headless has no WebGPU adapter
 * on most machines.
 */
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';

const { chromium } = createRequire(import.meta.url)(process.env.PLAYWRIGHT_MODULE || 'playwright');
const iso = process.env.MELEE_ISO;
if (!iso) throw Error('Set MELEE_ISO to your own GALE01 disc image.');
const base = process.env.MELEE_TEST_URL || 'http://127.0.0.1:5190/';
const output = path.resolve(process.env.TEST_OUTPUT || 'build/browser/shell-e2e');
const measureMs = Number(process.env.MELEE_MEASURE_MS || 20000);
await mkdir(output, { recursive: true });

const cases = [
  { name: 'title', env: {}, keys: ['x', 'x', 'x', 'Enter', 'Enter'] },
  { name: 'vs', env: { MELEE_BOOT_SCENE: 'vs' } },
  { name: 'classic', env: { MELEE_BOOT_SCENE: 'classic' } },
  { name: 'training', env: { MELEE_BOOT_SCENE: 'training' } },
  // Four CPUs on Final Destination: the heaviest scene, and it stays busy unattended.
  { name: 'vs-cpu4', env: { MELEE_BOOT_SCENE: 'vs', MELEE_DEBUG_VS: 'cpu4' } },
].filter((c) => process.argv.length < 3 || process.argv.slice(2).includes(c.name));

const browser = await chromium.launch({ channel: 'chrome', headless: false });
const results = [];
try {
  for (const test of cases) {
    const context = await browser.newContext({
      viewport: { width: 1000, height: 1000 },
    });
    const page = await context.newPage();
    const logs = [];
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e.stack || e)));
    page.on('console', (m) => {
      logs.push(m.text());
      if (/panic|fatal|abort\(|Assertion failed/i.test(m.text())) errors.push(m.text());
    });
    const result = { name: test.name, env: test.env };
    try {
      const url = new URL(base);
      for (const [k, v] of Object.entries({ MELEE_SEED: '1', MELEE_SCENE_LOG: '1', ...test.env })) url.searchParams.set(k, v);
      await page.goto(url.href);
      await page.locator('#disc').setInputFiles(iso);
      await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 60000 });
      const started = Date.now();
      await page.locator('#start').click();
      await page.waitForFunction(() => window.meleeFrames.count >= 300, null, { timeout: 180000 });
      result.bootMs = Date.now() - started;
      for (const key of test.keys || []) {
        await page.locator('#canvas').click();
        await page.keyboard.down(key);
        await page.waitForTimeout(120);
        await page.keyboard.up(key);
        await page.waitForTimeout(2500);
      }
      await page.evaluate(() => { window.meleeFrames.samples.length = 0; });
      // Screenshot mid-window so the image shows what was actually measured.
      await page.waitForTimeout(measureMs / 2);
      await page.screenshot({ path: path.join(output, `${test.name}.png`) });
      await page.waitForTimeout(measureMs / 2);
      Object.assign(result, await page.evaluate(() => {
        const samples = window.meleeFrames.samples;
        const sorted = [...samples].sort((a, b) => a - b);
        const total = samples.reduce((a, b) => a + b, 0);
        return {
          frames: samples.length,
          fps: samples.length * 1000 / total,
          p95: sorted[Math.floor(sorted.length * 0.95)],
          p99: sorted[Math.floor(sorted.length * 0.99)],
          maxFrameMs: sorted.at(-1),
          over33ms: samples.filter((ms) => ms > 33.4).length,
        };
      }));
      result.scenes = logs.filter((l) => l.startsWith('boot scene:'));
      if (test.env.MELEE_BOOT_SCENE && !result.scenes.some((l) => l.startsWith('boot scene: game mode'))) {
        throw Error('MELEE_BOOT_SCENE did not reach the engine: holding 60 fps on the boot path proves nothing.');
      }
      if (errors.length) throw Error(errors.join('\n'));
      if (result.fps < 58.5) throw Error(`Low FPS: ${result.fps.toFixed(2)}`);
      if (result.p99 > 33.4) throw Error(`p99 frame time ${result.p99.toFixed(1)} ms`);
      result.status = 'passed';
    } catch (error) {
      result.status = 'failed';
      result.error = String(error.stack || error);
      result.lastLogs = logs.slice(-15);
      await page.screenshot({ path: path.join(output, `${test.name}-failed.png`) }).catch(() => {});
    }
    await context.close();
    results.push(result);
    await writeFile(path.join(output, 'report.json'), JSON.stringify(results, null, 2) + '\n');
    console.log(test.name, result.status, result.fps?.toFixed(2) ?? '', result.error ?? '');
  }
} finally {
  await browser.close();
}
if (results.some((r) => r.status !== 'passed')) process.exitCode = 1;
