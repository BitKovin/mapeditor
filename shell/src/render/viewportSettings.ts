// Every viewport has its own settings (DESIGN.md §6.1/§6.2). 'unlit' and 'lightingOnly' (Core's live lightmaps) are implemented;
// the enum and the plumbing exist so adding the others is a new case + a shader path.
export type RenderMode = 'unlit' | 'lit' | 'wireframe' | 'lightingOnly'
export const RENDER_MODES: { id: RenderMode; label: string; implemented: boolean }[] = [
  { id: 'unlit', label: 'Unlit', implemented: true },
  { id: 'lit', label: 'Lit', implemented: true },
  { id: 'wireframe', label: 'Wireframe', implemented: true },
  { id: 'lightingOnly', label: 'Lighting only', implemented: true },
]
/** Grid sizes in metres. */
/** Rotation snap (degrees) and scale snap (factor step) are separate from the position grid (DESIGN 18.3). */
export const ROT_SNAPS = [1, 5, 10, 15, 22.5, 30, 45, 90]
export const SCALE_SNAPS = [0.01, 0.05, 0.1, 0.25, 0.5, 1]
/** Snap values for the modal rotate / scale; 0 = free (when snapping is off). */
export const rotSnapRad = (s: ViewportSettings): number => (s.snapEnabled ? (s.rotSnap * Math.PI) / 180 : 0)
export const scaleSnapStep = (s: ViewportSettings): number => (s.snapEnabled ? s.scaleSnap : 0)
export const GRID_SIZES = [0.1, 0.25, 0.5, 1, 2, 4, 5, 10, 15, 20, 50, 100, 250, 500]
export type CameraMode = 'fly' | 'orbit' | 'top' | 'front' | 'side'
export const isOrthoMode = (m: CameraMode): m is 'top' | 'front' | 'side' => m === 'top' || m === 'front' || m === 'side'
/** Fly speed range in metres per second; the slider is logarithmic, the wheel (RMB held) multiplies by WHEEL_SPEED_FACTOR. */
export const FLY_SPEED_MIN = 0.05, FLY_SPEED_MAX = 500, WHEEL_SPEED_FACTOR = 1.2
export const clampSpeed = (v: number): number => Math.min(FLY_SPEED_MAX, Math.max(FLY_SPEED_MIN, Number.isFinite(v) ? v : 8))
export interface ViewportSettings { cameraMode: CameraMode; flySpeed: number; showOutlines: boolean; showSubtractive: boolean; showVolumes: boolean; backfaceCulling: boolean; renderMode: RenderMode; showGrid: boolean; showOrigins: boolean; showDirections: boolean; showLinks: boolean; showLightGizmos: boolean; showLightvol: boolean; showVisibility: boolean; showLightmapDensity: boolean; showTexelHeat: boolean; showSizes: boolean; snapEnabled: boolean; gridSize: number; rotSnap: number; scaleSnap: number; gizmo: 'move' | 'rotate' | 'scale' }
export const defaultViewportSettings = (): ViewportSettings => ({ cameraMode: 'fly', flySpeed: 8, showOutlines: false, showSubtractive: true, showVolumes: true, backfaceCulling: true, renderMode: 'unlit', showGrid: true, showOrigins: true, showDirections: true, showLinks: true, showLightGizmos: true, showLightvol: false, showVisibility: false, showLightmapDensity: false, showTexelHeat: false, showSizes: true, snapEnabled: true, gridSize: 0.5, rotSnap: 15, scaleSnap: 0.1, gizmo: 'move' })
/** The snap step to use, 0 meaning free movement. */
export const snapStep = (s: ViewportSettings): number => (s.snapEnabled ? s.gridSize : 0)
export const isImplemented = (m: RenderMode): boolean => RENDER_MODES.find((x) => x.id === m)?.implemented ?? false
