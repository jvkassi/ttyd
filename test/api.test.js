// test/api.test.js
// Simple test suite for ttyd-nodejs REST API
const assert = require('assert');
const { test, describe } = require('node:test');
const fetch = (...args) => import('node-fetch').then(({default: fetch}) => fetch(...args));

const BASE_URL = process.env.API_URL || 'http://localhost:3000';

async function postCommand(command) {
  const res = await fetch(`${BASE_URL}/api/command`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ command }),
  });
  const json = await res.json();
  return { status: res.status, ...json };
}

describe('REST API /api/command', () => {
  test('should execute a valid command', async () => {
    const { status, stdout, exit_code } = await postCommand('echo hello');
    assert.strictEqual(status, 200);
    assert(stdout.includes('hello'));
    assert.strictEqual(exit_code, 0);
    console.log('PASS: valid command');
  });

  test('should handle invalid command', async () => {
    const { status, stdout, exit_code } = await postCommand('notarealcommand');
    assert.strictEqual(status, 200);
    assert(stdout.toLowerCase().includes('not found') || exit_code !== 0);
    console.log('PASS: invalid command');
  });

  test('should handle empty command', async () => {
    const res = await fetch(`${BASE_URL}/api/command`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ command: '' }),
    });
    assert.strictEqual(res.status, 400);
    const json = await res.json();
    assert(json.error);
    console.log('PASS: empty command');
  });

  test('should handle missing command', async () => {
    const res = await fetch(`${BASE_URL}/api/command`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
    });
    assert.strictEqual(res.status, 400);
    const json = await res.json();
    assert(json.error);
    console.log('PASS: missing command');
  });
});