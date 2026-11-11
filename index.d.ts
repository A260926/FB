type Rect = {
    x?: number,
    y?: number,
    width?: number,
    height?: number
}

type BasicExtrudeOpt = {
    bevelSize?: number,
    bevelSegments?: number,
    simplify?: number,
    smoothSide?: boolean | 'auto',
    smoothSideThreshold?: number,
    smoothBevel?: boolean,
    excludeBottom?: boolean,
    fitRect?: Rect,
    translate?: ArrayLike<number>,
    scale?: ArrayLike<number>
}

type ExtrudeResult = {
    indices: Uint16Array|Uint32Array,
    position: Float32Array,
    normal: Float32Array,
    uv: Float32Array,
    boundingRect: Rect
}

type Polygon = ArrayLike<ArrayLike<ArrayLike<number>>>;
type Polyline = ArrayLike<ArrayLike<number>>;

type GeoJSONPolygonGeometry = {
    type: 'Polygon',
    coordinates: Polygon
}

type GeoJSONLineStringGeometry = {
    type: 'LineString',
    coordinates: Polyline
}
type GeoJSONMultiPolygonGeometry = {
    type: 'MultiPolygon',
    coordinates: Array<Polygon>
}
type GeoJSONMultiLineStringGeometry = {
    type: 'MultiLineString',
    coordinates: Array<Polyline>
}

type GeoJSONFeature = {
    geometry: GeoJSONPolygonGeometry
        | GeoJSONLineStringGeometry
        | GeoJSONMultiPolygonGeometry
        | GeoJSONMultiLineStringGeometry,
    properties: Object
}

type GeoJSON = {
    features: Array<GeoJSONFeature>
}

interface GeometryExtrudeStatic {

    extrudePolygon(polygons: ArrayLike<Polygon>, opts: BasicExtrudeOpt & {
        depth: ((idx: number) => number) | number
        // Total radians the section rotates from bottom to top. Default to be 0.
        twist?: number,
        // Side segments along height when twist is not zero. Must be a positive integer. Default to be 8.
        twistSegments?: number,
        // 2D rotation center in the transformed (scale/translate/fitRect applied) coordinate.
        // Defaults to the center of the transformed bounding rect of the input polygons.
        twistOrigin?: ArrayLike<number>
    }): ExtrudeResult

    extrudePolyline(polylines: ArrayLike<Polyline>, opts: BasicExtrudeOpt & {
        depth: ((idx: number) => number) | number
        lineWidth?: number
        miterLimit?: number
    }): ExtrudeResult

    extrudeGeoJSON(
        geojson: GeoJSON | GeoJSONPolygonGeometry | GeoJSONLineStringGeometry | GeoJSONMultiLineStringGeometry | GeoJSONMultiPolygonGeometry,
        opts: BasicExtrudeOpt & {
            depth: ((feature: GeoJSONFeature) => number) | number
            lineWidth?: number
            miterLimit?: number
        }
    ): {polygon: ExtrudeResult, polyline: ExtrudeResult}
}

declare const exports: GeometryExtrudeStatic;
export = exports;
export as namespace geometryExtrude;