#pragma once
// rcp.hpp — umbrella for the type-theoretic RCP C++ SDK.
//
// RCP is an open, versioned JSON-RPC 2.0 protocol so any RAG engine — any
// language, any vendor — can expose embed/rerank/retrieve/graph/index, and any
// client can consume it uniformly. This SDK encodes the protocol's invariants
// in the C++ type system so they are proved at COMPILE time (the build is the
// test runner): typestate client, refinement types (TopK can't be 0), strong
// scalars (Score/Dimension), a Handler concept, and Result<T> totality.
//
//   Client (typestate — only exists after the handshake):
//     auto cli = rcp::Client::connect_stdio({"python3", "server.py"});
//     if (cli) { auto hits = cli->retrieve("query", rcp::TopK::trusted(5)); }
//
//   Server (concept-constrained — capabilities derive from implemented hooks):
//     struct Engine { PeerInfo info() const; Capabilities capabilities() const;
//                     Result<Json> retrieve(const Json&); };
//     rcp::Server{Engine{}}.serve_stdio();
//
// See spec/rcp-1.0.md. MIT licensed.

#include "rcp/types.hpp"
#include "rcp/protocol.hpp"
#include "rcp/filter.hpp"
#include "rcp/transport.hpp"
#include "rcp/client.hpp"
#include "rcp/selector.hpp"
#include "rcp/server.hpp"
#include "rcp/federation.hpp"
