// Key bindings. Blender/UE conventions (DESIGN.md §17). `implemented: false` entries are shown in the
// Mesh Edit tab so the planned workflow is visible, but do nothing until M3.
export interface ToolDef { id: string; label: string; keys: string; implemented: boolean; hint: string }

export const MESH_TOOLS: ToolDef[] = [
  { id: 'grab', label: 'Move', keys: 'G', implemented: true, hint: 'Move the selection; X/Y/Z constrain, type a number' },
  { id: 'rotate', label: 'Rotate', keys: 'R', implemented: true, hint: 'Rotate around the view (or X/Y/Z), type degrees' },
  { id: 'scale', label: 'Scale', keys: 'S', implemented: true, hint: 'Scale from the selection centre; X/Y/Z constrain' },
  { id: 'extrude', label: 'Extrude', keys: 'E', implemented: true, hint: 'Pull the selected faces out (or into the brush), then click' },
  { id: 'inset', label: 'Inset', keys: 'I', implemented: true, hint: 'Inset the selected faces' },
  { id: 'bevel', label: 'Bevel', keys: 'Ctrl+B', implemented: true, hint: 'Chamfer the selected edges' },
  { id: 'loopcut', label: 'Loop Cut', keys: 'Ctrl+R', implemented: true, hint: 'Hover an edge, click, slide, click' },
  { id: 'merge', label: 'Merge', keys: 'M', implemented: true, hint: 'Weld the selected vertices at their centre' },
  { id: 'dissolve', label: 'Dissolve', keys: 'X', implemented: true, hint: 'Remove the selection, merging the faces around it' },
  { id: 'fill', label: 'Fill', keys: 'F', implemented: true, hint: 'Close a hole from the selected vertices' },
  { id: 'delete', label: 'Delete faces', keys: 'Del', implemented: true, hint: 'Delete the faces touching the selection (leaves a hole; not for convex brushes)' },
  { id: 'flip', label: 'Flip normals', keys: 'Alt+N', implemented: true, hint: 'Flip the selected faces' },
  { id: 'recalc', label: 'Recalculate normals', keys: 'Shift+N', implemented: true, hint: 'Make all faces point outward' },
  { id: 'selectall', label: 'Select all', keys: 'A', implemented: true, hint: '' },
  { id: 'invert', label: 'Invert selection', keys: 'Ctrl+I', implemented: true, hint: '' },
  { id: 'grow', label: 'Grow', keys: '+', implemented: true, hint: 'Grow the selection by one ring' },
  { id: 'shrink', label: 'Shrink', keys: '-', implemented: true, hint: 'Shrink the selection by one ring' },
  { id: 'knife', label: 'Knife', keys: 'K', implemented: true, hint: 'Click points on edges or vertices (Ctrl = edge middle); Enter or right-click cuts, Esc cancels' },
  { id: 'bridge', label: 'Bridge Edge Loops', keys: '', implemented: false, hint: 'Connect two edge loops' },
  { id: 'mirror', label: 'Mirror', keys: 'Ctrl+M', implemented: true, hint: 'Then X, Y or Z: mirror the selection about its centre' },
  { id: 'proportional', label: 'Proportional editing', keys: 'O', implemented: true, hint: 'Toggle: nearby vertices follow G / R / S with a smooth falloff' },
  { id: 'connect', label: 'Connect vertices', keys: 'J', implemented: true, hint: 'Cut each face between the selected vertices it holds' },
  { id: 'subdivide', label: 'Subdivide', keys: '', implemented: true, hint: 'Edge mode: put new vertices on the selected edges and join them across faces (connect edges)' },
]

/** Radiant-style whole-brush tools (Brush mode). `id` is passed to the viewport as `brush:<id>`. */
export const BRUSH_TOOLS: ToolDef[] = [
  { id: 'clipper', label: 'Clipper', keys: 'X', implemented: true, hint: 'Click 2-3 points, then Enter keeps the back side, Shift+Enter splits, Ctrl+Enter keeps the front' },
  { id: 'hollow', label: 'Make hollow', keys: 'Ctrl+H', implemented: true, hint: 'Turn the brush into walls of the given thickness' },
  { id: 'merge', label: 'Merge brushes', keys: 'Ctrl+M', implemented: true, hint: 'Weld the selected brushes into one convex brush (refused if the result would not be convex)' },
  { id: 'snap', label: 'Snap to grid', keys: 'Ctrl+G', implemented: true, hint: 'Snap every vertex of the selected brushes to the grid' },
  { id: 'selall', label: 'Select all', keys: 'Ctrl+A', implemented: true, hint: 'Select every brush, point entity and prefab instance (Alt+A or Esc clears the selection)' },
  { id: 'selinv', label: 'Invert selection', keys: 'Ctrl+I', implemented: true, hint: 'Select everything that is not selected now' },
  { id: 'selinside', label: 'Select inside', keys: '', implemented: true, hint: 'Select every other brush that lies completely inside the selected brush(es)' },
  { id: 'seltouch', label: 'Select touching', keys: '', implemented: true, hint: 'Select every other brush that touches or overlaps the selected brush(es)' },
  { id: 'flipx', label: 'Flip X', keys: '', implemented: true, hint: 'Mirror the selection along X about its centre' },
  { id: 'flipy', label: 'Flip Y', keys: '', implemented: true, hint: 'Mirror the selection along Y (up) about its centre' },
  { id: 'flipz', label: 'Flip Z', keys: '', implemented: true, hint: 'Mirror the selection along Z about its centre' },
  { id: 'rotx', label: 'Rotate 90° X', keys: '', implemented: true, hint: 'Rotate the selection 90° about X' },
  { id: 'roty', label: 'Rotate 90° Y', keys: '', implemented: true, hint: 'Rotate the selection 90° about Y (up)' },
  { id: 'rotz', label: 'Rotate 90° Z', keys: '', implemented: true, hint: 'Rotate the selection 90° about Z' },
]

export const SHAPES: { id: string; label: string; implemented: boolean; keys?: string }[] = [
  { id: 'box', label: 'Box', implemented: true, keys: 'B' },
  { id: 'cylinder', label: 'Cylinder', implemented: true },
  { id: 'wedge', label: 'Wedge', implemented: true },
  { id: 'arch', label: 'Arch', implemented: true },
  { id: 'stairs', label: 'Stairs', implemented: true },
  { id: 'sphere', label: 'Sphere', implemented: false },
]

// Placeable point entities (Quake 3 classnames). Fallback only: when the game ships an entities .ent file (Core sends `entity_defs`) the Place tab uses that instead.
export const ENTITIES: { classname: string; label: string; keyvalues?: Record<string, string> }[] = [
  { classname: 'info_player_start', label: 'Player Start' },
  { classname: 'info_player_deathmatch', label: 'Deathmatch Spawn' },
  { classname: 'info_notnull', label: 'Info Notnull' },
  { classname: 'target_position', label: 'Target Position' },
  { classname: 'misc_model', label: 'Model', keyvalues: { model: '' } },
]
/** The modern light entities (core/assets/entities.ent has their typed keys; spot / sun / rect start pointing down). */
export const LIGHTS: { classname: string; label: string; sub: string }[] = [
  { classname: 'light_point', label: 'Point Light', sub: 'shines in every direction' },
  { classname: 'light_spot', label: 'Spot Light', sub: 'a cone along its arrow' },
  { classname: 'light_directional', label: 'Directional Light', sub: 'the sun: parallel rays' },
  { classname: 'light_rect', label: 'Rect Light', sub: 'a width x height panel' },
  { classname: 'light_sky', label: 'Sky Light', sub: 'the upper hemisphere' },
]
