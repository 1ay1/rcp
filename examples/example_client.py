#!/usr/bin/env python3
"""Example RCP/1 client — connect to the example server and use every capability."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sdk", "python"))

import rcp  # noqa: E402


def main():
    server = os.path.join(os.path.dirname(os.path.abspath(__file__)), "example_server.py")
    c = rcp.connect_stdio(["python3", server])

    print(f"connected to {c.server['name']} v{c.server['version']} (RCP/{c.protocol_version})")
    print("capabilities:", list(c.capabilities.keys()) if hasattr(c.capabilities, "keys") else c.capabilities)

    if c.supports(rcp.Capability.Embed):
        vecs = c.embed(["hello world"])
        print(f"embed      -> {len(vecs)} vector(s), dim={len(vecs[0])}")

    if c.supports(rcp.Capability.Retrieve):
        print("retrieve   ->")
        for h in c.retrieve("landmark in the French capital", k=2):
            print(f"   {h['id']}  score={h['score']:.3f}  {h['text'][:48]}")

    if c.supports(rcp.Capability.Graph):
        g = c.graph("global", {"query": "anything"})
        print("graph      ->", g)


if __name__ == "__main__":
    main()
