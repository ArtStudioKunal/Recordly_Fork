import { describe, expect, it } from "vitest";
import { composeDisplayLayout } from "./displayComposition";

describe("composeDisplayLayout", () => {
  it("places two side-by-side monitors into one canvas", () => {
    expect(composeDisplayLayout([
      { id: 1, bounds: { x: 0, y: 0, width: 1920, height: 1080 } },
      { id: 2, bounds: { x: 1920, y: 0, width: 2560, height: 1440 } },
    ])).toEqual({
      originX: 0, originY: 0, width: 4480, height: 1440,
      displays: [
        { id: 1, x: 0, y: 0, width: 1920, height: 1080 },
        { id: 2, x: 1920, y: 0, width: 2560, height: 1440 },
      ],
    });
  });

  it("normalizes monitors positioned left of and above the primary", () => {
    const layout = composeDisplayLayout([
      { id: "primary", bounds: { x: 0, y: 0, width: 1920, height: 1080 } },
      { id: "secondary", bounds: { x: -1280, y: -200, width: 1280, height: 1024 } },
    ]);
    expect(layout).toMatchObject({
      originX: -1280, originY: -200, width: 3200, height: 1280,
      displays: [
        { id: "primary", x: 1280, y: 200 },
        { id: "secondary", x: 0, y: 0 },
      ],
    });
  });

  it("rejects empty, duplicate, and invalid monitor selections", () => {
    expect(() => composeDisplayLayout([])).toThrow();
    const screen = { id: 1, bounds: { x: 0, y: 0, width: 100, height: 100 } };
    expect(() => composeDisplayLayout([screen, screen])).toThrow();
    expect(() => composeDisplayLayout([
      { id: 1, bounds: { x: 0, y: 0, width: 0, height: 100 } },
    ])).toThrow();
  });
});
