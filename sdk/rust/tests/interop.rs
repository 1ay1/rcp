//! interop.rs — integration tests for the Rust RCP SDK: in-proc dispatch,
//! cross-language client↔C++ server, and registry-driven selection.
//!
//! Run: cargo test

use rcp::{obj, Capability, Json, Method, Server};

fn req(id: i64, method: &str, params: Json) -> Json {
    obj(&[
        ("jsonrpc", "2.0".into()),
        ("id", id.into()),
        ("method", method.into()),
        ("params", params),
    ])
}

fn err_code(reply: &Json) -> Option<i64> {
    reply.get("error").and_then(|e| e.get("code")).and_then(|c| c.as_i64())
}

#[test]
fn server_dispatch_inproc() {
    let mut s = Server::new();
    s.set_info("rust-engine", "1.0");
    s.advertise(Capability::Retrieve, obj(&[("maxK", 100.into())]));
    s.on(Method::RETRIEVE, |p| {
        let q = p.get_str("query").unwrap_or("");
        Ok(obj(&[(
            "hits",
            vec![obj(&[("id", "d1".into()), ("score", 0.9.into()), ("text", q.into())])].into(),
        )]))
    });

    // pre-initialize -> NotInitialized
    let r = s
        .handle(&req(1, "retrieve", obj(&[("query", "x".into()), ("k", 1.into())])))
        .unwrap();
    assert_eq!(err_code(&r), Some(-32001), "{}", r);

    // initialize -> capabilities has retrieve, not embed
    let init = s.handle(&req(2, "initialize", obj(&[("protocolVersion", 1.into())]))).unwrap();
    let caps = init.get("result").unwrap().get("capabilities").unwrap();
    assert!(caps.contains_key("retrieve") && !caps.contains_key("embed"), "{}", init);

    // retrieve echoes the query
    let ret = s.handle(&req(3, "retrieve", obj(&[("query", "hello".into()), ("k", 1.into())]))).unwrap();
    let hits = ret.get("result").unwrap().get("hits").unwrap().as_array().unwrap();
    assert_eq!(hits[0].get_str("text"), Some("hello"));

    // unadvertised capability -> CapabilityMissing
    let emb = s.handle(&req(4, "embed", obj(&[("texts", vec![Json::from("a")].into())]))).unwrap();
    assert_eq!(err_code(&emb), Some(-32003), "{}", emb);

    // unknown method -> UnknownMethod
    let unk = s.handle(&req(5, "no/such", Json::object())).unwrap();
    assert_eq!(err_code(&unk), Some(-32004), "{}", unk);

    // batch: one reply per request, notification drops out
    let batch = Json::Array(vec![
        req(6, "ping", obj(&[("nonce", 1.into())])),
        obj(&[
            ("jsonrpc", "2.0".into()),
            ("method", "notifications/cancel".into()),
            ("params", obj(&[("id", 6.into())])),
        ]),
        req(7, "info", Json::object()),
    ])
    .dump();
    let out = s.handle_line(&batch);
    let arr = Json::parse(&out).unwrap();
    assert_eq!(arr.as_array().unwrap().len(), 2, "{}", out);
    assert_eq!(arr.as_array().unwrap()[0].get("result").unwrap().get("nonce").unwrap().as_i64(), Some(1));

    println!("server in-proc: ok");
}

#[test]
fn client_against_cpp_server() {
    let cpp = format!("{}/../cpp/example_server", env!("CARGO_MANIFEST_DIR"));
    if !std::path::Path::new(&cpp).exists() {
        eprintln!("client<->C++: skipped (build sdk/cpp/example_server first)");
        return;
    }
    let mut c = rcp::connect_stdio(&[cpp.as_str()]).expect("connect to C++ server");
    assert_eq!(c.protocol_version(), 1);
    assert!(c.supports(Capability::Retrieve));
    assert!(!c.supports(Capability::Rerank));

    let hits = c.retrieve("landmark in the French capital", 2).unwrap();
    assert_eq!(hits.len(), 2);
    assert!(!hits[0].id.is_empty());
    // the C++ server now returns agentic/eval fields — assert they cross the wire
    // and populate the typed Hit struct.
    let conf = hits[0].confidence.expect("confidence present");
    assert!((0.0..=1.0).contains(&conf));
    assert_eq!(hits[0].unit.as_deref(), Some("chunk"));
    assert_eq!(
        hits[0].trust.as_ref().and_then(|t| t.get("level")).and_then(|v| v.as_str()),
        Some("trusted")
    );
    println!("client<->C++: ok, top hit = {}", hits[0].id);

    // feedback + memory round-trip across languages against the C++ server
    if c.supports(Capability::Feedback) {
        let fb = c
            .feedback(
                vec![obj(&[("hitId", hits[0].id.clone().into()), ("used", true.into()), ("reward", 0.9.into())])],
                None,
            )
            .unwrap();
        assert_eq!(fb.get("accepted").and_then(|v| v.as_i64()), Some(1), "{}", fb);
        println!("client<->C++ feedback: ok");
    }
    if c.supports(Capability::Memory) {
        let mr = c.memory_recall("french landmark", None).unwrap();
        assert!(mr.get("clues").and_then(|v| v.as_array()).map(|a| !a.is_empty()).unwrap_or(false), "{}", mr);
        println!("client<->C++ memory: ok");
    }

    let pong = c.ping(Some(Json::from(123))).unwrap();
    assert_eq!(pong.get("nonce").and_then(|v| v.as_i64()), Some(123));

    // capability gating fails client-side with a message mentioning 'rerank'
    match c.rerank("q", &["a", "b"]) {
        Err(e) => assert!(e.to_string().contains("rerank"), "{}", e),
        Ok(_) => panic!("expected a gating error"),
    }
    c.shutdown();
}

#[test]
fn agentic_surfaces() {
    // feedback + memory dispatch, capability gating, and full Hit-field parsing.
    let mut s = Server::new();
    s.set_info("rust-agentic", "1.0");
    s.advertise(
        Capability::Retrieve,
        obj(&[("maxK", 100.into()), ("confidence", true.into())]),
    );
    s.advertise(Capability::Session, obj(&[("dedup", true.into())]));
    s.advertise(Capability::Feedback, Json::object());
    s.advertise(Capability::Memory, obj(&[("clues", true.into())]));

    s.on(Method::RETRIEVE, |p| {
        let unit = p.get_str("unit").unwrap_or("chunk");
        Ok(obj(&[(
            "hits",
            vec![obj(&[
                ("id", 42.into()),
                ("score", 0.9.into()),
                ("text", "x".into()),
                ("confidence", 0.83.into()),
                ("unit", unit.into()),
                ("level", 2.into()),
                ("provenance", obj(&[("path", vec![Json::from("e:a")].into())])),
                ("trust", obj(&[("level", "trusted".into())])),
                ("scores", obj(&[("dense", 0.7.into())])),
            ])]
            .into(),
        )]))
    });
    s.on(Method::FEEDBACK, |p| {
        let n = p.get("signals").and_then(|v| v.as_array()).map(|a| a.len()).unwrap_or(0);
        Ok(obj(&[("accepted", (n as i64).into())]))
    });
    s.on(Method::MEMORY_BUILD, |_p| {
        Ok(obj(&[("memoryId", "mem-1".into()), ("tokens", 2048.into())]))
    });
    s.on(Method::MEMORY_RECALL, |_p| {
        Ok(obj(&[(
            "clues",
            vec![obj(&[("query", "sub-q".into()), ("weight", 0.9.into())])].into(),
        )]))
    });

    s.handle(&req(1, "initialize", obj(&[("protocolVersion", 1.into())]))).unwrap();

    // retrieve carries agentic knobs through; hit fields parse via hit_from_json
    let ret = s
        .handle(&req(
            2,
            "retrieve",
            obj(&[("query", "q".into()), ("k", 1.into()), ("unit", "tree-node".into())]),
        ))
        .unwrap();
    let hit = &ret.get("result").unwrap().get("hits").unwrap().as_array().unwrap()[0];
    assert_eq!(hit.get("confidence").and_then(|v| v.as_f64()), Some(0.83));
    assert_eq!(hit.get_str("unit"), Some("tree-node"));
    assert_eq!(
        hit.get("provenance").unwrap().get("path").unwrap().as_array().unwrap()[0].as_str(),
        Some("e:a")
    );

    // feedback
    let fb = s
        .handle(&req(
            3,
            "feedback",
            obj(&[(
                "signals",
                vec![obj(&[("hitId", "42".into()), ("used", true.into()), ("reward", 0.8.into())])].into(),
            )]),
        ))
        .unwrap();
    assert_eq!(fb.get("result").unwrap().get("accepted").unwrap().as_i64(), Some(1), "{}", fb);

    // memory build + recall
    let mb = s.handle(&req(4, "memory/build", obj(&[("scope", "global".into())]))).unwrap();
    assert_eq!(mb.get("result").unwrap().get_str("memoryId"), Some("mem-1"));
    let mr = s.handle(&req(5, "memory/recall", obj(&[("query", "q".into())]))).unwrap();
    assert_eq!(
        mr.get("result").unwrap().get("clues").unwrap().as_array().unwrap().len(),
        1
    );

    // unadvertised memory -> CapabilityMissing
    let mut bare = Server::new();
    bare.set_info("bare", "1.0");
    bare.advertise(Capability::Retrieve, Json::object());
    bare.handle(&req(1, "initialize", obj(&[("protocolVersion", 1.into())]))).unwrap();
    let miss = bare.handle(&req(2, "memory/recall", obj(&[("query", "q".into())]))).unwrap();
    assert_eq!(err_code(&miss), Some(-32003), "{}", miss);

    println!("agentic surfaces: ok");
}

#[test]
fn registry_selector() {
    // Registry pointing at the repo's Python example server (proves Selector +
    // Rust-client ↔ Python-server interop).
    let py = format!("{}/../../examples/example_server.py", env!("CARGO_MANIFEST_DIR"));
    let reg = format!(
        r#"{{ "engines": [ {{ "id": "docs", "transport": "stdio", "command": ["python3", "{}"], "priority": 10 }} ] }}"#,
        py
    );
    let sel = rcp::Selector::loads(&reg).unwrap();
    assert_eq!(sel.size(), 1);
    let mut c = sel.select_primary().expect("select_primary");
    assert!(c.supports(Capability::Retrieve));
    let hits = c.retrieve("french landmark", 1).unwrap();
    assert_eq!(hits.len(), 1);
    c.shutdown();
    println!("registry selector: ok");
}
