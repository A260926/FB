//! Regression tests for the read-only procedure type graph.
//!
//! Each test builds metadata-only synthetic PDBs from `task_support/fixture.rs`, opens them through
//! the normal library API, and checks the structural graph with `Check::check`.

#[path = "../task_support/fixture.rs"]
mod fixture;
#[path = "../task_support/check.rs"]
#[allow(dead_code)]
mod check;

// `Check::check` is called through the `&mut Check` argument that `run` provides.
#[allow(unused_imports)]
use check::{run, Check};
use pdb::{
    ClassKind, FallibleIterator, IdIndex, PDB, SymbolData, TypeIndex, TypeNode, TypeNodeId,
    TypeNodeKind, TypeReference,
};
use std::io::Cursor;

/// Parse all procedure symbols, copying the fields the tests need out of the borrowed stream.
#[derive(Clone, Debug)]
struct Proc {
    name: String,
    kind: u16,
    type_index: TypeIndex,
    type_reference: TypeReference,
}

fn collect_procedures<'s>(pdb: &mut PDB<'s, Cursor<&'s [u8]>>) -> Vec<Proc> {
    let dbi = pdb.debug_information().expect("debug information");
    let module = dbi
        .modules()
        .expect("modules")
        .next()
        .expect("next module")
        .expect("module");
    let info = pdb.module_info(&module).expect("module info").expect("module stream");
    let mut symbols = info.symbols().expect("symbols");
    let mut out = Vec::new();
    while let Some(symbol) = symbols.next().expect("next symbol") {
        if let SymbolData::Procedure(procedure) = symbol.parse().expect("parse symbol") {
            out.push(Proc {
                name: procedure.name.to_string().into_owned(),
                kind: symbol.raw_kind(),
                type_index: procedure.type_index,
                type_reference: procedure.type_reference,
            });
        }
    }
    out
}

/// Resolve a procedure by name through a graph. The symbol is re-parsed from a fresh PDB because
/// the graph does not borrow the module view.
fn resolve_named<'s>(
    pdb: &mut PDB<'s, Cursor<&'s [u8]>>,
    graph: &mut pdb::TypeGraph,
    name: &str,
) -> pdb::Result<TypeNodeId> {
    let dbi = pdb.debug_information()?;
    let module = dbi.modules()?.next()?.expect("module");
    let info = pdb.module_info(&module)?.expect("module stream");
    let mut symbols = info.symbols()?;
    while let Some(symbol) = symbols.next()? {
        if let SymbolData::Procedure(procedure) = symbol.parse()? {
            if procedure.name.to_string() == name {
                return graph.resolve_procedure(&procedure);
            }
        }
    }
    panic!("procedure {} not found", name);
}

fn node<'g>(graph: &'g pdb::TypeGraph, id: TypeNodeId) -> &'g TypeNode {
    graph.node(id).expect("node is present")
}

fn class_kind(node: &TypeNode) -> Option<ClassKind> {
    match node.kind {
        TypeNodeKind::Class { kind, .. } => Some(kind),
        _ => None,
    }
}

fn class_members(node: &TypeNode) -> Option<&[pdb::TypeMember]> {
    match &node.kind {
        TypeNodeKind::Class { members, .. } => Some(members),
        _ => None,
    }
}

fn class_name(node: &TypeNode) -> Option<&[u8]> {
    match &node.kind {
        TypeNodeKind::Class { name, .. } => Some(name),
        _ => None,
    }
}

/// Build an image for the `origin` scenario and return the collected procedures.
fn origin_image() -> (Vec<u8>, fixture::OriginIndices) {
    let (types, ids, idx) = fixture::origin();
    let image = fixture::image_scenario(
        &types,
        &ids,
        &[
            (0x1110, idx.proc, "ordinary"),
            (0x1147, idx.func_id, "through_id"),
            (0x1146, idx.method_id, "member_id"),
        ],
    );
    (image, idx)
}

#[test]
fn origin() {
    run("origin", |check| {
        let (bytes, idx) = origin_image();
        let mut pdb = PDB::open(Cursor::new(bytes.as_slice())).expect("open pdb");

        // The parse API classifies each procedure's reference by symbol kind, while preserving the
        // raw index bits.
        let procs = collect_procedures(&mut pdb);
        check.check(procs.len() == 3, "three procedures parsed");
        let ordinary = procs.iter().find(|p| p.name == "ordinary").unwrap();
        let through_id = procs.iter().find(|p| p.name == "through_id").unwrap();
        let member_id = procs.iter().find(|p| p.name == "member_id").unwrap();

        check.check(
            ordinary.type_reference == TypeReference::Type(ordinary.type_index),
            "ordinary GPROC32 is a TPI reference",
        );
        check.check(
            through_id.type_reference == TypeReference::Id(IdIndex(through_id.type_index.0)),
            "GPROC32_ID is an IPI reference",
        );
        check.check(
            through_id.type_index == TypeIndex(idx.func_id),
            "raw bits are preserved for _ID procedures",
        );
        check.check(
            member_id.type_reference == TypeReference::Id(IdIndex(member_id.type_index.0)),
            "LPROC32_ID is an IPI reference",
        );
        check.check(
            ordinary.kind == 0x1110 && through_id.kind == 0x1147 && member_id.kind == 0x1146,
            "original symbol kinds survive",
        );

        let mut graph = pdb.type_graph().expect("type graph");

        // Ordinary procedure: TPI LF_PROCEDURE directly.
        let ordinary_node = resolve_named(&mut pdb, &mut graph, "ordinary").expect("resolve");
        let ordinary_index = node(&graph, ordinary_node).index;
        check.check(
            ordinary_index == TypeIndex(idx.proc),
            "ordinary procedure resolves the TPI procedure record",
        );
        let (ret, args, attrs) = match &node(&graph, ordinary_node).kind {
            TypeNodeKind::Procedure {
                return_type,
                arguments,
                attributes,
            } => (*return_type, arguments.clone(), *attributes),
            other => panic!("expected Procedure, got {:?}", other),
        };
        check.check(args.len() == 2, "procedure has two arguments");
        check.check(attrs.calling_convention() == 0x40, "calling convention preserved");

        // Return type is const int32 through LF_MODIFIER.
        let ret = ret.expect("return type");
        match &node(&graph, ret).kind {
            TypeNodeKind::Modifier {
                to,
                constant,
                volatile,
                unaligned,
            } => {
                check.check(*constant && !*volatile && !*unaligned, "return is only const");
                check.check(
                    node(&graph, *to).index == TypeIndex(0x74),
                    "modifier points at the int32 primitive",
                );
                check.check(
                    matches!(node(&graph, *to).kind, TypeNodeKind::Primitive { .. }),
                    "int32 is a primitive node",
                );
            }
            other => panic!("expected Modifier, got {:?}", other),
        }

        // First argument is const volatile int* with pointer attributes intact.
        let arg0 = args[0];
        match &node(&graph, arg0).kind {
            TypeNodeKind::Pointer { to, attributes } => {
                check.check(attributes.is_const(), "pointer const attribute kept");
                check.check(attributes.is_volatile(), "pointer volatile attribute kept");
                check.check(
                    node(&graph, *to).index == TypeIndex(0x74),
                    "pointer target is int32",
                );
            }
            other => panic!("expected Pointer, got {:?}", other),
        }

        // _ID procedure: IPI LF_FUNC_ID -> TPI LF_PROCEDURE.
        let id_node = resolve_named(&mut pdb, &mut graph, "through_id").expect("resolve id");
        check.check(
            node(&graph, id_node).index == TypeIndex(idx.proc),
            "LF_FUNC_ID follows its function_type, not the same-numbered TPI record",
        );
        // The same-numbered TPI record is a different procedure.
        let same_number = graph.resolve_type(TypeIndex(idx.tpi_same)).expect("resolve same number");
        check.check(
            same_number != id_node,
            "same numeric index in TPI and IPI resolve to different nodes",
        );
        check.check(
            node(&graph, same_number).index == TypeIndex(idx.tpi_same),
            "the same-numbered TPI record resolves to itself",
        );

        // Member function through LF_MFUNC_ID, including class and this pointer.
        let member_node = resolve_named(&mut pdb, &mut graph, "member_id").expect("resolve member");
        let int32 = graph.resolve_type(TypeIndex(0x74)).expect("int32");
        let member = match &node(&graph, member_node).kind {
            TypeNodeKind::MemberFunction {
                return_type,
                class,
                this_pointer,
                arguments,
                attributes,
                this_adjustment,
            } => (
                *return_type,
                *class,
                *this_pointer,
                arguments.clone(),
                *attributes,
                *this_adjustment,
            ),
            other => panic!("expected MemberFunction, got {:?}", other),
        };
        let (return_type, class, this_pointer, arguments, attributes, this_adjustment) = member;
        check.check(this_adjustment == 4, "this adjustment preserved");
        check.check(attributes.calling_convention() == 0x40, "member call convention");
        check.check(arguments.len() == 2, "member has two arguments");
        check.check(
            return_type == int32,
            "member return type shared with the int32 node",
        );
        let class_node = node(&graph, class);
        check.check(
            class_node.index == TypeIndex(idx.class_c),
            "member class is the complete class C",
        );
        check.check(
            class_name(class_node) == Some(b"C".as_ref()),
            "class name preserved as raw bytes",
        );
        let this_node = this_pointer.expect("this pointer");
        check.check(
            matches!(node(&graph, this_node).kind, TypeNodeKind::Pointer { .. }),
            "this type is a pointer node",
        );
        // The member Id's parent was a forward declaration; canonicalizing it must reach the same
        // class node as the member function's class type.
        let class_forward = graph
            .resolve_type(TypeIndex(idx.class_c_forward))
            .expect("forward parent");
        check.check(
            class_forward == class,
            "LF_MFUNC_ID parent canonicalizes to the member function's class node",
        );

        // The class reached directly resolves to the same node as through the member function.
        let class_direct = graph.resolve_type(TypeIndex(idx.class_c)).expect("class");
        check.check(
            class_direct == class,
            "shared class node identity",
        );
        let members = class_members(node(&graph, class_direct)).expect("class members");
        check.check(members.len() == 1, "class C has one member");
        check.check(members[0].name == b"a", "member name raw bytes");
        check.check(members[0].offset == 0, "member offset preserved");
    });
}

#[test]
fn forward() {
    run("forward", |check| {
        let (types, idx) = fixture::forward();
        let bytes = fixture::image_scenario(
            &types,
            &[],
            &[(0x1110, idx.proc, "returns_unbound"), (0x1110, idx.twin_proc, "returns_twin")],
        );
        let mut pdb = PDB::open(Cursor::new(bytes.as_slice())).expect("open pdb");
        let mut graph = pdb.type_graph().expect("type graph");

        // Unique name wins over display name.
        let unique = graph.resolve_type(TypeIndex(idx.unique_forward)).expect("unique forward");
        check.check(
            node(&graph, unique).index == TypeIndex(idx.unique_real),
            "forward with unique name binds to the definition with matching unique bytes",
        );
        check.check(
            class_name(node(&graph, unique)) == Some(b"DifferentName".as_ref()),
            "display-name decoy is not chosen",
        );
        check.check(
            graph
                .resolve_type(TypeIndex(idx.unique_real))
                .expect("real")
                == unique,
            "forward and direct references to the complete definition share one node",
        );

        // Display name only, one candidate.
        let display = graph.resolve_type(TypeIndex(idx.display_forward));
        match display {
            Err(pdb::Error::TypeGraphAmbiguousForward { forward, candidates }) => {
                check.check(
                    forward == TypeIndex(idx.display_forward),
                    "ambiguity names the forward index",
                );
                let mut expected = vec![
                    TypeIndex(idx.display_def_a),
                    TypeIndex(idx.display_def_b),
                ];
                expected.sort();
                let mut got = candidates.clone();
                got.sort();
                check.check(got == expected, "ambiguity lists both candidate indices");
            }
            other => panic!("expected ambiguity, got {:?}", other),
        }

        // Zero candidates: a stable incomplete node.
        let unbound = graph.resolve_type(TypeIndex(idx.unbound)).expect("unbound forward");
        match &node(&graph, unbound).kind {
            TypeNodeKind::Class {
                name,
                complete,
                members,
                ..
            } => {
                check.check(!*complete, "unbound forward is incomplete");
                check.check(members.is_empty(), "unbound forward has no members");
                check.check(name == b"Missing", "unbound forward keeps its display name");
            }
            other => panic!("expected Class, got {:?}", other),
        }
        check.check(
            node(&graph, unbound).index == TypeIndex(idx.unbound),
            "incomplete node keeps the forward index",
        );
        check.check(
            graph.resolve_type(TypeIndex(idx.unbound)).expect("again") == unbound,
            "unbound forward is stable",
        );

        // Explicitly referenced complete definitions do not merge despite identical display names.
        let twin_a = graph.resolve_type(TypeIndex(idx.explicit_a)).expect("twin a");
        let twin_b = graph.resolve_type(TypeIndex(idx.explicit_b)).expect("twin b");
        check.check(
            twin_a != twin_b,
            "explicit complete definitions with equal display names stay separate",
        );
        check.check(
            class_name(node(&graph, twin_a)) == Some(b"Twin".as_ref()),
            "first twin name",
        );
        check.check(
            matches!(
                &node(&graph, twin_a).kind,
                TypeNodeKind::Class { unique_name: Some(unique), .. } if unique == b".?AUTwin1@@"
            ),
            "first twin unique name preserved",
        );

        // Display-name matching also works when the complete definition is recorded first, and the
        // result is identical whichever entry point resolves first.
        let sole_forward = graph.resolve_type(TypeIndex(idx.sole_forward)).expect("sole forward");
        check.check(
            node(&graph, sole_forward).index == TypeIndex(idx.sole_def),
            "forward binds to the sole display-name definition recorded before it",
        );
        let mut second_graph = pdb.type_graph().expect("second graph");
        let sole_def_first = second_graph
            .resolve_type(TypeIndex(idx.sole_def))
            .expect("definition first");
        let sole_forward_second = second_graph
            .resolve_type(TypeIndex(idx.sole_forward))
            .expect("forward second");
        check.check(
            sole_def_first == sole_forward_second,
            "definition-first and forward-first resolution converge on one node",
        );

        // A display-name-only forward also matches a complete definition that carries a unique
        // name, because the policy is based on display name when the forward has none.
        let named = graph.resolve_type(TypeIndex(idx.named_forward)).expect("named forward");
        check.check(
            node(&graph, named).index == TypeIndex(idx.named_def),
            "display-name forward matches a unique-named complete definition",
        );

        // A struct forward must not bind to a class definition of the same display name.
        let shape = graph.resolve_type(TypeIndex(idx.shape_forward)).expect("shape");
        check.check(
            node(&graph, shape).index == TypeIndex(idx.shape_forward),
            "struct forward does not match a class definition",
        );
        check.check(
            class_kind(node(&graph, shape)) == Some(ClassKind::Struct)
                && matches!(&node(&graph, shape).kind, TypeNodeKind::Class { complete: false, .. }),
            "unmatched struct forward stays incomplete",
        );

        // The unbound forward is reachable through a procedure, and unrelated queries still work.
        let proc = resolve_named(&mut pdb, &mut graph, "returns_unbound").expect("proc");
        check.check(
            matches!(&node(&graph, proc).kind, TypeNodeKind::Procedure { return_type: Some(rt), .. } if *rt == unbound),
            "procedure return reaches the unbound forward node",
        );
        let int_ptr = graph.resolve_type(TypeIndex(idx.proc)).expect("proc type");
        check.check(
            matches!(node(&graph, int_ptr).kind, TypeNodeKind::Procedure { .. }),
            "supported query unaffected by the earlier ambiguity failure",
        );
    });
}

#[test]
fn recursive() {
    run("recursive", |check| {
        let (types, idx) = fixture::recursive();
        let bytes = fixture::image_scenario(
            &types,
            &[],
            &[
                (0x1110, idx.proc_a, "returns_a"),
                (0x1110, idx.proc_b, "returns_b"),
                (0x1110, idx.shared_proc, "returns_shared"),
            ],
        );
        let mut pdb = PDB::open(Cursor::new(bytes.as_slice())).expect("open pdb");
        let mut graph = pdb.type_graph().expect("type graph");

        // Resolve A first, then B; mutual recursion must terminate and share the class nodes.
        let a = graph.resolve_type(TypeIndex(idx.a_forward)).expect("a");
        let b = graph.resolve_type(TypeIndex(idx.b_forward)).expect("b");
        check.check(a != b, "mutually recursive classes are distinct nodes");
        check.check(
            node(&graph, a).index == TypeIndex(idx.a_def),
            "A forward binds to the A definition",
        );
        check.check(
            node(&graph, b).index == TypeIndex(idx.b_def),
            "B forward binds to the B definition",
        );

        // A.b -> pointer -> B; B.a -> pointer -> A. Both directions land on the shared nodes.
        let a_members = class_members(node(&graph, a)).expect("a members");
        let b_members = class_members(node(&graph, b)).expect("b members");
        check.check(a_members.len() == 1 && b_members.len() == 1, "one member each");
        let a_to_b = match &node(&graph, a_members[0].ty).kind {
            TypeNodeKind::Pointer { to, .. } => *to,
            other => panic!("expected pointer, got {:?}", other),
        };
        check.check(a_to_b == b, "A's pointer reaches the shared B node");
        let b_to_a = match &node(&graph, b_members[0].ty).kind {
            TypeNodeKind::Pointer { to, .. } => *to,
            other => panic!("expected pointer, got {:?}", other),
        };
        check.check(b_to_a == a, "B's pointer reaches the shared A node");

        // Resolving in the other order in a fresh graph yields the same structure.
        let mut reversed = pdb.type_graph().expect("second graph");
        let rb = reversed.resolve_type(TypeIndex(idx.b_forward)).expect("b first");
        let ra = reversed.resolve_type(TypeIndex(idx.a_forward)).expect("a second");
        check.check(
            node(&reversed, rb).index == TypeIndex(idx.b_def),
            "reverse order B binds to the definition",
        );
        check.check(
            node(&reversed, ra).index == TypeIndex(idx.a_def),
            "reverse order A binds to the definition",
        );
        let rbm = class_members(node(&reversed, rb)).expect("b members");
        let r_to_a = match &node(&reversed, rbm[0].ty).kind {
            TypeNodeKind::Pointer { to, .. } => *to,
            other => panic!("expected pointer, got {:?}", other),
        };
        check.check(r_to_a == ra, "reverse order preserves shared identity");

        // A field list continued by LF_INDEX appends tail members after the head members.
        let shared = graph.resolve_type(TypeIndex(idx.shared)).expect("shared");
        let members = class_members(node(&graph, shared)).expect("shared members");
        check.check(members.len() == 2, "continuation members are appended");
        check.check(members[0].name == b"first" && members[0].offset == 0, "head member first");
        check.check(members[1].name == b"second" && members[1].offset == 4, "tail member second");
        check.check(
            members[0].ty == members[1].ty,
            "repeated member type reuses the same primitive node",
        );

        // Procedure roots reach the same class nodes.
        let proc_a = resolve_named(&mut pdb, &mut graph, "returns_a").expect("proc a");
        let proc_b = resolve_named(&mut pdb, &mut graph, "returns_b").expect("proc b");
        let (ret_a, ret_b) = match (&node(&graph, proc_a).kind, &node(&graph, proc_b).kind) {
            (
                TypeNodeKind::Procedure { return_type: Some(ret_a), .. },
                TypeNodeKind::Procedure { return_type: Some(ret_b), .. },
            ) => (*ret_a, *ret_b),
            other => panic!("unexpected procedure kinds: {:?}", other),
        };
        check.check(ret_a == a && ret_b == b, "procedures share the class nodes");
    });
}

#[test]
fn compatibility() {
    run("compatibility", |check| {
        // A valid but unsupported reachable type reports its source and index.
        const BITFIELD: u32 = 0x1000; // LF_BITFIELD (unsupported by the graph)
        const BITFIELD_ARGS: u32 = 0x1001; // LF_ARGLIST containing the bitfield
        const UNSUPPORTED_PROC: u32 = 0x1002; // LF_PROCEDURE taking the bitfield
        const EMPTY_ARGS: u32 = 0x1003; // LF_ARGLIST for an unrelated procedure
        const SUPPORTED_PROC: u32 = 0x1004; // LF_PROCEDURE returning int32
        let types = vec![
            fixture::bitfield(0x74, 3, 0),
            fixture::arglist(&[BITFIELD]),
            fixture::procedure(0x74, BITFIELD_ARGS, 1),
            fixture::arglist(&[]),
            fixture::procedure(0x74, EMPTY_ARGS, 0),
        ];
        let bytes = fixture::image_scenario(
            &types,
            &[],
            &[
                (0x1110, UNSUPPORTED_PROC, "unsupported"),
                (0x1110, SUPPORTED_PROC, "supported"),
            ],
        );
        let mut pdb = PDB::open(Cursor::new(bytes.as_slice())).expect("open pdb");
        let mut graph = pdb.type_graph().expect("type graph");

        let err = graph
            .resolve_type(TypeIndex(UNSUPPORTED_PROC))
            .expect_err("bitfield argument is unsupported");
        match err {
            pdb::Error::TypeGraphUnsupported { source, kind, path } => {
                check.check(
                    source == TypeReference::Type(TypeIndex(BITFIELD)),
                    "unsupported error names the reached record",
                );
                check.check(kind == 0x1205, "unsupported error names the record kind");
                check.check(
                    path.contains(&TypeReference::Type(TypeIndex(BITFIELD))),
                    "unsupported error includes the reference path",
                );
            }
            other => panic!("expected unsupported, got {:?}", other),
        }

        // A later unrelated supported query still works.
        let supported = graph.resolve_type(TypeIndex(SUPPORTED_PROC)).expect("supported query");
        check.check(
            matches!(
                &node(&graph, supported).kind,
                TypeNodeKind::Procedure { arguments, .. } if arguments.is_empty()
            ),
            "a failed query does not poison later supported queries",
        );

        // Nodes returned before a failure remain valid and keep their identity.
        let stable = graph.resolve_type(TypeIndex(SUPPORTED_PROC)).expect("before failure");
        check.check(
            graph.resolve_type(TypeIndex(BITFIELD)).is_err(),
            "the unsupported bitfield still errors",
        );
        check.check(
            graph.resolve_type(TypeIndex(SUPPORTED_PROC)).expect("again") == stable,
            "a node returned before a failure keeps its identifier and contents",
        );

        // A missing IPI is fine for direct TPI queries.
        const DIRECT_ARGS: u32 = 0x1000;
        const DIRECT_PROC: u32 = 0x1001;
        let direct_types = vec![fixture::arglist(&[]), fixture::procedure(0x74, DIRECT_ARGS, 0)];
        let no_ipi = fixture::image_without_ipi(
            &direct_types,
            &fixture::module_stream_with(&[(0x1110, DIRECT_PROC, "direct")]),
        );
        let mut pdb_no_ipi = PDB::open(Cursor::new(no_ipi.as_slice())).expect("open no ipi");
        let mut no_ipi_graph = pdb_no_ipi.type_graph().expect("graph without ipi");
        let direct = no_ipi_graph
            .resolve_type(TypeIndex(DIRECT_PROC))
            .expect("direct tpi query without ipi");
        check.check(
            matches!(node(&no_ipi_graph, direct).kind, TypeNodeKind::Procedure { .. }),
            "direct TPI query works without an IPI stream",
        );

        // A procedure with an ID reference fails clearly when the IPI is missing.
        let id_image = fixture::image_without_ipi(
            &direct_types,
            &fixture::module_stream_with(&[(0x1147, DIRECT_PROC, "through_id")]),
        );
        let mut pdb_id = PDB::open(Cursor::new(id_image.as_slice())).expect("open id image");
        let mut id_graph = pdb_id.type_graph().expect("graph");
        let mut failures = 0;
        // Collect the symbol, then resolve against the graph.
        {
            let dbi = pdb_id.debug_information().expect("dbi");
            let module = dbi.modules().expect("modules").next().expect("module").expect("module");
            let info = pdb_id.module_info(&module).expect("info").expect("stream");
            let mut symbols = info.symbols().expect("symbols");
            while let Some(symbol) = symbols.next().expect("symbol") {
                if let SymbolData::Procedure(procedure) = symbol.parse().expect("parse") {
                    match id_graph.resolve_procedure(&procedure) {
                        Err(pdb::Error::TypeGraphMissingId(id)) => {
                            check.check(id == IdIndex(DIRECT_PROC), "missing id error names the id");
                            failures += 1;
                        }
                        other => panic!("expected missing id, got {:?}", other),
                    }
                }
            }
        }
        check.check(failures == 1, "one ID procedure reported a missing IPI record");

        // An `LF_FUNC_ID` that names a member function is a kind mismatch, not a silent success.
        let mut mismatch = fixture::TypeBuilder::new();
        let mismatch_args = mismatch.arglist(&[]);
        mismatch.procedure(0x74, mismatch_args, 0, 0); // 0x1001 LF_PROCEDURE
        let mismatch_this = mismatch.pointer(0x74, 0x1000c); // 0x1002
        let mismatch_method_args = mismatch.arglist(&[]); // 0x1003
        let mismatch_fields = mismatch.fieldlist(&[]); // 0x1004
        let mismatch_class = mismatch.complete(0x1504, "K", None, mismatch_fields, 4); // 0x1005
        let mismatch_method =
            mismatch.member_function(0x74, mismatch_class, mismatch_this, 0, 0, mismatch_method_args, 0); // 0x1006
        let mismatch_types = mismatch.finish();
        // LF_FUNC_ID must reference an LF_PROCEDURE, but points at the LF_MFUNCTION instead.
        // The IPI stream's only record is at id index 0x1000, which the symbol names.
        let mismatch_ids = vec![fixture::function_id("wrong_kind", mismatch_method)];
        let mismatch_image = fixture::image_scenario(
            &mismatch_types,
            &mismatch_ids,
            &[(0x1147, 0x1000, "wrong_kind")],
        );
        let mut mismatch_pdb =
            PDB::open(Cursor::new(mismatch_image.as_slice())).expect("mismatch pdb");
        let mut mismatch_graph = mismatch_pdb.type_graph().expect("mismatch graph");
        let mismatch = resolve_named(&mut mismatch_pdb, &mut mismatch_graph, "wrong_kind");
        match mismatch {
            Err(pdb::Error::TypeGraphProcedureMismatch { source, .. }) => {
                check.check(
                    source == TypeReference::Id(IdIndex(0x1000)),
                    "mismatch error names the IPI id and the wrong-kind failure",
                );
            }
            other => panic!("expected mismatch, got {:?}", other),
        }

        // Lvalue and rvalue references are ordinary pointer nodes with their mode preserved, while
        // a pointer-to-member is a valid unsupported record.
        let mut refs = fixture::TypeBuilder::new();
        let lref = refs.pointer(0x74, 0x1000c | (0x01 << 5)); // lvalue reference to int32
        let rref = refs.pointer(0x74, 0x1000c | (0x04 << 5)); // rvalue reference to int32
        let member_ptr = refs.pointer(0x74, 0x1000c | (0x02 << 5)); // pointer to data member
        let ref_args = refs.arglist(&[lref, rref]);
        let ref_proc = refs.procedure(0x74, ref_args, 2, 0);
        let ref_types = refs.finish();
        let ref_image = fixture::image_scenario(
            &ref_types,
            &[],
            &[(0x1110, ref_proc, "takes_refs")],
        );
        let mut ref_pdb = PDB::open(Cursor::new(ref_image.as_slice())).expect("ref pdb");
        let mut ref_graph = ref_pdb.type_graph().expect("ref graph");
        let ref_node = ref_graph.resolve_type(TypeIndex(ref_proc)).expect("ref proc");
        let ref_args_nodes = match &node(&ref_graph, ref_node).kind {
            TypeNodeKind::Procedure { arguments, .. } => arguments.clone(),
            other => panic!("expected Procedure, got {:?}", other),
        };
        for (arg, expected_reference) in [(ref_args_nodes[0], true), (ref_args_nodes[1], true)] {
            match &node(&ref_graph, arg).kind {
                TypeNodeKind::Pointer { attributes, .. } => check.check(
                    attributes.is_reference() == expected_reference,
                    "reference pointer mode preserved",
                ),
                other => panic!("expected Pointer, got {:?}", other),
            }
        }
        check.check(
            ref_graph.resolve_type(TypeIndex(member_ptr)).is_err(),
            "pointer-to-member is reported as unsupported",
        );

        // Self-recursion through the original fixture's `Node` type: the forward declaration, the
        // complete definition, and the `next` pointer all land on one node.
        let mut node_pdb = PDB::open(Cursor::new(fixture::image())).expect("node pdb");
        let mut node_graph = node_pdb.type_graph().expect("node graph");
        let node_forward = node_graph.resolve_type(TypeIndex(0x1000)).expect("forward");
        let node_complete = node_graph.resolve_type(TypeIndex(0x1004)).expect("complete");
        check.check(
            node_forward == node_complete,
            "self-recursive forward and definition share one node",
        );
        let node_members = class_members(node(&node_graph, node_forward)).expect("members");
        check.check(
            node_members.len() == 2
                && node_members[0].name == b"value"
                && node_members[1].name == b"next",
            "self-recursive members are ordered across the LF_INDEX continuation",
        );
        let next_to = match &node(&node_graph, node_members[1].ty).kind {
            TypeNodeKind::Pointer { to, .. } => *to,
            other => panic!("expected Pointer, got {:?}", other),
        };
        check.check(
            next_to == node_forward,
            "the recursive pointer returns to the same class node",
        );

        // The original fixture keeps its exact legacy parse results, iteration order, and bytes.
        let original = fixture::image();
        let original_copy = original.clone();
        let mut original_pdb =
            PDB::open(Cursor::new(original.as_slice())).expect("original pdb");
        let original_procs = collect_procedures(&mut original_pdb);
        let original_pairs: Vec<(&str, u32)> = original_procs
            .iter()
            .map(|p| (p.name.as_str(), p.type_index.0))
            .collect();
        check.check(
            original_pairs == [("direct", 0x1007), ("through_id", 0x1007), ("member_id", 0x1008)],
            "original fixture procedure order and raw type bits are unchanged",
        );
        check.check(
            original_procs[0].type_reference == TypeReference::Type(TypeIndex(0x1007))
                && original_procs[1].type_reference == TypeReference::Id(IdIndex(0x1007))
                && original_procs[2].type_reference == TypeReference::Id(IdIndex(0x1008)),
            "original fixture distinguishes TPI and IPI references",
        );
        let original_tpi = original_pdb.type_information().expect("original tpi");
        let mut original_finder = original_tpi.finder();
        let mut original_iter = original_tpi.iter();
        let mut original_count = 0;
        while let Some(item) = original_iter.next().expect("next type") {
            original_finder.update(&original_iter);
            original_count += 1;
            if item.index() != TypeIndex(0x1003) {
                let _ = item.parse().expect("legacy parse");
            }
        }
        check.check(original_count == 11, "original fixture still has eleven TPI records");
        // The legacy `TypeData::FieldList` decoder keeps its historical (unpadded) LF_INDEX
        // interpretation, which the baseline example exposes as an error for this record.
        check.check(
            matches!(
                original_finder
                    .find(TypeIndex(0x1003))
                    .expect("find continuation")
                    .parse(),
                Err(pdb::Error::UnimplementedTypeKind(0))
            ),
            "legacy FieldList parse result for the padded LF_INDEX record is unchanged",
        );
        check.check(
            original == original_copy,
            "the original fixture bytes are never modified",
        );

        // IPI `scope` is not an extra type edge: an id whose scope names a missing record still
        // resolves through its function_type alone.
        let scoped_types = vec![
            fixture::arglist(&[]),
            fixture::procedure(0x74, 0x1000, 0),
        ];
        let scoped_ids = vec![fixture::function_id_scoped(0x7fff, "scoped", 0x1001)];
        let scoped_image = fixture::image_scenario(
            &scoped_types,
            &scoped_ids,
            &[(0x1147, 0x1000, "scoped")],
        );
        let mut scoped_pdb = PDB::open(Cursor::new(scoped_image.as_slice())).expect("scoped pdb");
        let mut scoped_graph = scoped_pdb.type_graph().expect("scoped graph");
        let scoped_node =
            resolve_named(&mut scoped_pdb, &mut scoped_graph, "scoped").expect("scoped resolve");
        check.check(
            node(&scoped_graph, scoped_node).index == TypeIndex(0x1001),
            "id scope is not followed as a type edge",
        );

        // The legacy parse API continues to work unchanged, and its iteration order is preserved.
        let legacy_bytes = origin_image().0;
        let legacy_copy = legacy_bytes.clone();
        let mut legacy = PDB::open(Cursor::new(legacy_bytes.as_slice())).expect("legacy pdb");
        let legacy_procs = collect_procedures(&mut legacy);
        let names: Vec<&str> = legacy_procs.iter().map(|p| p.name.as_str()).collect();
        check.check(
            names == ["ordinary", "through_id", "member_id"],
            "legacy procedure iteration order is unchanged",
        );
        let type_information = legacy.type_information().expect("tpi");
        let mut iter = type_information.iter();
        let mut indexes = Vec::new();
        while let Some(item) = iter.next().expect("next type") {
            indexes.push(item.index());
            let _ = item.parse();
        }
        check.check(
            indexes.len() == fixture::origin().0.len(),
            "legacy TPI iteration still yields every record",
        );
        check.check(
            legacy_bytes == legacy_copy,
            "the input bytes are never modified",
        );

        // A graph outlives its PDB, module views and source.
        let mut owned_graph = {
            let bytes = origin_image().0;
            let mut pdb = PDB::open(Cursor::new(bytes)).expect("owned pdb");
            pdb.type_graph().expect("owned graph")
        };
        let owned = owned_graph
            .resolve_type(TypeIndex(0x1003))
            .expect("resolve after pdb drop");
        check.check(
            matches!(node(&owned_graph, owned).kind, TypeNodeKind::Procedure { .. }),
            "graph remains usable after the PDB is dropped",
        );

        // Two graphs from two PDBs never share nodes, even at equal numeric indices.
        // The second PDB uses a deliberately different TPI record at index 0x1003, so observing
        // the first graph's cache would be visible in the node's contents.
        let (mut different_types, different_ids, different_idx) = fixture::origin();
        different_types[(different_idx.proc - 0x1000) as usize] =
            fixture::procedure(0x74, different_idx.args, 2);
        let different = fixture::image_scenario(
            &different_types,
            &different_ids,
            &[(0x1110, different_idx.proc, "ordinary")],
        );
        let mut first = PDB::open(Cursor::new(origin_image().0)).expect("first pdb");
        let mut second = PDB::open(Cursor::new(different.as_slice())).expect("second pdb");
        let mut first_graph = first.type_graph().expect("first graph");
        let mut second_graph = second.type_graph().expect("second graph");
        let (_, origin_idx) = origin_image();
        let first_node = first_graph
            .resolve_type(TypeIndex(origin_idx.proc))
            .expect("first node");
        // Build lots of nodes in the first graph; the second must not see any of them.
        first_graph
            .resolve_type(TypeIndex(origin_idx.class_c))
            .expect("first graph class");
        let second_node = second_graph
            .resolve_type(TypeIndex(origin_idx.proc))
            .expect("second node");
        check.check(
            second_graph.node(second_node).map(|n| n.index) == Some(TypeIndex(origin_idx.proc)),
            "second graph resolves its own record, not the first graph's cached node",
        );
        let first_ret = match &node(&first_graph, first_node).kind {
            TypeNodeKind::Procedure { return_type, .. } => return_type.expect("first return"),
            other => panic!("expected Procedure, got {:?}", other),
        };
        let second_ret = match &node(&second_graph, second_node).kind {
            TypeNodeKind::Procedure { return_type, .. } => return_type.expect("second return"),
            other => panic!("expected Procedure, got {:?}", other),
        };
        check.check(
            matches!(node(&first_graph, first_ret).kind, TypeNodeKind::Modifier { .. }),
            "first graph returns the const modifier from its own record",
        );
        check.check(
            matches!(node(&second_graph, second_ret).kind, TypeNodeKind::Primitive { .. }),
            "second graph returns the plain primitive from its own record",
        );
        // Node identifiers may coincide numerically, but they denote unrelated nodes.
        check.check(
            format!("{:?}", first_node) == format!("{:?}", second_node),
            "identifier numbers are graph-local, not globally meaningful",
        );

        // Interleaving different procedures reuses shared types and preserves identity.
        let (origin_types, origin_ids, origin_idx) = fixture::origin();
        let interleaved = fixture::image_scenario(
            &origin_types,
            &origin_ids,
            &[
                (0x1110, origin_idx.proc, "ordinary"),
                (0x1147, origin_idx.func_id, "through_id"),
                (0x1146, origin_idx.method_id, "member_id"),
            ],
        );
        let mut interleaved_pdb =
            PDB::open(Cursor::new(interleaved.as_slice())).expect("interleaved pdb");
        let mut interleaved_graph = interleaved_pdb.type_graph().expect("interleaved graph");
        let first_proc =
            resolve_named(&mut interleaved_pdb, &mut interleaved_graph, "member_id").expect("member");
        let second_proc =
            resolve_named(&mut interleaved_pdb, &mut interleaved_graph, "ordinary").expect("ordinary");
        let member_args = match &node(&interleaved_graph, first_proc).kind {
            TypeNodeKind::MemberFunction { arguments, .. } => arguments.clone(),
            other => panic!("expected MemberFunction, got {:?}", other),
        };
        let ordinary_args = match &node(&interleaved_graph, second_proc).kind {
            TypeNodeKind::Procedure { arguments, .. } => arguments.clone(),
            other => panic!("expected Procedure, got {:?}", other),
        };
        check.check(
            member_args[0] == ordinary_args[0]
                && member_args[1] == ordinary_args[1],
            "interleaved procedures share their argument nodes",
        );
        check.check(
            member_args[0] == interleaved_graph.resolve_type(TypeIndex(origin_idx.int_ptr)).expect("ptr"),
            "shared nodes match a direct root query",
        );
    });
}
