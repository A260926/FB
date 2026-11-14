---
sidebar_label: ElementHandle.clickablePoint
---

# ElementHandle.clickablePoint() method

Returns a point within the element that can be used to interact with it.

The point is selected from the intersection of the element's border box with the visible areas of every frame it is nested in. Frame borders, padding and independent scrolling are taken into account, as well as CSS transforms on the element and its ancestor frames.

If `offset` is provided, it is interpreted as a local CSS pixel offset from the top-left corner of the element's **untransformed** border box, and then mapped to the main frame. An error is thrown if the resulting point is clipped away by a frame.

Throws if the visible intersection is empty.

### Signature

```typescript
class ElementHandle {
  clickablePoint(offset?: Offset): Promise<Point>;
}
```

## Parameters

<table><thead><tr><th>

Parameter

</th><th>

Type

</th><th>

Description

</th></tr></thead>
<tbody><tr><td>

offset

</td><td>

[Offset](./puppeteer.offset.md)

</td><td>

_(Optional)_

</td></tr>
</tbody></table>
**Returns:**

Promise&lt;[Point](./puppeteer.point.md)&gt;
