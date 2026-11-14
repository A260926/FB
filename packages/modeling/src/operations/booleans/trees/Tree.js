const Node = require('./Node')
const PolygonTreeNode = require('./PolygonTreeNode')

// # class Tree
// This is the root of a BSP tree.
// This separate class for the root of the tree in order to hold the PolygonTreeNode root.
// The actual tree is kept in this.rootnode
class Tree {
  constructor (polygons, options) {
    this.polygonTree = new PolygonTreeNode()
    this.rootnode = new Node(null)
    this.options = options
    if (polygons) this.addPolygons(polygons)
  }

  invert () {
    this.polygonTree.invert()
    this.rootnode.invert()
  }

  // Remove all polygons in this BSP tree that are inside the other BSP tree
  // `tree`.
  clipTo (tree, alsoRemovecoplanarFront = false) {
    this.rootnode.clipTo(tree, alsoRemovecoplanarFront, this.options)
  }

  allPolygons () {
    const result = []
    this.polygonTree.getPolygons(result)
    return result
  }

  addPolygons (polygons) {
    const polygontreenodes = new Array(polygons.length)
    for (let i = 0; i < polygons.length; i++) {
      polygontreenodes[i] = this.polygonTree.addChild(polygons[i])
    }
    this.rootnode.addPolygonTreeNodes(polygontreenodes, this.options)
  }

  clear () {
    this.polygonTree.clear()
  }

  toString () {
    const result = 'Tree: ' + this.polygonTree.toString('')
    return result
  }
}

module.exports = Tree
