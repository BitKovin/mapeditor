// Where the desktop app finds its parts, and the small static file server for the Shell (kept free of Electron so tests run in plain Node).
'use strict'
const fs = require('node:fs')
const path = require('node:path')

/**
 * Packaged (release\LevelEditor): resources\core\mapeditor_core.exe with resources\core\assets, resources\shell\index.html.
 * From the working tree (npm start in desktop\): core\build\mapeditor_core.exe run in core\ (assets = core\assets, as run.bat does), shell\dist.
 */
function layout(packaged, resourcesPath, appDir) {
  if (packaged) {
    const coreDir = path.join(resourcesPath, 'core')
    return { coreExe: path.join(coreDir, 'mapeditor_core.exe'), coreCwd: coreDir, shellDir: path.join(resourcesPath, 'shell'), icon: path.join(resourcesPath, 'icon.ico') }
  }
  const root = path.resolve(appDir, '..')
  return { coreExe: path.join(root, 'core', 'build', 'mapeditor_core.exe'), coreCwd: path.join(root, 'core'), shellDir: path.join(root, 'shell', 'dist'), icon: path.join(appDir, 'icon.ico') }
}

/** mapeditor_core [port] [assetsRoot] [initialMap]: the app's own arguments are [assetsFolder] [initialMap]. */
function coreArgs(port, userArgs) {
  return [String(port), ...userArgs.slice(0, 2).map((a, i) => (i === 0 ? path.resolve(a) : a))]
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif',
  '.ico': 'image/x-icon', '.webp': 'image/webp', '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.wasm': 'application/wasm', '.map': 'application/json',
}

/** The file under `root` that a request path names, or null when it would leave `root`. */
function resolveInside(root, urlPath) {
  let p
  try { p = decodeURIComponent(urlPath.split('?')[0].split('#')[0]) } catch { return null }
  if (p.includes('\0')) return null
  if (p === '/' || p === '') p = '/index.html'
  const full = path.resolve(root, '.' + p)
  const rel = path.relative(path.resolve(root), full)
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) return null
  return full
}

function serveFile(root, req, res) {
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); return res.end() }
  const file = resolveInside(root, req.url || '/')
  if (!file) { res.writeHead(403); return res.end() }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('not found') }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'Cache-Control': 'no-cache' })
    res.end(req.method === 'HEAD' ? undefined : data)
  })
}

module.exports = { layout, coreArgs, resolveInside, serveFile }
