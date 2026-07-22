//! example_client.rs — connect to the example server and use every capability.
//!
//!   cargo build --examples
//!   cargo run --example example_client

use rcp::Capability;

fn main() -> Result<(), rcp::RcpError> {
    // The example_server binary sits next to this one in target/.../examples/.
    let exe = std::env::current_exe().expect("current exe");
    let server = exe.parent().unwrap().join("example_server");
    let server = server.to_string_lossy().to_string();

    let mut c = rcp::connect_stdio(&[server.as_str()])?;

    println!(
        "connected to {} v{} (RCP/{})",
        c.server().get_str("name").unwrap_or("?"),
        c.server().get_str("version").unwrap_or("?"),
        c.protocol_version()
    );
    if let Some(caps) = c.capabilities().as_object() {
        let keys: Vec<&str> = caps.iter().map(|(k, _)| k.as_str()).collect();
        println!("capabilities: {:?}", keys);
    }

    if c.supports(Capability::Embed) {
        let vecs = c.embed(&["hello world"], None)?;
        println!("embed      -> {} vector(s), dim={}", vecs.len(), vecs[0].len());
    }

    if c.supports(Capability::Retrieve) {
        println!("retrieve   ->");
        for h in c.retrieve("landmark in the French capital", 2)? {
            let snippet: String = h.text.chars().take(48).collect();
            println!("   {}  score={:.3}  {}", h.id, h.score, snippet);
        }
    }

    if c.supports(Capability::Graph) {
        let g = c.graph("global", Some(rcp::obj(&[("query", "anything".into())])))?;
        println!("graph      -> {}", g);
    }

    c.shutdown();
    Ok(())
}
