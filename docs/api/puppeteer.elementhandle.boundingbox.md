---
sidebar_label: ElementHandle.boundingBox
---

# ElementHandle.boundingBox() method

This method returns the bounding box of the element (relative to the main frame), or `null` if the element is [not part of the layout](https://drafts.csswg.org/css-display-4/#box-generation) (example: `display: none`).

The result is the axis-aligned bounding box of the element's complete border box projected into the main frame's viewport. Frame borders, padding, independent scrolling and CSS transforms on the element and on any ancestor frames are all accounted for. The box is not clipped to the visible area; see [ElementHandle.clickablePoint()](./puppeteer.elementhandle.clickablepoint.md) for that.

### Signature

```typescript
class ElementHandle {
  boundingBox(): Promise<BoundingBox | null>;
}
```

**Returns:**

Promise&lt;[BoundingBox](./puppeteer.boundingbox.md) \| null&gt;
