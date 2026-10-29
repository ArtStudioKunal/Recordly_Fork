/**
 * Geometry for composing multiple physical displays into one recording canvas.
 * Coordinates are in the same desktop coordinate system (including negative origins).
 * Keep this helper independent of Electron so it can be tested on any platform.
 */
export type RecordingDisplay = {
  id: string | number;
  bounds: { x: number; y: number; width: number; height: number };
};

export type ComposedDisplay = {
  id: string | number;
  x: number;
  y: number;
  width: number;
  height: number;
};

export type DisplayComposition = {
  originX: number;
  originY: number;
  width: number;
  height: number;
  displays: ComposedDisplay[];
};

export function composeDisplayLayout(displays: readonly RecordingDisplay[]): DisplayComposition {
  if (displays.length === 0) {
    throw new Error("Select at least one display to record.");
  }
  const ids = new Set<string>();
  for (const display of displays) {
    const { x, y, width, height } = display.bounds;
    if ([x, y, width, height].some((value) => !Number.isFinite(value)) ||
        width <= 0 || height <= 0) {
      throw new Error("Monitor geometry must be finite and have positive dimensions.");
    }
    if (ids.has(String(display.id))) {
      throw new Error("A display cannot be selected more than once.");
    }
    ids.add(String(display.id));
  }
  const originX = Math.min(...displays.map((d) => d.bounds.x));
  const originY = Math.min(...displays.map((d) => d.bounds.y));
  const right = Math.max(...displays.map((d) => d.bounds.x + d.bounds.width));
  const bottom = Math.max(...displays.map((d) => d.bounds.y + d.bounds.height));
  return {
    originX,
    originY,
    width: right - originX,
    height: bottom - originY,
    displays: displays.map(({ id, bounds }) => ({
      id,
      x: bounds.x - originX,
      y: bounds.y - originY,
      width: bounds.width,
      height: bounds.height,
    })),
  };
}
