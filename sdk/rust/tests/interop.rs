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
    println!("client<->C++: ok, top hit = {}", hits[0].id);

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
