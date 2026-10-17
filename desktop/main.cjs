// Level Editor desktop app (REQUIREMENTS §29, DESIGN §11): starts the Core, serves the built Shell to its own window from
// http://127.0.0.1 (the Core's origin policy admits it unchanged) and stops the Core when the app quits.
// Usage: LevelEditor.exe [assetsFolder] [initialMap]   (the same arguments as run.bat)
'use strict'
const { app, BrowserWindow, dialog, shell, Menu } = require('electron')
const { spawn } = require('node:child_process')
const fs = require('node:fs')
const http = require('node:http')
const net = require('node:net')
const path = require('node:path')
const { layout, coreArgs, serveFile } = require('./paths.cjs')

// one data folder whatever the window title: %APPDATA%\LevelEditor (the Core's log, the window's size, Chromium's cache)
app.setPath('userData', path.join(app.getPath('appData'), 'LevelEditor'))
const paths = layout(app.isPackaged, process.resourcesPath, __dirname)
const userArgs = process.argv.slice(app.isPackaged ? 1 : 2).filter((a) => !a.startsWith('-'))
const logDir = path.join(app.getPath('userData'), 'logs')
const logFile = path.join(logDir, 'core.log')
const windowStateFile = path.join(app.getPath('userData'), 'window.json')

let mainWindow = null
let core = null          // the Core child process
let corePort = 0
let quitting = false
let logTail = []         // the Core's last lines, for the "Core stopped" message

const singleInstance = app.requestSingleInstanceLock()
if (!singleInstance) app.quit()
app.on('second-instance', () => {
  if (!mainWindow) return
  if (mainWindow.isMinimized()) mainWindow.restore()
  mainWindow.focus()
})

/** A free TCP port on 127.0.0.1 (the OS picks it). */
function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer()
    s.once('error', reject)
    s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => resolve(port)) })
  })
}

/** Resolves once something accepts connections on the port, rejects when `alive()` turns false or after `ms`. */
function waitForPort(port, alive, ms) {
  const until = Date.now() + ms
  return new Promise((resolve, reject) => {
    const attempt = () => {
      if (!alive()) return reject(new Error('the Core exited while starting'))
      const sock = net.connect(port, '127.0.0.1')
      sock.once('connect', () => { sock.destroy(); resolve() })
      sock.once('error', () => {
        sock.destroy()
        if (Date.now() > until) reject(new Error(`the Core did not open port ${port} within ${ms / 1000} s`))
        else setTimeout(attempt, 150)
      })
    }
    attempt()
  })
}

function log(line) {
  logTail.push(line)
  if (logTail.length > 40) logTail.shift()
  try { fs.appendFileSync(logFile, line + '\n') } catch {}
}

/** Starts the Core on `port` (the same port on a restart, so the Shell reconnects by itself) and waits until it listens. */
async function startCore(port) {
  if (!fs.existsSync(paths.coreExe)) throw new Error(`Core not found: ${paths.coreExe}`)
  fs.mkdirSync(logDir, { recursive: true })
  log(`---- ${new Date().toISOString()} starting ${paths.coreExe} on port ${port}`)
  const child = spawn(paths.coreExe, coreArgs(port, userArgs), { cwd: paths.coreCwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] })
  core = child
  let exited = false
  const onData = (buf) => { for (const l of buf.toString().split(/\r?\n/)) if (l) log(l) }
  child.stdout.on('data', onData)
  child.stderr.on('data', onData)
  child.on('error', (e) => { exited = true; log(`failed to start the Core: ${e.message}`) })
  child.on('exit', (code, signal) => {
    exited = true
    log(`Core exited (code ${code}${signal ? `, ${signal}` : ''})`)
    if (core === child) core = null
    if (!quitting && mainWindow) void coreStopped(code)
  })
  await waitForPort(port, () => !exited, 30000)
}

function stopCore() {
  const c = core
  core = null
  if (c && c.exitCode === null) c.kill()
}

async function coreStopped(code) {
  const { response } = await dialog.showMessageBox(mainWindow, {
    type: 'error',
    title: 'Level Editor',
    message: `The Core stopped (exit code ${code}).`,
    detail: `${logTail.slice(-12).join('\n')}\n\nFull log: ${logFile}`,
    buttons: ['Restart the Core', 'Open the log', 'Quit'],
    defaultId: 0,
    noLink: true,
  })
  if (response === 0) {
    try { await startCore(corePort) } catch (e) { return fatal(e) }
  } else if (response === 1) {
    await shell.openPath(logFile)
    return coreStopped(code)
  } else app.quit()
}

function fatal(e) {
  dialog.showErrorBox('Level Editor', `${e.message}\n\n${logTail.slice(-12).join('\n')}\n\nFull log: ${logFile}`)
  quitting = true
  stopCore()
  app.exit(1)
}

/** Serves the built Shell (shell/dist) on 127.0.0.1; only files inside that folder. */
function serveShell() {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => serveFile(paths.shellDir, req, res))
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => resolve(server.address().port))
  })
}

function loadWindowState() {
  try { return JSON.parse(fs.readFileSync(windowStateFile, 'utf8')) } catch { return { width: 1600, height: 950, maximized: true } }
}

function saveWindowState(win) {
  try {
    const b = win.getNormalBounds()
    fs.writeFileSync(windowStateFile, JSON.stringify({ ...b, maximized: win.isMaximized() }))
  } catch {}
}

/** Popout panels (Dockview) open same-origin windows that share the page's JavaScript; everything else goes to the system browser. */
function guardWindow(win, origin) {
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (url === 'about:blank' || url.startsWith(origin + '/')) {
      return { action: 'allow', overrideBrowserWindowOptions: { autoHideMenuBar: true, backgroundColor: '#1e1e1e' } }
    }
    if (/^https?:\/\//.test(url)) void shell.openExternal(url)
    return { action: 'deny' }
  })
  win.webContents.on('did-create-window', (child) => { child.setMenu(null); guardWindow(child, origin); devToolsKeys(child) })
  win.webContents.on('will-navigate', (e, url) => {
    if (url.startsWith(origin + '/')) return
    e.preventDefault()  // a dropped file or a link must not replace the editor
    if (/^https?:\/\//.test(url)) void shell.openExternal(url)
  })
}

function devToolsKeys(win) {
  win.webContents.on('before-input-event', (e, input) => {
    if (input.type !== 'keyDown') return
    if (input.key === 'F12' || (input.control && input.shift && input.key.toLowerCase() === 'i')) {
      win.webContents.toggleDevTools()
      e.preventDefault()
    }
  })
}

async function main() {
  Menu.setApplicationMenu(null)  // the Shell has its own menu bar; no default accelerators stealing its shortcuts
  const state = loadWindowState()
  mainWindow = new BrowserWindow({
    x: state.x, y: state.y, width: state.width || 1600, height: state.height || 950,
    minWidth: 800, minHeight: 500,
    show: false, backgroundColor: '#1e1e1e', title: 'Level Editor',
    icon: fs.existsSync(paths.icon) ? paths.icon : undefined,
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true, spellcheck: false },
  })
  if (state.maximized) mainWindow.maximize()
  mainWindow.once('ready-to-show', () => mainWindow.show())
  mainWindow.on('close', () => saveWindowState(mainWindow))
  mainWindow.on('closed', () => { mainWindow = null; app.quit() })
  devToolsKeys(mainWindow)
  // the Shell asks before closing with unsaved changes (beforeunload); a browser shows its own question, Electron needs this one
  mainWindow.webContents.on('will-prevent-unload', (e) => {
    const choice = dialog.showMessageBoxSync(mainWindow, {
      type: 'question', title: 'Level Editor', message: 'The map has unsaved changes.', detail: 'Close without saving?',
      buttons: ['Close without saving', 'Cancel'], defaultId: 1, cancelId: 1, noLink: true,
    })
    if (choice === 0) e.preventDefault()
  })
  await mainWindow.loadURL('data:text/html,' + encodeURIComponent('<body style="background:#1e1e1e;color:#aaa;font:14px sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">Starting the Core...</body>'))

  try {
    if (!fs.existsSync(path.join(paths.shellDir, 'index.html'))) throw new Error(`Shell not built: ${paths.shellDir}\\index.html is missing`)
    corePort = await freePort()
    await startCore(corePort)
    const shellPort = await serveShell()
    const origin = `http://127.0.0.1:${shellPort}`
    guardWindow(mainWindow, origin)
    await mainWindow.loadURL(`${origin}/index.html?core=${encodeURIComponent(`ws://127.0.0.1:${corePort}`)}`)
  } catch (e) {
    fatal(e)
  }
}

if (singleInstance) app.whenReady().then(main)
app.on('before-quit', () => { quitting = true })
app.on('will-quit', stopCore)
app.on('window-all-closed', () => app.quit())
process.on('exit', stopCore)
