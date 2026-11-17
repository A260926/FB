// Copyright 2017 pdb Developers
//
// Licensed under the Apache License, Version 2.0, <LICENSE-APACHE or
// http://apache.org/licenses/LICENSE-2.0> or the MIT license <LICENSE-MIT or
// http://opensource.org/licenses/MIT>, at your option. This file may not be
// copied, modified, or distributed except according to those terms.

//! A read-only, self-contained structural graph of procedures and their types.
//!
//! The graph is built from the TPI and IPI streams of a PDB by copying the raw stream bytes. It
//! therefore does not borrow from a [`PDB`](crate::PDB), its module views, or its source, and
//! remains usable after all of them have been dropped. All caching is local to one graph, so
//! numerically equal indices in two different PDBs can never share results.
//!
//! Records are decoded with the crate's existing type and id parsers; the graph only adds the
//! structural linking (return types, argument lists, pointers, modifiers, and class members).

use std::collections::{HashMap, HashSet};
use std::fmt;
use std::rc::Rc;

use crate::common::*;
use crate::tpi::constants::*;
use crate::tpi::{parse_padding, parse_type_data, type_data_for_primitive, Header};
use crate::tpi::{
    ArgumentList, ClassKind, ClassType, FieldAttributes, FunctionAttributes, IdData, MemberType,
    ModifierType, PointerAttributes, PointerType, ProcedureType, TypeData,
};

/// Opaque identifier of a node inside one [`TypeGraph`].
///
/// The identifier is meaningful only within the graph that produced it. Numeric equality across
/// two graphs is not part of the API; identical numbers in a different graph refer to unrelated
/// nodes. Once a graph returns an identifier, that node keeps the same identifier for the lifetime
/// of the graph.
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct TypeNodeId(u32);

impl fmt::Debug for TypeNodeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TypeNodeId({})", self.0)
    }
}

/// A node in a [`TypeGraph`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TypeNode {
    /// The TPI index this node was built from.
    ///
    /// For a forward declaration that aliases a complete definition, this is the index of the
    /// complete definition (the canonical index). For an unbound forward declaration it is the
    /// forward record's own index.
    pub index: TypeIndex,
    /// The structural contents of this node.
    pub kind: TypeNodeKind,
}

/// The structural kind of a [`TypeNode`].
///
/// This is a data model, not a C declarator pretty-printer: list records such as `LF_ARGLIST` and
/// `LF_FIELDLIST` are internal and never become separate public nodes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TypeNodeKind {
    /// A primitive type, identified by its TPI index (which is below `0x1000`).
    Primitive {
        /// Index of the primitive type.
        index: TypeIndex,
    },
    /// An ordinary pointer or an lvalue/rvalue reference.
    Pointer {
        /// The pointed-to node.
        to: TypeNodeId,
        /// Raw pointer attributes, preserving const/volatile/unaligned/size/mode information.
        attributes: PointerAttributes,
    },
    /// A `const`, `volatile`, and/or `unaligned` modifier.
    Modifier {
        /// The modified node.
        to: TypeNodeId,
        /// Whether the type is `const`.
        constant: bool,
        /// Whether the type is `volatile`.
        volatile: bool,
        /// Whether the type is unaligned.
        unaligned: bool,
    },
    /// An `LF_PROCEDURE` signature.
    Procedure {
        /// The return type, or `None` for an untyped/void return encoding.
        return_type: Option<TypeNodeId>,
        /// The argument types in source order.
        arguments: Vec<TypeNodeId>,
        /// Calling convention and function flags.
        attributes: FunctionAttributes,
    },
    /// An `LF_MFUNCTION` signature.
    MemberFunction {
        /// The return type.
        return_type: TypeNodeId,
        /// The class this member function belongs to.
        class: TypeNodeId,
        /// The `this` pointer type, if one is encoded.
        this_pointer: Option<TypeNodeId>,
        /// The argument types in source order (excluding `this`).
        arguments: Vec<TypeNodeId>,
        /// Calling convention and function flags.
        attributes: FunctionAttributes,
        /// The `this` adjustment.
        this_adjustment: u32,
    },
    /// An `LF_CLASS` or `LF_STRUCTURE`.
    Class {
        /// Whether this is a class or a struct.
        kind: ClassKind,
        /// The raw display-name bytes.
        name: Vec<u8>,
        /// The raw unique-name bytes, if the record carries one.
        unique_name: Option<Vec<u8>>,
        /// The size of the type in bytes.
        size: u64,
        /// Whether a complete definition was found (or this is one).
        complete: bool,
        /// Data members in source order, with continuation members appended after the head list.
        members: Vec<TypeMember>,
    },
}

/// A data member of a class or struct.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TypeMember {
    /// The raw name bytes.
    pub name: Vec<u8>,
    /// Byte offset of the member within the class.
    pub offset: u64,
    /// The member's type.
    pub ty: TypeNodeId,
    /// Access and other field attributes.
    pub attributes: FieldAttributes,
}

#[derive(Debug)]
struct OwnedStream {
    bytes: Rc<Vec<u8>>,
    /// Sorted `(index, data_offset, data_len)`; `data_offset` skips the 16-bit length prefix.
    ranges: Vec<(u32, usize, usize)>,
}

impl OwnedStream {
    fn empty() -> Self {
        Self {
            bytes: Rc::new(Vec::new()),
            ranges: Vec::new(),
        }
    }

    fn parse(bytes: Vec<u8>) -> Result<Self> {
        if bytes.is_empty() {
            return Ok(Self::empty());
        }

        let header = {
            let mut buf = ParseBuffer::from(bytes.as_slice());
            Header::parse(&mut buf)?
        };

        let mut ranges = Vec::new();
        let mut pos = header.header_size as usize;
        let mut index = header.minimum_index;

        while index < header.maximum_index && pos + 2 <= bytes.len() {
            let length = {
                let mut buf = ParseBuffer::from(&bytes[pos..]);
                buf.parse_u16()? as usize
            };
            let data_offset = pos + 2;
            let data_end = data_offset
                .checked_add(length)
                .ok_or(Error::InvalidStreamLength("type information"))?;
            if data_end > bytes.len() {
                return Err(Error::InvalidStreamLength("type information"));
            }
            ranges.push((index, data_offset, length));
            pos = data_end;
            index += 1;
        }

        Ok(Self {
            bytes: Rc::new(bytes),
            ranges,
        })
    }

    fn record(&self, index: u32) -> Option<(Rc<Vec<u8>>, usize, usize)> {
        self.ranges
            .binary_search_by_key(&index, |entry| entry.0)
            .ok()
            .map(|position| {
                let (_, offset, length) = self.ranges[position];
                (Rc::clone(&self.bytes), offset, length)
            })
    }
}

/// A self-contained, read-only structural graph of types and procedure signatures.
///
/// Obtain one with [`PDB::type_graph`](crate::PDB::type_graph). Resolve roots with
/// [`resolve_type`](Self::resolve_type) or [`resolve_procedure`](Self::resolve_procedure), then
/// inspect them with [`node`](Self::node).
#[derive(Debug)]
pub struct TypeGraph {
    types: OwnedStream,
    ids: OwnedStream,
    nodes: Vec<TypeNode>,
    /// TPI index to canonical node. Never shared between graphs.
    type_nodes: HashMap<TypeIndex, TypeNodeId>,
    /// IPI index to procedure node. Never shared between graphs.
    id_nodes: HashMap<IdIndex, TypeNodeId>,
    /// Nodes currently under construction, used to give recursive references a stable identity.
    resolving: HashMap<TypeIndex, TypeNodeId>,
    /// References followed from the current public query root, outermost first.
    path: Vec<TypeReference>,
}

impl TypeGraph {
    pub(crate) fn from_streams(tpi: Option<&[u8]>, ipi: Option<&[u8]>) -> Result<Self> {
        Ok(Self {
            types: OwnedStream::parse(tpi.unwrap_or(&[]).to_vec())?,
            ids: OwnedStream::parse(ipi.unwrap_or(&[]).to_vec())?,
            nodes: Vec::new(),
            type_nodes: HashMap::new(),
            id_nodes: HashMap::new(),
            resolving: HashMap::new(),
            path: Vec::new(),
        })
    }

    /// Resolve a TPI root index into a graph node.
    ///
    /// Resolving the same index again returns the same node. Forward declarations that match
    /// exactly one complete definition alias that definition's node.
    pub fn resolve_type(&mut self, index: TypeIndex) -> Result<TypeNodeId> {
        self.path.clear();
        self.path.push(TypeReference::Type(index));
        let snapshot = self.nodes.len();
        let result = self.resolve_index(index);
        self.path.clear();
        match result {
            Ok(id) => Ok(id),
            Err(error) => {
                self.rollback(snapshot);
                Err(error)
            }
        }
    }

    /// Resolve the signature of a procedure symbol into a graph node.
    ///
    /// Symbols with a [`TypeReference::Type`] reference go straight to the TPI stream. Symbols with
    /// a [`TypeReference::Id`] reference first resolve `LF_FUNC_ID` or `LF_MFUNC_ID` from the IPI
    /// stream, then follow the referenced TPI procedure type. A failed IPI lookup is never retried
    /// against a same-numbered TPI record.
    pub fn resolve_procedure(&mut self, procedure: &crate::ProcedureSymbol<'_>) -> Result<TypeNodeId> {
        self.path.clear();
        self.path.push(procedure.type_reference);
        let snapshot = self.nodes.len();
        let result = self.resolve_procedure_inner(procedure);
        self.path.clear();
        match result {
            Ok(id) => Ok(id),
            Err(error) => {
                self.rollback(snapshot);
                Err(error)
            }
        }
    }

    /// Return the node with the given identifier, or `None` if the identifier belongs to another
    /// graph or was never returned by this graph.
    pub fn node(&self, id: TypeNodeId) -> Option<&TypeNode> {
        self.nodes.get(id.0 as usize)
    }

    fn rollback(&mut self, snapshot: usize) {
        self.nodes.truncate(snapshot);
        let snapshot = snapshot as u32;
        self.type_nodes.retain(|_, id| id.0 < snapshot);
        self.id_nodes.retain(|_, id| id.0 < snapshot);
        self.resolving.clear();
    }

    fn push_node(&mut self, node: TypeNode) -> TypeNodeId {
        let id = TypeNodeId(self.nodes.len() as u32);
        self.nodes.push(node);
        id
    }

    fn reserve(&mut self, index: TypeIndex) -> TypeNodeId {
        let id = self.push_node(TypeNode {
            index,
            kind: TypeNodeKind::Primitive { index },
        });
        self.resolving.insert(index, id);
        id
    }

    fn finish(&mut self, index: TypeIndex, id: TypeNodeId, kind: TypeNodeKind) {
        self.resolving.remove(&index);
        self.nodes[id.0 as usize] = TypeNode { index, kind };
        self.type_nodes.insert(index, id);
    }

    fn type_record(&self, index: TypeIndex) -> Result<(Rc<Vec<u8>>, usize, usize)> {
        self.types
            .record(index.0)
            .ok_or(Error::TypeNotFound(index.0))
    }

    fn id_record(&self, index: IdIndex) -> Result<(Rc<Vec<u8>>, usize, usize)> {
        self.ids
            .record(index.0)
            .ok_or(Error::TypeGraphMissingId(index))
    }

    fn resolve_index(&mut self, index: TypeIndex) -> Result<TypeNodeId> {
        if let Some(id) = self.type_nodes.get(&index) {
            return Ok(*id);
        }
        if let Some(id) = self.resolving.get(&index) {
            return Ok(*id);
        }

        if index.0 < 0x1000 {
            let kind = match type_data_for_primitive(index)? {
                TypeData::Primitive(_) => TypeNodeKind::Primitive { index },
                _ => unreachable!("primitive decoding must return a primitive"),
            };
            let id = self.push_node(TypeNode { index, kind });
            self.type_nodes.insert(index, id);
            return Ok(id);
        }

        let (bytes, offset, length) = self.type_record(index)?;
        let raw_kind = Self::raw_kind(&bytes, offset);
        let data = {
            let mut buf = ParseBuffer::from(&bytes[offset..offset + length]);
            parse_type_data(&mut buf).map_err(|error| {
                self.contextual(TypeReference::Type(index), error)
            })?
        };

        // Forward declarations are aliases and never get their own node when a definition exists.
        if let TypeData::Class(ref class) = data {
            if Self::is_forward(class) {
                return self.resolve_forward(index, class);
            }
        }

        let id = self.reserve(index);
        let kind = self.build_kind(index, raw_kind, data)?;
        self.finish(index, id, kind);
        Ok(id)
    }

    /// Resolve `index` as a child of the record currently being built, recording it in the error
    /// path. Cache hits and nodes under construction do not extend the path, and a failed child
    /// leaves the path unchanged so outer frames do not accumulate stale entries.
    fn resolve_child(&mut self, index: TypeIndex) -> Result<TypeNodeId> {
        if self.type_nodes.contains_key(&index) || self.resolving.contains_key(&index) {
            return self.resolve_index(index);
        }
        self.path.push(TypeReference::Type(index));
        let result = self.resolve_index(index);
        self.path.pop();
        result
    }

    fn raw_kind(bytes: &[u8], offset: usize) -> u16 {
        u16::from(bytes[offset]) | (u16::from(bytes[offset + 1]) << 8)
    }

    fn contextual(&self, source: TypeReference, error: Error) -> Error {
        match error {
            Error::UnimplementedTypeKind(kind) => Error::TypeGraphUnsupported {
                source,
                kind,
                path: self.path.clone(),
            },
            other => other,
        }
    }

    /// Report a valid-but-unsupported record reached from the current query.
    fn unsupported(&self, source: TypeReference, kind: u16) -> Error {
        Error::TypeGraphUnsupported {
            source,
            kind,
            path: self.path.clone(),
        }
    }

    fn is_forward(class: &ClassType<'_>) -> bool {
        matches!(class.kind, ClassKind::Class | ClassKind::Struct)
            && class.properties.forward_reference()
    }

    fn resolve_forward(
        &mut self,
        index: TypeIndex,
        class: &ClassType<'_>,
    ) -> Result<TypeNodeId> {
        let candidates = self.forward_candidates(class);

        if candidates.len() == 1 {
            let id = self.resolve_child(candidates[0])?;
            self.type_nodes.insert(index, id);
            return Ok(id);
        }

        if candidates.len() > 1 {
            return Err(Error::TypeGraphAmbiguousForward {
                forward: index,
                candidates,
            });
        }

        let id = self.push_node(TypeNode {
            index,
            kind: TypeNodeKind::Class {
                kind: class.kind,
                name: class.name.as_bytes().to_vec(),
                unique_name: class.unique_name.map(|name| name.as_bytes().to_vec()),
                size: class.size,
                complete: false,
                members: Vec::new(),
            },
        });
        self.type_nodes.insert(index, id);
        Ok(id)
    }

    fn forward_candidates(&self, forward: &ClassType<'_>) -> Vec<TypeIndex> {
        let unique_name = forward.unique_name.map(|name| name.as_bytes());
        let display_name = forward.name.as_bytes();
        let mut candidates = Vec::new();

        for &(raw_index, offset, length) in &self.types.ranges {
            let index = TypeIndex(raw_index);
            let bytes = match self.types.record(raw_index) {
                Some((bytes, _, _)) => bytes,
                None => continue,
            };
            let mut buf = ParseBuffer::from(&bytes[offset..offset + length]);
            let class = match parse_type_data(&mut buf) {
                Ok(TypeData::Class(class)) => class,
                _ => continue,
            };
            if class.kind != forward.kind || Self::is_forward(&class) {
                continue;
            }
            if class.properties.forward_reference() {
                continue;
            }

            let matches = match unique_name {
                Some(unique) => {
                    class.unique_name.map(|name| name.as_bytes()) == Some(unique)
                }
                None => class.name.as_bytes() == display_name,
            };
            if matches {
                candidates.push(index);
            }
        }

        candidates
    }

    fn build_kind(
        &mut self,
        index: TypeIndex,
        raw_kind: u16,
        data: TypeData<'_>,
    ) -> Result<TypeNodeKind> {
        match data {
            TypeData::Class(class) => self.build_class(index, class),
            TypeData::Procedure(procedure) => self.build_procedure(procedure),
            TypeData::MemberFunction(member) => self.build_member_function(member),
            TypeData::Pointer(pointer) => self.build_pointer(index, pointer),
            TypeData::Modifier(modifier) => self.build_modifier(modifier),
            _ => Err(self.unsupported(TypeReference::Type(index), raw_kind)),
        }
    }

    fn build_class(
        &mut self,
        index: TypeIndex,
        class: ClassType<'_>,
    ) -> Result<TypeNodeKind> {
        if class.kind == ClassKind::Interface {
            return Err(self.unsupported(TypeReference::Type(index), LF_INTERFACE));
        }

        let mut members = Vec::new();
        if let Some(fields) = class.fields {
            self.read_field_list(fields, &mut members)?;
        }

        Ok(TypeNodeKind::Class {
            kind: class.kind,
            name: class.name.as_bytes().to_vec(),
            unique_name: class.unique_name.map(|name| name.as_bytes().to_vec()),
            size: class.size,
            complete: true,
            members,
        })
    }

    fn build_procedure(&mut self, procedure: ProcedureType) -> Result<TypeNodeKind> {
        let return_type = match procedure.return_type {
            Some(index) => Some(self.resolve_child(index)?),
            None => None,
        };
        let arguments = self.resolve_arguments(procedure.argument_list)?;
        Ok(TypeNodeKind::Procedure {
            return_type,
            arguments,
            attributes: procedure.attributes,
        })
    }

    fn build_member_function(
        &mut self,
        member: crate::tpi::MemberFunctionType,
    ) -> Result<TypeNodeKind> {
        let return_type = self.resolve_child(member.return_type)?;
        let class = self.resolve_child(member.class_type)?;
        let this_pointer = match member.this_pointer_type {
            Some(index) => Some(self.resolve_child(index)?),
            None => None,
        };
        let arguments = self.resolve_arguments(member.argument_list)?;
        Ok(TypeNodeKind::MemberFunction {
            return_type,
            class,
            this_pointer,
            arguments,
            attributes: member.attributes,
            this_adjustment: member.this_adjustment,
        })
    }

    fn build_pointer(
        &mut self,
        index: TypeIndex,
        pointer: PointerType,
    ) -> Result<TypeNodeKind> {
        if pointer.attributes.pointer_to_member() {
            return Err(self.unsupported(TypeReference::Type(index), LF_POINTER));
        }
        let to = self.resolve_child(pointer.underlying_type)?;
        Ok(TypeNodeKind::Pointer {
            to,
            attributes: pointer.attributes,
        })
    }

    fn build_modifier(&mut self, modifier: ModifierType) -> Result<TypeNodeKind> {
        let to = self.resolve_child(modifier.underlying_type)?;
        Ok(TypeNodeKind::Modifier {
            to,
            constant: modifier.constant,
            volatile: modifier.volatile,
            unaligned: modifier.unaligned,
        })
    }

    fn resolve_arguments(&mut self, list: TypeIndex) -> Result<Vec<TypeNodeId>> {
        if list == TypeIndex(0) || list == TypeIndex(0xffff) {
            return Ok(Vec::new());
        }

        let (bytes, offset, length) = self.type_record(list)?;
        let data = {
            let mut buf = ParseBuffer::from(&bytes[offset..offset + length]);
            parse_type_data(&mut buf)
                .map_err(|error| self.contextual(TypeReference::Type(list), error))?
        };

        let arguments = match data {
            TypeData::ArgumentList(ArgumentList { arguments }) => arguments,
            _ => {
                return Err(Error::TypeGraphProcedureMismatch {
                    source: TypeReference::Type(list),
                    detail: "expected an LF_ARGLIST record",
                })
            }
        };

        let mut resolved = Vec::with_capacity(arguments.len());
        for argument in arguments {
            resolved.push(self.resolve_child(argument)?);
        }
        Ok(resolved)
    }

    fn read_field_list(
        &mut self,
        index: TypeIndex,
        members: &mut Vec<TypeMember>,
    ) -> Result<()> {
        let mut next = Some(index);
        let mut seen = HashSet::new();

        while let Some(list_index) = next.take() {
            if !seen.insert(list_index) {
                return Err(Error::UnimplementedFeature("cyclic LF_INDEX chain"));
            }

            let (bytes, offset, length) = self.type_record(list_index)?;
            let mut buf = ParseBuffer::from(&bytes[offset..offset + length]);
            let leaf = buf.parse_u16()?;
            if leaf != LF_FIELDLIST {
                return Err(Error::TypeGraphProcedureMismatch {
                    source: TypeReference::Type(list_index),
                    detail: "expected an LF_FIELDLIST record",
                });
            }

            loop {
                if buf.is_empty() {
                    break;
                }

                let field_kind = buf.peek_u16()?;
                if field_kind == LF_INDEX {
                    // Microsoft's lfIndex has a required zero u16 before the u32 type index,
                    // unlike the legacy 16t LF_INDEX variant.
                    buf.parse_u16()?;
                    let _padding: u16 = buf.parse_u16()?;
                    next = Some(buf.parse()?);
                    break;
                }

                let field = parse_type_data(&mut buf)
                    .map_err(|error| self.contextual(TypeReference::Type(list_index), error))?;

                let member: MemberType<'_> = match field {
                    TypeData::Member(member) => member,
                    _ => {
                        return Err(
                            self.unsupported(TypeReference::Type(list_index), field_kind)
                        )
                    }
                };

                let ty = self.resolve_child(member.field_type)?;
                members.push(TypeMember {
                    name: member.name.as_bytes().to_vec(),
                    offset: member.offset,
                    ty,
                    attributes: member.attributes,
                });

                parse_padding(&mut buf)?;
            }
        }

        Ok(())
    }

    fn resolve_procedure_inner(
        &mut self,
        procedure: &crate::ProcedureSymbol<'_>,
    ) -> Result<TypeNodeId> {
        match procedure.type_reference {
            TypeReference::Type(index) => {
                let id = self.resolve_index(index)?;
                self.require_procedure(id, TypeReference::Type(index))?;
                Ok(id)
            }
            TypeReference::Id(index) => self.resolve_id(index),
        }
    }

    fn resolve_id(&mut self, index: IdIndex) -> Result<TypeNodeId> {
        if let Some(id) = self.id_nodes.get(&index) {
            return Ok(*id);
        }

        let (bytes, offset, length) = self.id_record(index)?;
        let raw_kind = Self::raw_kind(&bytes, offset);
        let data = {
            let mut buf = ParseBuffer::from(&bytes[offset..offset + length]);
            let data: IdData<'_> = buf
                .parse()
                .map_err(|error| self.contextual(TypeReference::Id(index), error))?;
            data
        };

        match data {
            IdData::Function(function) => {
                let id = self.resolve_child(function.function_type)?;
                self.require_kind(id, TypeReference::Id(index), false)?;
                self.id_nodes.insert(index, id);
                Ok(id)
            }
            IdData::MemberFunction(member) => {
                let id = self.resolve_child(member.function_type)?;
                self.require_kind(id, TypeReference::Id(index), true)?;
                let class = match self.nodes.get(id.0 as usize) {
                    Some(TypeNode {
                        kind: TypeNodeKind::MemberFunction { class, .. },
                        ..
                    }) => *class,
                    // `require_kind` above guarantees this, but stay defensive.
                    _ => {
                        return Err(Error::TypeGraphProcedureMismatch {
                            source: TypeReference::Id(index),
                            detail: "LF_MFUNC_ID does not reference an LF_MFUNCTION",
                        })
                    }
                };

                // The parent class is an additional consistency check, not a type edge: its node
                // is resolved (canonicalizing a forward declaration) but is not stored.
                let parent = self.resolve_child(member.parent)?;
                let classes_match = match (
                    self.nodes.get(class.0 as usize),
                    self.nodes.get(parent.0 as usize),
                ) {
                    (
                        Some(TypeNode {
                            kind: TypeNodeKind::Class { .. },
                            ..
                        }),
                        Some(TypeNode {
                            kind: TypeNodeKind::Class { .. },
                            ..
                        }),
                    ) => class == parent,
                    _ => false,
                };
                if !classes_match {
                    return Err(Error::TypeGraphProcedureMismatch {
                        source: TypeReference::Id(index),
                        detail: "LF_MFUNC_ID parent does not match the member function's class",
                    });
                }

                self.id_nodes.insert(index, id);
                Ok(id)
            }
            _ => Err(self.unsupported(TypeReference::Id(index), raw_kind)),
        }
    }

    fn require_procedure(&self, id: TypeNodeId, source: TypeReference) -> Result<()> {
        match self.nodes.get(id.0 as usize) {
            Some(TypeNode {
                kind: TypeNodeKind::Procedure { .. },
                ..
            })
            | Some(TypeNode {
                kind: TypeNodeKind::MemberFunction { .. },
                ..
            }) => Ok(()),
            _ => Err(Error::TypeGraphProcedureMismatch {
                source,
                detail: "reference does not resolve to LF_PROCEDURE or LF_MFUNCTION",
            }),
        }
    }

    /// Require the node to be the kind mandated by a `LF_FUNC_ID`/`LF_MFUNC_ID` record.
    fn require_kind(
        &self,
        id: TypeNodeId,
        source: TypeReference,
        member: bool,
    ) -> Result<()> {
        let matches = match self.nodes.get(id.0 as usize) {
            Some(TypeNode {
                kind: TypeNodeKind::MemberFunction { .. },
                ..
            }) => member,
            Some(TypeNode {
                kind: TypeNodeKind::Procedure { .. },
                ..
            }) => !member,
            _ => false,
        };
        if matches {
            Ok(())
        } else if member {
            Err(Error::TypeGraphProcedureMismatch {
                source,
                detail: "LF_MFUNC_ID does not reference an LF_MFUNCTION",
            })
        } else {
            Err(Error::TypeGraphProcedureMismatch {
                source,
                detail: "LF_FUNC_ID does not reference an LF_PROCEDURE",
            })
        }
    }
}
