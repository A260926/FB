"use strict"

//Generates a staircase voxel model, meshes it with the exact AO path and
//writes an ASCII PLY whose per-vertex colour is the ambient-occlusion byte
//(a greyscale value).  Run with:  node examples/staircase-ply.js [out.ply]

var fs = require("fs")
var path = require("path")
var ndarray = require("ndarray")
var mesh = require("../mesh.js")

var S = 12
var array = ndarray(new Uint16Array((S + 2) * (S + 2) * (S + 2)), [S + 2, S + 2, S + 2])

//Staircase: column i is i voxels tall, extruded along y.
for(var i = 1; i <= S; ++i) {
  for(var j = 1; j <= S; ++j) {
    for(var k = 1; k <= i; ++k) {
      array.set(i, j, k, mesh.OPAQUE_BIT | 1)
    }
  }
}

var bytes = mesh.exactAO(array)
if(!bytes) {
  throw new Error("staircase produced no faces")
}

var vertexCount = bytes.length / 8
var faceCount = vertexCount / 3
var lines = [
  "ply",
  "format ascii 1.0",
  "comment exact AO staircase, vertex colour = ambient occlusion",
  "element vertex " + vertexCount,
  "property float x",
  "property float y",
  "property float z",
  "property uchar red",
  "property uchar green",
  "property uchar blue",
  "element face " + faceCount,
  "property list uchar int vertex_indices",
  "end_header"
]

for(var p = 0; p < bytes.length; p += 8) {
  var x = bytes[p], y = bytes[p + 1], z = bytes[p + 2], ao = bytes[p + 3]
  lines.push(x + " " + y + " " + z + " " + ao + " " + ao + " " + ao)
}

//Six vertices per quad, emitted as two independent triangles.
for(var f = 0; f < faceCount; ++f) {
  var base = f * 3
  lines.push("3 " + base + " " + (base + 1) + " " + (base + 2))
}

var outPath = process.argv[2] || path.join(__dirname, "staircase.ply")
fs.writeFileSync(outPath, lines.join("\n") + "\n")
console.log("wrote " + outPath + " (" + vertexCount + " vertices, " + faceCount + " triangles)")
