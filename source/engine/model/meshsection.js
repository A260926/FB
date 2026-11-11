import { Coord2D } from '../geometry/coord2d.js';
import { Coord3D, CoordDistance3D, SubCoord3D, CrossVector3D, DotVector3D } from '../geometry/coord3d.js';
import { RGBColor } from './color.js';
import { Mesh } from './mesh.js';
import { Triangle } from './triangle.js';

const SectionDistanceEps = 1e-7;

function IsFiniteNumber (value)
{
    return typeof value === 'number' && Number.isFinite (value);
}

/**
 * Checks a section plane description, and returns it in normalized form. The
 * normal and the constant are normalized together, so the normal doesn't need
 * to be a unit vector. Throws RangeError if the plane is not finite, the
 * normal is a zero vector, or the cap material index is not a non-negative
 * integer (or is out of range when a material count is provided).
 * @param {object} plane The section plane description.
 * @param {Coord3D} plane.normal The normal of the plane. Doesn't need to be a unit vector.
 * @param {number} plane.constant The constant of the plane equation.
 * @param {number} plane.capMaterialIndex The material index of the generated cap faces.
 * @param {number} materialCount Optional material count to check the cap material index against.
 * @returns {object} The normalized section plane description.
 */
export function CheckSectionPlane (plane, materialCount)
{
    if (plane === null || plane === undefined) {
        throw new RangeError ('Invalid section plane.');
    }
    const normal = plane.normal;
    if (normal === null || normal === undefined ||
        !IsFiniteNumber (normal.x) || !IsFiniteNumber (normal.y) || !IsFiniteNumber (normal.z)) {
        throw new RangeError ('Section plane normal is not finite.');
    }
    if (!IsFiniteNumber (plane.constant)) {
        throw new RangeError ('Section plane constant is not finite.');
    }
    if (!Number.isInteger (plane.capMaterialIndex) || plane.capMaterialIndex < 0) {
        throw new RangeError ('Section plane cap material index is not a non-negative integer.');
    }
    const normalLength = Math.sqrt (normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!IsFiniteNumber (normalLength) || normalLength === 0.0) {
        throw new RangeError ('Section plane normal is a zero vector.');
    }
    const result = {
        normal : new Coord3D (
            normal.x / normalLength,
            normal.y / normalLength,
            normal.z / normalLength
        ),
        constant : plane.constant / normalLength,
        capMaterialIndex : plane.capMaterialIndex
    };
    if (!IsFiniteNumber (result.constant)) {
        throw new RangeError ('Section plane constant is not finite.');
    }
    if (materialCount !== null && materialCount !== undefined && result.capMaterialIndex >= materialCount) {
        throw new RangeError ('Section plane cap material index is out of range.');
    }
    return result;
}

/**
 * Checks if a transformation matrix can be used for solid sectioning. Only
 * finite, invertible affine transformations with positive determinant are
 * allowed. Throws RangeError if the transformation is not supported.
 * @param {Matrix} matrix The transformation matrix to check.
 */
export function CheckSectionTransformation (matrix)
{
    if (matrix === null || matrix === undefined || !matrix.IsValid ()) {
        throw new RangeError ('Invalid section transformation.');
    }
    const values = matrix.Get ();
    for (let i = 0; i < 16; i++) {
        if (!IsFiniteNumber (values[i])) {
            throw new RangeError ('Section transformation is not finite.');
        }
    }
    if (values[3] !== 0.0 || values[7] !== 0.0 || values[11] !== 0.0 || values[15] !== 1.0) {
        throw new RangeError ('Section transformation is not affine.');
    }
    const determinant = matrix.Determinant ();
    if (!(determinant > 0.0) || matrix.Invert () === null) {
        throw new RangeError ('Section transformation is not invertible or changes orientation.');
    }
}

/**
 * Transforms a section plane to the local coordinate system of a transformation.
 * @param {Coord3D} normal The normal of the plane.
 * @param {number} constant The constant of the plane equation.
 * @param {Matrix} matrix The local to world transformation matrix.
 * @returns {object} The plane in the local coordinate system.
 */
export function TransformSectionPlaneToLocal (normal, constant, matrix)
{
    const values = matrix.Get ();
    const localNormal = new Coord3D (
        values[0] * normal.x + values[1] * normal.y + values[2] * normal.z,
        values[4] * normal.x + values[5] * normal.y + values[6] * normal.z,
        values[8] * normal.x + values[9] * normal.y + values[10] * normal.z
    );
    const localConstant = constant + normal.x * values[12] + normal.y * values[13] + normal.z * values[14];
    return {
        normal : localNormal,
        constant : localConstant
    };
}

function CheckSectionMesh (mesh)
{
    if (mesh.LineCount () !== 0) {
        throw new RangeError ('Section input mesh contains line elements.');
    }
    for (let vertexIndex = 0; vertexIndex < mesh.VertexCount (); vertexIndex++) {
        const vertex = mesh.GetVertex (vertexIndex);
        if (!IsFiniteNumber (vertex.x) || !IsFiniteNumber (vertex.y) || !IsFiniteNumber (vertex.z)) {
            throw new RangeError ('Section input mesh contains non-finite vertices.');
        }
    }
}

function IsDegenerateTriangle (vertex0, vertex1, vertex2)
{
    const edge0 = SubCoord3D (vertex1, vertex0);
    const edge1 = SubCoord3D (vertex2, vertex0);
    const edgeLengthSquare = Math.max (DotVector3D (edge0, edge0), DotVector3D (edge1, edge1));
    if (edgeLengthSquare === 0.0) {
        return true;
    }
    const normal = CrossVector3D (edge0, edge1);
    return normal.Length () <= 1e-9 * edgeLengthSquare;
}

function SignedArea2D (loop)
{
    let area = 0.0;
    for (let i = 0; i < loop.length; i++) {
        const current = loop[i];
        const next = loop[(i + 1) % loop.length];
        area += current.x * next.y - next.x * current.y;
    }
    return area / 2.0;
}

function Cross2D (a, b, c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

function PointInPolygon2D (point, polygon)
{
    let inside = false;
    for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
        const current = polygon[i];
        const previous = polygon[j];
        if ((current.y > point.y) !== (previous.y > point.y) &&
            point.x < (previous.x - current.x) * (point.y - current.y) / (previous.y - current.y) + current.x) {
            inside = !inside;
        }
    }
    return inside;
}

function PointInTriangle2D (point, a, b, c, eps)
{
    return Cross2D (a, b, point) > eps && Cross2D (b, c, point) > eps && Cross2D (c, a, point) > eps;
}

function PointInTriangle2DLoose (point, a, b, c, eps)
{
    const ab = Cross2D (a, b, point);
    const bc = Cross2D (b, c, point);
    const ca = Cross2D (c, a, point);
    return (ab >= -eps && bc >= -eps && ca >= -eps) || (ab <= eps && bc <= eps && ca <= eps);
}

function GetRayAngle (from, to)
{
    return Math.abs (Math.atan2 (to.y - from.y, to.x - from.x));
}

function GetLoopScale2D (points)
{
    let scale = 1.0;
    for (const point of points) {
        scale = Math.max (scale, Math.abs (point.x), Math.abs (point.y));
    }
    return scale;
}

function TriangulateSimpleLoop2D (points, remaining, areaEps, triangles)
{
    while (remaining.length > 3) {
        let earClipped = false;
        for (let i = 0; i < remaining.length; i++) {
            const prevIndex = remaining[(i + remaining.length - 1) % remaining.length];
            const currentIndex = remaining[i];
            const nextIndex = remaining[(i + 1) % remaining.length];
            if (Cross2D (points[prevIndex], points[currentIndex], points[nextIndex]) <= areaEps) {
                continue;
            }
            let containsPoint = false;
            for (const otherIndex of remaining) {
                if (otherIndex === prevIndex || otherIndex === currentIndex || otherIndex === nextIndex) {
                    continue;
                }
                if (PointInTriangle2D (points[otherIndex], points[prevIndex], points[currentIndex], points[nextIndex], areaEps)) {
                    containsPoint = true;
                    break;
                }
            }
            if (containsPoint) {
                continue;
            }
            triangles.push ([prevIndex, currentIndex, nextIndex]);
            remaining.splice (i, 1);
            earClipped = true;
            break;
        }
        if (!earClipped) {
            // no strict ear found: remove a duplicated point of a hole bridge
            // channel (zero width, so the covered region doesn't change)
            let pointRemoved = false;
            for (let i = 0; i < remaining.length; i++) {
                const currentIndex = remaining[i];
                const hasDuplicate = remaining.some ((otherIndex) => {
                    if (otherIndex === currentIndex) {
                        return false;
                    }
                    const current = points[currentIndex];
                    const other = points[otherIndex];
                    return Math.abs (current.x - other.x) <= areaEps && Math.abs (current.y - other.y) <= areaEps;
                });
                if (hasDuplicate) {
                    remaining.splice (i, 1);
                    pointRemoved = true;
                    break;
                }
            }
            if (!pointRemoved) {
                return false;
            }
        }
    }
    if (remaining.length === 3) {
        triangles.push ([remaining[0], remaining[1], remaining[2]]);
    }
    return true;
}

function PointLiesOnSegment2D (point, start, end, eps)
{
    if (Math.abs (Cross2D (start, end, point)) > eps) {
        return false;
    }
    const dot = (point.x - start.x) * (point.x - end.x) + (point.y - start.y) * (point.y - end.y);
    return dot <= eps;
}

// Ear clipping for one loop that may contain collinear points (for example the
// cut points left by original triangle diagonals). Collinear points are taken
// out for the clipping and re-inserted by splitting the spanning triangles, so
// every boundary edge of the original loop is present in the result.
function TriangulateLoop2D (points)
{
    const areaEps = 1e-10 * GetLoopScale2D (points) ** 2;
    const suppressed = new Set ();
    for (let i = 0; i < points.length; i++) {
        const previous = points[(i + points.length - 1) % points.length];
        const current = points[i];
        const next = points[(i + 1) % points.length];
        if (Math.abs (Cross2D (previous, current, next)) > areaEps) {
            continue;
        }
        const toPrevious = { x : previous.x - current.x, y : previous.y - current.y };
        const toNext = { x : next.x - current.x, y : next.y - current.y };
        if (toPrevious.x * toNext.x + toPrevious.y * toNext.y < 0.0) {
            suppressed.add (i);
        }
    }

    const remaining = points.map ((point, index) => index).filter ((index) => !suppressed.has (index));
    if (remaining.length < 3) {
        return [];
    }

    const triangles = [];
    if (!TriangulateSimpleLoop2D (points, remaining, areaEps, triangles)) {
        return [];
    }

    for (const suppressedIndex of suppressed) {
        const point = points[suppressedIndex];
        let splitDone = true;
        while (splitDone) {
            splitDone = false;
            for (let triangleIndex = 0; triangleIndex < triangles.length; triangleIndex++) {
                const triangle = triangles[triangleIndex];
                for (let edge = 0; edge < 3; edge++) {
                    const startIndex = triangle[edge];
                    const endIndex = triangle[(edge + 1) % 3];
                    const thirdIndex = triangle[(edge + 2) % 3];
                    if (!PointLiesOnSegment2D (point, points[startIndex], points[endIndex], areaEps)) {
                        continue;
                    }
                    // the point is already a vertex of this triangle, nothing to split
                    if (startIndex === suppressedIndex || endIndex === suppressedIndex || thirdIndex === suppressedIndex) {
                        continue;
                    }
                    triangles.splice (triangleIndex, 1,
                        [startIndex, suppressedIndex, thirdIndex],
                        [suppressedIndex, endIndex, thirdIndex]
                    );
                    splitDone = true;
                    break;
                }
                if (splitDone) {
                    break;
                }
            }
        }
    }
    return triangles;
}

function BridgeHoleIntoLoop (outerPoints, holePoints)
{
    const eps = 1e-9 * GetLoopScale2D (outerPoints);

    let holeStart = 0;
    for (let i = 1; i < holePoints.length; i++) {
        const isMoreRight = holePoints[i].x > holePoints[holeStart].x;
        const isSameHeight = holePoints[i].x === holePoints[holeStart].x && holePoints[i].y > holePoints[holeStart].y;
        if (isMoreRight || isSameHeight) {
            holeStart = i;
        }
    }
    const holePoint = holePoints[holeStart];

    let bestEdge = -1;
    let bestX = Infinity;
    for (let edge = 0; edge < outerPoints.length; edge++) {
        const current = outerPoints[edge];
        const next = outerPoints[(edge + 1) % outerPoints.length];
        if ((current.y > holePoint.y) === (next.y > holePoint.y)) {
            continue;
        }
        const t = (holePoint.y - current.y) / (next.y - current.y);
        const x = current.x + t * (next.x - current.x);
        if (x < holePoint.x - eps) {
            continue;
        }
        if (x < bestX) {
            bestX = x;
            bestEdge = edge;
        }
    }
    if (bestEdge === -1) {
        return outerPoints.concat (holePoints);
    }

    const edgeStart = outerPoints[bestEdge];
    const edgeEnd = outerPoints[(bestEdge + 1) % outerPoints.length];
    let visible = null;
    if (Math.abs (edgeStart.x - bestX) <= eps && Math.abs (edgeStart.y - holePoint.y) <= eps) {
        visible = edgeStart;
    } else if (Math.abs (edgeEnd.x - bestX) <= eps && Math.abs (edgeEnd.y - holePoint.y) <= eps) {
        visible = edgeEnd;
    } else {
        visible = (edgeStart.x > edgeEnd.x) ? edgeStart : edgeEnd;
    }

    const intersection = { x : bestX, y : holePoint.y };
    let bestAngle = GetRayAngle (holePoint, visible);
    for (const candidate of outerPoints) {
        if (candidate === visible) {
            continue;
        }
        if (!PointInTriangle2DLoose (candidate, holePoint, intersection, visible, eps)) {
            continue;
        }
        const angle = GetRayAngle (holePoint, candidate);
        if (angle < bestAngle) {
            visible = candidate;
            bestAngle = angle;
        }
    }

    const visibleIndex = outerPoints.indexOf (visible);
    const merged = [];
    for (let i = 0; i <= visibleIndex; i++) {
        merged.push (outerPoints[i]);
    }
    for (let i = 0; i < holePoints.length; i++) {
        merged.push (holePoints[(holeStart + i) % holePoints.length]);
    }
    merged.push (holePoints[holeStart]);
    merged.push (visible);
    for (let i = visibleIndex + 1; i < outerPoints.length; i++) {
        merged.push (outerPoints[i]);
    }
    return merged;
}

class MeshSectioner
{
    constructor (mesh, plane)
    {
        this.mesh = mesh;
        this.plane = plane;
        this.result = new Mesh ();
        this.result.SetName (mesh.GetName ());
        mesh.CloneProperties (this.result);

        this.distances = [];
        this.vertexToPoint = new Map ();
        this.edgeToPoint = new Map ();
        this.colorToIndex = new Map ();
        this.normalToIndex = new Map ();
        this.uvToIndex = new Map ();
        this.capNormalIndex = null;
    }

    Run ()
    {
        CheckSectionMesh (this.mesh);
        for (let vertexIndex = 0; vertexIndex < this.mesh.VertexCount (); vertexIndex++) {
            const vertex = this.mesh.GetVertex (vertexIndex);
            this.distances.push (DotVector3D (this.plane.normal, vertex) + this.plane.constant);
        }
        for (let triangleIndex = 0; triangleIndex < this.mesh.TriangleCount (); triangleIndex++) {
            this.ProcessTriangle (triangleIndex);
        }
        this.GenerateCap ();
        return this.result;
    }

    IsInside (distance)
    {
        return distance <= SectionDistanceEps;
    }

    IsOnPlane (distance)
    {
        return Math.abs (distance) <= SectionDistanceEps;
    }

    SnapToPlane (coord)
    {
        const distance = DotVector3D (this.plane.normal, coord) + this.plane.constant;
        return new Coord3D (
            coord.x - this.plane.normal.x * distance,
            coord.y - this.plane.normal.y * distance,
            coord.z - this.plane.normal.z * distance
        );
    }

    GetPointOfVertex (vertexIndex)
    {
        if (!this.vertexToPoint.has (vertexIndex)) {
            const vertex = this.mesh.GetVertex (vertexIndex);
            let position = vertex.Clone ();
            if (this.IsOnPlane (this.distances[vertexIndex])) {
                position = this.SnapToPlane (vertex);
            }
            this.vertexToPoint.set (vertexIndex, this.result.AddVertex (position));
        }
        return this.vertexToPoint.get (vertexIndex);
    }

    GetPointOfEdge (vertexIndex0, vertexIndex1)
    {
        const minIndex = Math.min (vertexIndex0, vertexIndex1);
        const maxIndex = Math.max (vertexIndex0, vertexIndex1);
        const edgeKey = minIndex * this.mesh.VertexCount () + maxIndex;
        if (!this.edgeToPoint.has (edgeKey)) {
            const distance0 = this.distances[vertexIndex0];
            const distance1 = this.distances[vertexIndex1];
            const t = distance0 / (distance0 - distance1);
            const vertex0 = this.mesh.GetVertex (vertexIndex0);
            const vertex1 = this.mesh.GetVertex (vertexIndex1);
            const position = new Coord3D (
                vertex0.x + (vertex1.x - vertex0.x) * t,
                vertex0.y + (vertex1.y - vertex0.y) * t,
                vertex0.z + (vertex1.z - vertex0.z) * t
            );
            this.edgeToPoint.set (edgeKey, this.result.AddVertex (this.SnapToPlane (position)));
        }
        return this.edgeToPoint.get (edgeKey);
    }

    MapColorIndex (colorIndex)
    {
        if (colorIndex === null) {
            return null;
        }
        if (!this.colorToIndex.has (colorIndex)) {
            this.colorToIndex.set (colorIndex, this.result.AddVertexColor (this.mesh.GetVertexColor (colorIndex).Clone ()));
        }
        return this.colorToIndex.get (colorIndex);
    }

    MapNormalIndex (normalIndex)
    {
        if (normalIndex === null) {
            return null;
        }
        if (!this.normalToIndex.has (normalIndex)) {
            this.normalToIndex.set (normalIndex, this.result.AddNormal (this.mesh.GetNormal (normalIndex).Clone ()));
        }
        return this.normalToIndex.get (normalIndex);
    }

    MapUVIndex (uvIndex)
    {
        if (uvIndex === null) {
            return null;
        }
        if (!this.uvToIndex.has (uvIndex)) {
            this.uvToIndex.set (uvIndex, this.result.AddTextureUV (this.mesh.GetTextureUV (uvIndex).Clone ()));
        }
        return this.uvToIndex.get (uvIndex);
    }

    GetOriginalCorner (triangle, cornerIndex)
    {
        const vertexIndex = [triangle.v0, triangle.v1, triangle.v2][cornerIndex];
        return {
            sourceVertexIndex : vertexIndex,
            pointIndex : null,
            colorIndex : [triangle.c0, triangle.c1, triangle.c2][cornerIndex],
            normalIndex : [triangle.n0, triangle.n1, triangle.n2][cornerIndex],
            uvIndex : [triangle.u0, triangle.u1, triangle.u2][cornerIndex],
            mapped : false
        };
    }

    GetIntersectionCorner (corner0, corner1)
    {
        const distance0 = this.distances[corner0.sourceVertexIndex];
        const distance1 = this.distances[corner1.sourceVertexIndex];
        if (this.IsOnPlane (distance0)) {
            return corner0;
        }
        if (this.IsOnPlane (distance1)) {
            return corner1;
        }
        const t = distance0 / (distance0 - distance1);
        const corner = {
            sourceVertexIndex : null,
            pointIndex : this.GetPointOfEdge (corner0.sourceVertexIndex, corner1.sourceVertexIndex),
            colorIndex : null,
            normalIndex : null,
            uvIndex : null,
            mapped : true
        };
        if (corner0.colorIndex !== null && corner1.colorIndex !== null) {
            const color0 = this.mesh.GetVertexColor (corner0.colorIndex);
            const color1 = this.mesh.GetVertexColor (corner1.colorIndex);
            corner.colorIndex = this.result.AddVertexColor (new RGBColor (
                Math.round (color0.r + (color1.r - color0.r) * t),
                Math.round (color0.g + (color1.g - color0.g) * t),
                Math.round (color0.b + (color1.b - color0.b) * t)
            ));
        }
        if (corner0.normalIndex !== null && corner1.normalIndex !== null) {
            const normal0 = this.mesh.GetNormal (corner0.normalIndex);
            const normal1 = this.mesh.GetNormal (corner1.normalIndex);
            const normal = new Coord3D (
                normal0.x + (normal1.x - normal0.x) * t,
                normal0.y + (normal1.y - normal0.y) * t,
                normal0.z + (normal1.z - normal0.z) * t
            );
            normal.Normalize ();
            corner.normalIndex = this.result.AddNormal (normal);
        }
        if (corner0.uvIndex !== null && corner1.uvIndex !== null) {
            const uv0 = this.mesh.GetTextureUV (corner0.uvIndex);
            const uv1 = this.mesh.GetTextureUV (corner1.uvIndex);
            corner.uvIndex = this.result.AddTextureUV (new Coord2D (
                uv0.x + (uv1.x - uv0.x) * t,
                uv0.y + (uv1.y - uv0.y) * t
            ));
        }
        return corner;
    }

    GetCornerPointIndex (corner)
    {
        if (corner.pointIndex === null) {
            corner.pointIndex = this.GetPointOfVertex (corner.sourceVertexIndex);
        }
        return corner.pointIndex;
    }

    GetCornerColorIndex (corner)
    {
        if (corner.mapped) {
            return corner.colorIndex;
        }
        return this.MapColorIndex (corner.colorIndex);
    }

    GetCornerNormalIndex (corner)
    {
        if (corner.mapped) {
            return corner.normalIndex;
        }
        return this.MapNormalIndex (corner.normalIndex);
    }

    GetCornerUVIndex (corner)
    {
        if (corner.mapped) {
            return corner.uvIndex;
        }
        return this.MapUVIndex (corner.uvIndex);
    }

    ProcessTriangle (triangleIndex)
    {
        const triangle = this.mesh.GetTriangle (triangleIndex);
        const vertexIndices = [triangle.v0, triangle.v1, triangle.v2];
        const distances = vertexIndices.map ((vertexIndex) => this.distances[vertexIndex]);
        const inside = distances.map ((distance) => this.IsInside (distance));
        const insideCount = inside.filter ((isInside) => isInside).length;

        if (insideCount === 0) {
            return;
        }

        const corners = [
            this.GetOriginalCorner (triangle, 0),
            this.GetOriginalCorner (triangle, 1),
            this.GetOriginalCorner (triangle, 2)
        ];

        if (insideCount === 3) {
            this.EmitSidePolygon (corners, triangle);
            return;
        }

        let polygon = [];
        for (let edge = 0; edge < 3; edge++) {
            const current = edge;
            const next = (edge + 1) % 3;
            if (inside[current]) {
                if (inside[next]) {
                    polygon.push (corners[next]);
                } else {
                    polygon.push (this.GetIntersectionCorner (corners[current], corners[next]));
                }
            } else if (inside[next]) {
                polygon.push (this.GetIntersectionCorner (corners[current], corners[next]));
                polygon.push (corners[next]);
            }
        }
        polygon = this.RemoveDuplicateCorners (polygon);
        if (polygon.length < 3) {
            return;
        }
        this.EmitSidePolygon (polygon, triangle);
    }

    RemoveDuplicateCorners (polygon)
    {
        const result = [];
        for (const corner of polygon) {
            if (result.length > 0) {
                const last = result[result.length - 1];
                if (this.GetCornerPointIndex (last) === this.GetCornerPointIndex (corner)) {
                    continue;
                }
            }
            result.push (corner);
        }
        if (result.length > 1) {
            const first = result[0];
            const last = result[result.length - 1];
            if (this.GetCornerPointIndex (first) === this.GetCornerPointIndex (last)) {
                result.pop ();
            }
        }
        return result;
    }

    EmitSidePolygon (polygon, sourceTriangle)
    {
        for (let i = 1; i + 1 < polygon.length; i++) {
            this.EmitSideTriangle (polygon[0], polygon[i], polygon[i + 1], sourceTriangle);
        }
    }

    EmitSideTriangle (corner0, corner1, corner2, sourceTriangle)
    {
        const pointIndex0 = this.GetCornerPointIndex (corner0);
        const pointIndex1 = this.GetCornerPointIndex (corner1);
        const pointIndex2 = this.GetCornerPointIndex (corner2);
        if (IsDegenerateTriangle (this.result.GetVertex (pointIndex0), this.result.GetVertex (pointIndex1), this.result.GetVertex (pointIndex2))) {
            return;
        }
        const triangle = new Triangle (pointIndex0, pointIndex1, pointIndex2);
        triangle.SetVertexColors (
            this.GetCornerColorIndex (corner0),
            this.GetCornerColorIndex (corner1),
            this.GetCornerColorIndex (corner2)
        );
        triangle.SetNormals (
            this.GetCornerNormalIndex (corner0),
            this.GetCornerNormalIndex (corner1),
            this.GetCornerNormalIndex (corner2)
        );
        triangle.SetTextureUVs (
            this.GetCornerUVIndex (corner0),
            this.GetCornerUVIndex (corner1),
            this.GetCornerUVIndex (corner2)
        );
        triangle.SetMaterial (sourceTriangle.mat);
        triangle.SetCurve (sourceTriangle.curve);
        this.result.AddTriangle (triangle);
    }

    GetPositionKey (pointIndex)
    {
        const vertex = this.result.GetVertex (pointIndex);
        const quantize = (value) => Math.round (value / SectionDistanceEps).toString ();
        return quantize (vertex.x) + ',' + quantize (vertex.y) + ',' + quantize (vertex.z);
    }

    GenerateCap ()
    {
        const boundaryEdges = this.GetSideBoundaryEdgesOnPlane ();
        if (boundaryEdges.length === 0) {
            return;
        }
        const loops = this.BuildCapLoops (boundaryEdges);
        if (loops.length === 0) {
            return;
        }

        const frame = this.GetPlaneFrame ();
        const loops2D = loops.map ((loop) => this.ProjectLoop (loop, frame));
        const depths = loops2D.map ((loop, loopIndex) => {
            let depth = 0;
            for (let otherIndex = 0; otherIndex < loops2D.length; otherIndex++) {
                if (otherIndex === loopIndex) {
                    continue;
                }
                if (PointInPolygon2D (loop[0], loops2D[otherIndex])) {
                    depth += 1;
                }
            }
            return depth;
        });

        const groups = new Map ();
        loops2D.forEach ((loop, loopIndex) => {
            if (depths[loopIndex] % 2 === 0) {
                if (SignedArea2D (loop) < 0.0) {
                    loop.reverse ();
                }
                groups.set (loopIndex, {
                    outer : loop,
                    holes : []
                });
            }
        });
        loops2D.forEach ((loop, loopIndex) => {
            if (depths[loopIndex] % 2 === 1) {
                if (SignedArea2D (loop) > 0.0) {
                    loop.reverse ();
                }
                for (const [outerIndex, group] of groups) {
                    if (depths[outerIndex] === depths[loopIndex] - 1 && PointInPolygon2D (loop[0], loops2D[outerIndex])) {
                        group.holes.push (loop);
                        break;
                    }
                }
            }
        });

        for (const group of groups.values ()) {
            this.EmitCapGroup (group);
        }
    }

    GetSideBoundaryEdgesOnPlane ()
    {
        // The clipped side mesh is closed everywhere except on the cut plane.
        // Its unpaired directed edges are therefore exactly the cap boundaries.
        // Geometric positions are used as keys, so edges emitted by different
        // faces at the same seam position are treated as one edge.
        const directedEdges = new Map ();
        const edgeData = new Map ();
        for (let triangleIndex = 0; triangleIndex < this.result.TriangleCount (); triangleIndex++) {
            const triangle = this.result.GetTriangle (triangleIndex);
            const pointIndices = [triangle.v0, triangle.v1, triangle.v2];
            for (let edge = 0; edge < 3; edge++) {
                const from = pointIndices[edge];
                const to = pointIndices[(edge + 1) % 3];
                const fromKey = this.GetPositionKey (from);
                const toKey = this.GetPositionKey (to);
                if (fromKey === toKey) {
                    continue;
                }
                const directedKey = fromKey + '>' + toKey;
                directedEdges.set (directedKey, true);
                if (!edgeData.has (directedKey)) {
                    edgeData.set (directedKey, {
                        from : from,
                        to : to,
                        fromKey : fromKey,
                        toKey : toKey
                    });
                }
            }
        }
        const boundaryEdges = [];
        for (const [directedKey, edge] of edgeData) {
            const reversedKey = directedKey.split ('>').reverse ().join ('>');
            if (directedEdges.has (reversedKey)) {
                continue;
            }
            const fromVertex = this.result.GetVertex (edge.from);
            const toVertex = this.result.GetVertex (edge.to);
            if (this.IsOnPlane (DotVector3D (this.plane.normal, fromVertex) + this.plane.constant) &&
                this.IsOnPlane (DotVector3D (this.plane.normal, toVertex) + this.plane.constant)) {
                boundaryEdges.push (edge);
            }
        }
        return boundaryEdges;
    }

    BuildCapLoops (boundaryEdges)
    {
        const startMap = new Map ();
        boundaryEdges.forEach ((edge, edgeIndex) => {
            if (!startMap.has (edge.fromKey)) {
                startMap.set (edge.fromKey, []);
            }
            startMap.get (edge.fromKey).push (edgeIndex);
        });
        const visited = new Array (boundaryEdges.length).fill (false);
        const loops = [];
        for (let start = 0; start < boundaryEdges.length; start++) {
            if (visited[start]) {
                continue;
            }
            const startEdge = boundaryEdges[start];
            const loop = [startEdge.from, startEdge.to];
            visited[start] = true;
            let currentKey = startEdge.toKey;
            while (currentKey !== startEdge.fromKey) {
                const candidates = startMap.get (currentKey);
                let nextEdge = -1;
                if (candidates !== undefined) {
                    for (const candidate of candidates) {
                        if (!visited[candidate]) {
                            nextEdge = candidate;
                            break;
                        }
                    }
                }
                if (nextEdge === -1) {
                    break;
                }
                visited[nextEdge] = true;
                loop.push (boundaryEdges[nextEdge].to);
                currentKey = boundaryEdges[nextEdge].toKey;
            }
            if (currentKey !== startEdge.fromKey) {
                continue;
            }
            loop.pop ();
            const cleanedLoop = this.CleanCapLoop (loop);
            if (cleanedLoop.length >= 3) {
                loops.push (cleanedLoop);
            }
        }
        return loops;
    }

    CleanCapLoop (loop)
    {
        // Only duplicate positions are removed here. Collinear cut points (for
        // example the ones left by the diagonals of the original triangulation)
        // must be kept so the cap edges exactly match the cut edges of the sides.
        const cleaned = [];
        for (const pointIndex of loop) {
            if (cleaned.length > 0) {
                const last = this.result.GetVertex (cleaned[cleaned.length - 1]);
                const current = this.result.GetVertex (pointIndex);
                if (CoordDistance3D (last, current) <= SectionDistanceEps) {
                    continue;
                }
            }
            cleaned.push (pointIndex);
        }
        while (cleaned.length > 1) {
            const first = this.result.GetVertex (cleaned[0]);
            const last = this.result.GetVertex (cleaned[cleaned.length - 1]);
            if (CoordDistance3D (first, last) > SectionDistanceEps) {
                break;
            }
            cleaned.pop ();
        }
        return cleaned;
    }

    GetPlaneFrame ()
    {
        const normal = this.plane.normal;
        const absX = Math.abs (normal.x);
        const absY = Math.abs (normal.y);
        const absZ = Math.abs (normal.z);
        let axis = null;
        if (absX <= absY && absX <= absZ) {
            axis = new Coord3D (1.0, 0.0, 0.0);
        } else if (absY <= absZ) {
            axis = new Coord3D (0.0, 1.0, 0.0);
        } else {
            axis = new Coord3D (0.0, 0.0, 1.0);
        }
        const u = CrossVector3D (axis, normal);
        u.Normalize ();
        const v = CrossVector3D (normal, u);
        return {
            u : u,
            v : v
        };
    }

    ProjectLoop (loop, frame)
    {
        return loop.map ((pointIndex) => {
            const vertex = this.result.GetVertex (pointIndex);
            return {
                x : DotVector3D (vertex, frame.u),
                y : DotVector3D (vertex, frame.v),
                pointIndex : pointIndex
            };
        });
    }

    EmitCapGroup (group)
    {
        let merged = group.outer;
        const holes = [...group.holes];
        holes.sort ((a, b) => {
            const rightmost = (loop) => Math.max (...loop.map ((point) => point.x));
            return rightmost (b) - rightmost (a);
        });
        for (const hole of holes) {
            merged = BridgeHoleIntoLoop (merged, hole);
        }
        const triangles = TriangulateLoop2D (merged);
        if (triangles.length === 0) {
            return;
        }
        if (this.capNormalIndex === null) {
            this.capNormalIndex = this.result.AddNormal (this.plane.normal.Clone ());
        }
        for (const triangleIndices of triangles) {
            const pointIndex0 = merged[triangleIndices[0]].pointIndex;
            const pointIndex1 = merged[triangleIndices[1]].pointIndex;
            const pointIndex2 = merged[triangleIndices[2]].pointIndex;
            const vertex0 = this.result.GetVertex (pointIndex0);
            const vertex1 = this.result.GetVertex (pointIndex1);
            const vertex2 = this.result.GetVertex (pointIndex2);
            if (IsDegenerateTriangle (vertex0, vertex1, vertex2)) {
                continue;
            }
            const normal = CrossVector3D (SubCoord3D (vertex1, vertex0), SubCoord3D (vertex2, vertex0));
            let orderedPointIndices = [pointIndex0, pointIndex1, pointIndex2];
            if (DotVector3D (normal, this.plane.normal) < 0.0) {
                orderedPointIndices = [pointIndex0, pointIndex2, pointIndex1];
            }
            const capTriangle = new Triangle (orderedPointIndices[0], orderedPointIndices[1], orderedPointIndices[2]);
            capTriangle.SetNormals (this.capNormalIndex, this.capNormalIndex, this.capNormalIndex);
            capTriangle.SetMaterial (this.plane.capMaterialIndex);
            this.result.AddTriangle (capTriangle);
        }
    }
}

/**
 * Cuts a closed solid triangle mesh with a plane, and closes the cut with cap
 * faces so the result is a closed solid again. The part of the solid is kept
 * where `normal * p + constant <= 0`. The plane normal doesn't need to be a
 * unit vector, it's normalized together with the constant. The plane object
 * also contains the material index of the generated cap faces.
 *
 * The input must be a closed triangle mesh with finite coordinates, outward
 * orientation, and no line elements. Disjoint shells, concave sections,
 * cavities, and solid islands inside cavities are handled. The original mesh
 * is not modified, and the result is an independent mesh. Cap faces get the
 * cap material index, the plane unit normal, and no texture coordinates or
 * vertex colors.
 * @param {Mesh} mesh The closed solid mesh to cut.
 * @param {object} plane The section plane description.
 * @param {Coord3D} plane.normal The normal of the plane. Doesn't need to be a unit vector.
 * @param {number} plane.constant The constant of the plane equation.
 * @param {number} plane.capMaterialIndex The material index of the generated cap faces.
 * @returns {Mesh} The sectioned solid mesh.
 */
export function ClipMeshByPlane (mesh, plane)
{
    const normalizedPlane = CheckSectionPlane (plane, null);
    const sectioner = new MeshSectioner (mesh, normalizedPlane);
    return sectioner.Run ();
}
