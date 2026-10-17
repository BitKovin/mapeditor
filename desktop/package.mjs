// Packages the desktop app into release\LevelEditor (REQUIREMENTS §29 P2). build_app.bat runs this after building the Core and the Shell.
// The folder holds everything the program needs: LevelEditor.exe (Electron), resources\core (the Core, its tools, the denoiser and MSVC
// runtime DLLs, the default assets), resources\shell (the built Shell).
import { packager } from '@electron/packager'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')
// the Core's build folder: the first argument (build_app.bat passes core\build_release), else core\build_release, else core\build
const coreBuild = process.argv[2] ? path.resolve(process.argv[2]) : [path.join(root, 'core', 'build_release'), path.join(root, 'core', 'build')].find((d) => fs.existsSync(path.join(d, 'mapeditor_core.exe'))) ?? path.join(root, 'core', 'build_release')
const shellDist = path.join(root, 'shell', 'dist')
const stage = path.join(here, 'out', 'stage')
const release = path.join(root, 'release', 'LevelEditor')

function need(file, hint) {
  if (!fs.existsSync(file)) { console.error(`ERROR: ${file} is missing (${hint})`); process.exit(1) }
}

/** The newest MSVC runtime DLLs of the Visual Studio install (app-local deployment), or [] when none is found. */
function msvcRuntimeDlls() {
  let vsPath = ''
  const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)', 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
  if (fs.existsSync(vswhere)) {
    try { vsPath = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' }).trim() } catch {}
  }
  const redistRoot = path.join(vsPath, 'VC', 'Redist', 'MSVC')
  if (!vsPath || !fs.existsSync(redistRoot)) return []
  const versions = fs.readdirSync(redistRoot).filter((v) => /^\d+\./.test(v)).sort((a, b) => b.localeCompare(a, undefined, { numeric: true }))
  for (const v of versions) {
    const x64 = path.join(redistRoot, v, 'x64')
    if (!fs.existsSync(x64)) continue
    const crt = fs.readdirSync(x64).find((d) => /^Microsoft\.VC\d+\.CRT$/i.test(d))
    if (crt) return fs.readdirSync(path.join(x64, crt)).filter((f) => f.toLowerCase().endsWith('.dll')).map((f) => path.join(x64, crt, f))
  }
  return []
}

need(path.join(coreBuild, 'mapeditor_core.exe'), 'run build_app.bat or build_core.bat')
console.log(`Core from ${coreBuild}`)
need(path.join(shellDist, 'index.html'), 'run build_shell.bat')

console.log('=== Staging the Core and the Shell ===')
fs.rmSync(stage, { recursive: true, force: true })
const stageCore = path.join(stage, 'core'), stageShell = path.join(stage, 'shell')
fs.mkdirSync(stageCore, { recursive: true })
for (const exe of ['mapeditor_core.exe', 'lightbake_tool.exe', 'bsp_export_tool.exe']) {
  if (fs.existsSync(path.join(coreBuild, exe))) fs.copyFileSync(path.join(coreBuild, exe), path.join(stageCore, exe))
}
for (const f of fs.readdirSync(coreBuild)) if (f.toLowerCase().endsWith('.dll')) fs.copyFileSync(path.join(coreBuild, f), path.join(stageCore, f))
const runtime = msvcRuntimeDlls()
if (runtime.length) for (const f of runtime) fs.copyFileSync(f, path.join(stageCore, path.basename(f)))
else console.warn('WARNING: MSVC runtime DLLs not found; the target PC then needs the "Microsoft Visual C++ Redistributable (x64)"')
// the default assets folder (the Core uses ./assets when no game folder is set in Settings); generator scripts stay out
fs.cpSync(path.join(root, 'core', 'assets'), path.join(stageCore, 'assets'), { recursive: true, filter: (src) => !src.toLowerCase().endsWith('.py') })
fs.cpSync(shellDist, stageShell, { recursive: true })

console.log('=== Packaging with Electron ===')
const electronVersion = JSON.parse(fs.readFileSync(path.join(here, 'node_modules', 'electron', 'package.json'), 'utf8')).version
const icon = path.join(here, 'icon.ico')
const extraResource = [stageCore, stageShell, ...(fs.existsSync(icon) ? [icon] : [])]
const [out] = await packager({
  dir: here,
  out: path.join(here, 'out', 'pkg'),
  name: 'LevelEditor',
  executableName: 'LevelEditor',
  platform: 'win32',
  arch: 'x64',
  electronVersion,
  asar: true,
  overwrite: true,
  prune: true,
  ignore: [/^\/out($|\/)/, /^\/tests($|\/)/, /^\/package\.mjs$/, /^\/icon\.ico$/],
  extraResource,
  ...(fs.existsSync(icon) ? { icon } : {}),
  win32metadata: { CompanyName: 'Level Editor', FileDescription: 'Level Editor', ProductName: 'Level Editor', InternalName: 'LevelEditor', OriginalFilename: 'LevelEditor.exe' },
})

fs.rmSync(release, { recursive: true, force: true })
fs.mkdirSync(path.dirname(release), { recursive: true })
fs.renameSync(out, release)
fs.writeFileSync(path.join(release, 'README.txt'), [
  'Level Editor',
  '',
  'Run LevelEditor.exe. Optional arguments: LevelEditor.exe [assetsFolder] [initialMap]',
  'The game folder (assets) is chosen in File > Settings > General. Its settings live in it: editor_settings.json and lighting.json',
  '(next to game_profile.json). resources\\core\\settings.json only remembers which game folder to open and the recent maps.',
  'Without a game folder the bundled resources\\core\\assets is used.',
  'The Core\'s log: %APPDATA%\\LevelEditor\\logs\\core.log. DevTools: F12.',
  'Command line tools: resources\\core\\bsp_export_tool.exe, resources\\core\\lightbake_tool.exe.',
  '',
].join('\r\n'))
fs.rmSync(path.join(here, 'out'), { recursive: true, force: true })
console.log(`\nApp packaged: ${release}\\LevelEditor.exe`)
