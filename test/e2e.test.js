// test/e2e.test.js
// End-to-end test: starts the server, runs API tests, then shuts down
const assert = require('assert');
const { test, describe, before, after } = require('node:test');
const fetch = (...args) =>
  import("node-fetch").then(({ default: fetch }) => fetch(...args));
const { spawn } = require('child_process');
const path = require('path');

const SERVER_PATH = path.resolve(__dirname, '../server.js');
const PORT = 3100 + Math.floor(Math.random() * 1000);
const BASE_URL = `http://localhost:${PORT}`;
let serverProcess;

async function waitForServer(url, timeout = 5000) {
  const start = Date.now();
  while (Date.now() - start < timeout) {
    try {
      const res = await fetch(url);
      if (res.ok) return true;
    } catch (e) {}
    await new Promise(r => setTimeout(r, 100));
  }
  throw new Error('Server did not start in time');
}

async function postCommand(command) {
  const res = await fetch(`${BASE_URL}/api/command`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ command }),
  });
  const json = await res.json();
  return { status: res.status, ...json };
}

describe('E2E: ttyd-nodejs server', () => {
  before(async () => {
    serverProcess = spawn('node', [SERVER_PATH], {
      env: { ...process.env, PORT },
      stdio: 'inherit',
    });
    await waitForServer(`${BASE_URL}`);
  });

  after(() => {
    if (serverProcess) serverProcess.kill();
  });

  test('should execute a valid command', async () => {
    const { status, stdout, exit_code } = await postCommand('echo e2e');
    assert.strictEqual(status, 200);
    console.log('STDOUT:', JSON.stringify(stdout));
    assert.strictEqual(stdout.trim(), 'e2e');
    assert.strictEqual(exit_code, 0);
    console.log('PASS: valid command (e2e)');
  });

  test('should handle invalid command', async () => {
    const { status, stdout, exit_code } = await postCommand('notarealcommand');
    assert.strictEqual(status, 200);
    assert(stdout.toLowerCase().includes('not found') || exit_code !== 0);
    console.log('PASS: invalid command (e2e)');
  });
});