// node --test tests/   (npm test): the desktop app's layout, Core arguments and the Shell's file server
'use strict'
const test = require('node:test')
const assert = require('node:assert')
const fs = require('node:fs')
const http = require('node:http')
const os = require('node:os')
const path = require('node:path')
const { layout, coreArgs, resolveInside, serveFile } = require('../paths.cjs')

test('packaged layout: the Core and the Shell in resources, the Core runs in its own folder (./assets)', () => {
  const l = layout(true, 'C:\\App\\resources', 'C:\\App\\resources\\app.asar')
  assert.strictEqual(l.coreExe, path.join('C:\\App\\resources', 'core', 'mapeditor_core.exe'))
  assert.strictEqual(l.coreCwd, path.join('C:\\App\\resources', 'core'))
  assert.strictEqual(l.shellDir, path.join('C:\\App\\resources', 'shell'))
})

test('working tree layout: core\\build exe run in core\\ (as run.bat), shell\\dist', () => {
  const l = layout(false, '', 'F:\\repo\\desktop')
  assert.strictEqual(l.coreExe, path.join('F:\\repo', 'core', 'build', 'mapeditor_core.exe'))
  assert.strictEqual(l.coreCwd, path.join('F:\\repo', 'core'))
  assert.strictEqual(l.shellDir, path.join('F:\\repo', 'shell', 'dist'))
})

test('Core arguments: port first, then the assets folder (absolute) and the map', () => {
  assert.deepStrictEqual(coreArgs(1234, []), ['1234'])
  assert.deepStrictEqual(coreArgs(1234, ['D:\\game']), ['1234', path.resolve('D:\\game')])
  assert.deepStrictEqual(coreArgs(1234, ['D:\\game', 'maps/a.map.json', 'extra']), ['1234', path.resolve('D:\\game'), 'maps/a.map.json'])
})

test('the file server never leaves the Shell folder', () => {
  const root = path.resolve('/srv/shell')
  assert.strictEqual(resolveInside(root, '/'), path.join(root, 'index.html'))
  assert.strictEqual(resolveInside(root, '/index.html?core=ws%3A%2F%2F127.0.0.1%3A1'), path.join(root, 'index.html'))
  assert.strictEqual(resolveInside(root, '/assets/a.js'), path.join(root, 'assets', 'a.js'))
  assert.strictEqual(resolveInside(root, '/../secret.txt'), null)
  assert.strictEqual(resolveInside(root, '/%2e%2e/secret.txt'), null)
  assert.strictEqual(resolveInside(root, '/..%5csecret.txt'), null)
  assert.strictEqual(resolveInside(root, '/a%00b'), null)
  assert.strictEqual(resolveInside(root, '/%zz'), null)
})

test('the file server answers with the right types', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'le-shell-'))
  fs.writeFileSync(path.join(dir, 'index.html'), '<html></html>')
  fs.mkdirSync(path.join(dir, 'assets'))
  fs.writeFileSync(path.join(dir, 'assets', 'app.js'), 'x')
  const server = http.createServer((req, res) => serveFile(dir, req, res))
  await new Promise((r) => server.listen(0, '127.0.0.1', r))
  const base = `http://127.0.0.1:${server.address().port}`
  try {
    let r = await fetch(`${base}/index.html?core=x`)
    assert.strictEqual(r.status, 200)
    assert.match(r.headers.get('content-type'), /text\/html/)
    r = await fetch(`${base}/assets/app.js`)
    assert.match(r.headers.get('content-type'), /javascript/)
    assert.strictEqual((await fetch(`${base}/missing.js`)).status, 404)
    assert.strictEqual((await fetch(`${base}/x`, { method: 'POST' })).status, 405)
  } finally {
    server.close()
    fs.rmSync(dir, { recursive: true, force: true })
  }
})
