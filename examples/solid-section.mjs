// Solid section example: cuts the rectangular tube of the section fixture with
// a plane, renders walls, caps, the open bore and an oblique cut to an SVG
// file, and prints the measured solid volume and cap area.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as OV from '../source/engine/main.js';
import { MakeSectionFixture } from '../checks/section-fixture.mjs';

const currentDir = path.dirname (fileURLToPath (import.meta.url));
const outputPath = path.join (currentDir, 'solid-section.svg');

const { mesh } = MakeSectionFixture ();
const capMaterialIndex = 1;

const horizontalCut = OV.ClipMeshByPlane (mesh, {
    normal : new OV.Coord3D (0.0, 0.0, 1.0),
    constant : -2.0,
    capMaterialIndex : capMaterialIndex
});
const obliqueCut = OV.ClipMeshByPlane (mesh, {
    normal : new OV.Coord3D (0.0, 0.6, 0.8),
    constant : -2.4,
    capMaterialIndex : capMaterialIndex
});

function GetMaterialArea (sectionedMesh, materialIndex)
{
    let area = 0.0;
    for (let triangleIndex = 0; triangleIndex < sectionedMesh.TriangleCount (); triangleIndex++) {
        const triangle = sectionedMesh.GetTriangle (triangleIndex);
        if (triangle.mat !== materialIndex) {
            continue;
        }
        area += OV.GetTriangleArea (
            sectionedMesh.GetVertex (triangle.v0),
            sectionedMesh.GetVertex (triangle.v1),
            sectionedMesh.GetVertex (triangle.v2)
        );
    }
    return area;
}

const horizontalVolume = OV.CalculateVolume (horizontalCut);
const horizontalCapArea = GetMaterialArea (horizontalCut, capMaterialIndex);
const obliqueVolume = OV.CalculateVolume (obliqueCut);
const obliqueCapArea = GetMaterialArea (obliqueCut, capMaterialIndex);

const WallColor = '#508cbe';
const CapColor = '#f0aa28';
const EdgeColor = '#243447';
const BoreColor = '#ffffff';

function MeshTriangles (sectionedMesh)
{
    const triangles = [];
    for (let triangleIndex = 0; triangleIndex < sectionedMesh.TriangleCount (); triangleIndex++) {
        const triangle = sectionedMesh.GetTriangle (triangleIndex);
        triangles.push ({
            points : [triangle.v0, triangle.v1, triangle.v2].map ((vertexIndex) => sectionedMesh.GetVertex (vertexIndex)),
            isCap : triangle.mat === capMaterialIndex
        });
    }
    return triangles;
}

function ProjectedArea2D (points)
{
    let area = 0.0;
    for (let i = 0; i < points.length; i++) {
        const current = points[i];
        const next = points[(i + 1) % points.length];
        area += current.x * next.y - next.x * current.y;
    }
    return Math.abs (area / 2.0);
}

function CreatePanel (originX, originY, width, height, project)
{
    return {
        originX : originX,
        originY : originY,
        width : width,
        height : height,
        project : project,
        bounds : null,
        scale : 1.0,
        polygons : []
    };
}

function AddMeshToPanel (panel, sectionedMesh)
{
    const triangles = MeshTriangles (sectionedMesh);
    for (const triangle of triangles) {
        const projected = triangle.points.map (panel.project);
        for (const point of projected) {
            if (panel.bounds === null) {
                panel.bounds = {
                    minX : point.x, maxX : point.x,
                    minY : point.y, maxY : point.y
                };
            } else {
                panel.bounds.minX = Math.min (panel.bounds.minX, point.x);
                panel.bounds.maxX = Math.max (panel.bounds.maxX, point.x);
                panel.bounds.minY = Math.min (panel.bounds.minY, point.y);
                panel.bounds.maxY = Math.max (panel.bounds.maxY, point.y);
            }
        }
    }

    const margin = 24.0;
    const titleGap = 24.0;
    const scaleX = (panel.width - 2.0 * margin) / (panel.bounds.maxX - panel.bounds.minX);
    const scaleY = (panel.height - 2.0 * margin - titleGap) / (panel.bounds.maxY - panel.bounds.minY);
    panel.scale = Math.min (scaleX, scaleY);

    const projectPoint = (point) => {
        const projected = panel.project (point);
        return {
            x : panel.originX + margin + (projected.x - panel.bounds.minX) * panel.scale,
            y : panel.originY + margin + titleGap + (projected.y - panel.bounds.minY) * panel.scale
        };
    };
    panel.projectPoint = projectPoint;

    for (const triangle of triangles) {
        const points = triangle.points.map (projectPoint);
        // Faces seen edge-on collapse to lines and only clutter the drawing.
        if (ProjectedArea2D (points) < 1e-6) {
            continue;
        }
        panel.polygons.push ({
            points : points,
            fill : triangle.isCap ? CapColor : WallColor
        });
    }
}

function AddBoreOutline (panel)
{
    // The center bore of the tube is [-1, 1] x [-1, 1]; it stays an open hole.
    const corners = [
        new OV.Coord3D (-1.0, -1.0, 2.0),
        new OV.Coord3D (1.0, -1.0, 2.0),
        new OV.Coord3D (1.0, 1.0, 2.0),
        new OV.Coord3D (-1.0, 1.0, 2.0)
    ];
    return corners.map ((corner) => panel.projectPoint (corner));
}

const panelWidth = 340.0;
const panelHeight = 300.0;
const panelGap = 30.0;
const svgWidth = panelWidth * 3 + panelGap * 4;
const svgHeight = panelHeight + 190.0;

const topPanel = CreatePanel (panelGap, 70.0, panelWidth, panelHeight,
    (point) => ({ x : point.x, y : -point.y }));
AddMeshToPanel (topPanel, horizontalCut);
const boreOutline = AddBoreOutline (topPanel);

const sidePanel = CreatePanel (panelGap * 2 + panelWidth, 70.0, panelWidth, panelHeight,
    (point) => ({ x : point.x, y : -point.z }));
AddMeshToPanel (sidePanel, horizontalCut);

const obliquePanel = CreatePanel (panelGap * 3 + panelWidth * 2, 70.0, panelWidth, panelHeight,
    (point) => ({ x : point.x, y : -point.z }));
AddMeshToPanel (obliquePanel, obliqueCut);

function PolygonTag (polygon)
{
    const points = polygon.points.map ((point) => point.x.toFixed (2) + ',' + point.y.toFixed (2)).join (' ');
    return `<polygon points="${points}" fill="${polygon.fill}" stroke="${EdgeColor}" stroke-width="0.6" stroke-linejoin="round"/>`;
}

function PanelFrame (originX, title, subtitle)
{
    const frame = [];
    frame.push (`<text x="${originX + panelWidth / 2.0}" y="40.0" text-anchor="middle" class="title">${title}</text>`);
    frame.push (`<text x="${originX + panelWidth / 2.0}" y="60.0" text-anchor="middle" class="subtitle">${subtitle}</text>`);
    frame.push (`<rect x="${originX}" y="70.0" width="${panelWidth}" height="${panelHeight}" fill="#f7f9fb" stroke="#c7d0da" stroke-width="1"/>`);
    return frame.join ('\n');
}

const borePoints = boreOutline.map ((point) => point.x.toFixed (2) + ',' + point.y.toFixed (2)).join (' ');

const svg = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="${svgWidth}" height="${svgHeight}" viewBox="0 0 ${svgWidth} ${svgHeight}">
<style>
.title { font-family: sans-serif; font-size: 17px; font-weight: bold; fill: #1f2d3d; }
.subtitle { font-family: sans-serif; font-size: 12px; fill: #5a6b7d; }
.legend { font-family: sans-serif; font-size: 13px; fill: #1f2d3d; }
.note { font-family: sans-serif; font-size: 12px; fill: #5a6b7d; }
</style>
${PanelFrame (panelGap, 'Section cap (top view)', 'wall ring is capped, the bore stays open')}
${topPanel.polygons.map (PolygonTag).join ('\n')}
<polygon points="${borePoints}" fill="${BoreColor}" stroke="#b03a2e" stroke-width="1.4" stroke-dasharray="5 4"/>
<text x="${panelGap + 12.0}" y="${70.0 + panelHeight - 12.0}" class="note">dashed rectangle: open center bore</text>
${PanelFrame (panelGap * 2 + panelWidth, 'Horizontal section at z = 2', 'walls keep their material, cap faces close the solid')}
${sidePanel.polygons.map (PolygonTag).join ('\n')}
${PanelFrame (panelGap * 3 + panelWidth * 2, 'Oblique section', 'plane normal (0, 0.6, 0.8), cap faces follow the plane')}
${obliquePanel.polygons.map (PolygonTag).join ('\n')}
<rect x="${panelGap}" y="${70.0 + panelHeight + 24.0}" width="16.0" height="16.0" fill="${WallColor}" stroke="${EdgeColor}" stroke-width="0.6"/>
<text x="${panelGap + 24.0}" y="${70.0 + panelHeight + 37.0}" class="legend">original tube wall</text>
<rect x="${panelGap + 200.0}" y="${70.0 + panelHeight + 24.0}" width="16.0" height="16.0" fill="${CapColor}" stroke="${EdgeColor}" stroke-width="0.6"/>
<text x="${panelGap + 224.0}" y="${70.0 + panelHeight + 37.0}" class="legend">generated cap face</text>
<text x="${panelGap}" y="${70.0 + panelHeight + 70.0}" class="note">horizontal cut volume: ${horizontalVolume.toFixed (4)}, cap area: ${horizontalCapArea.toFixed (4)}</text>
<text x="${panelGap}" y="${70.0 + panelHeight + 92.0}" class="note">oblique cut volume: ${obliqueVolume.toFixed (4)}, cap area: ${obliqueCapArea.toFixed (4)}</text>
</svg>
`;

fs.writeFileSync (outputPath, svg);
console.log ('Solid section SVG written to ' + outputPath);
console.log ('Horizontal cut (z = 2): volume = ' + horizontalVolume.toFixed (4) + ', cap area = ' + horizontalCapArea.toFixed (4));
console.log ('Oblique cut: volume = ' + obliqueVolume.toFixed (4) + ', cap area = ' + obliqueCapArea.toFixed (4));
