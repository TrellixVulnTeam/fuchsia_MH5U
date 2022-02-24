// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_
#define SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_

#include <fidl/fuchsia.shell/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/stdcompat/optional.h>
#include <zircon/status.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "src/developer/shell/interpreter/src/expressions.h"
#include "src/developer/shell/interpreter/src/interpreter.h"
#include "src/developer/shell/interpreter/src/schema.h"

namespace shell {
namespace interpreter {
namespace server {

class Server;
class Service;

// Holds a context at the server level.
class ServerInterpreterContext {
 public:
  explicit ServerInterpreterContext(ExecutionContext* execution_context)
      : execution_context_(execution_context) {}

  ExecutionContext* execution_context() const { return execution_context_; }

  // True if there are unused AST nodes.
  bool PendingNodes() const { return !expressions_.empty() || !instructions_.empty(); }

  // Adds an expression to the context. This expression must be used later by another node.
  void AddExpression(std::unique_ptr<Expression> expression) {
    expressions_.emplace(expression->id(), std::move(expression));
  }

  // Adds an instruction to the context. This instruction must be used later by another node.
  void AddInstruction(std::unique_ptr<Instruction> instruction) {
    instructions_.emplace(instruction->id(), std::move(instruction));
  }

  // Adds a field schema definition to the context. This definition must be used later by another
  // node.
  void AddObjectFieldSchema(std::shared_ptr<ObjectFieldSchema> field) {
    object_field_schemas_.emplace(field->id(), field);
  }

  // Adds an object field to the context. This definition must be used later by another node.
  void AddObjectField(std::unique_ptr<ObjectDeclarationField> field) {
    fields_.emplace(field->id(), std::move(field));
  }

  // Retrieves the expression for the given node id. If the expression is found, the expression is
  // removes from the waiting instruction map.
  std::unique_ptr<Expression> GetExpression(const NodeId& node_id);

  // Retrieves the field definition for the given node id.
  std::unique_ptr<ObjectDeclarationField> GetObjectField(const NodeId& node_id);

  // Retrieves the field definition for the given node id.
  std::shared_ptr<ObjectFieldSchema> GetObjectFieldSchema(const NodeId& node_id);

 private:
  // The execution context (interpreter level) associated with this context.
  ExecutionContext* const execution_context_;
  // All the expressions waiting to be used.
  std::map<NodeId, std::unique_ptr<Expression>> expressions_;
  // All the instructions waiting to be used.
  std::map<NodeId, std::unique_ptr<Instruction>> instructions_;
  // All of the fields waiting to be used.
  std::map<NodeId, std::unique_ptr<ObjectDeclarationField>> fields_;
  // All of the fields waiting to be used.
  std::map<NodeId, std::shared_ptr<ObjectFieldSchema>> object_field_schemas_;
};

// Defines an interpreter managed by a server.
class ServerInterpreter : public Interpreter {
 public:
  explicit ServerInterpreter(Service* service) : service_(service) {}

  void EmitError(ExecutionContext* context, std::string error_message) override;
  void EmitError(ExecutionContext* context, NodeId node_id, std::string error_message) override;
  void DumpDone(ExecutionContext* context) override;
  void ContextDone(ExecutionContext* context) override;
  void ContextDoneWithAnalysisError(ExecutionContext* context) override;
  void ContextDoneWithExecutionError(ExecutionContext* context) override;
  void TextResult(ExecutionContext* context, std::string_view text) override;
  void Result(ExecutionContext* context, const Value& result) override;

  // Gets the server context for the given id.
  ServerInterpreterContext* GetServerContext(uint64_t id) {
    auto context = contexts_.find(id);
    if (context != contexts_.end()) {
      return context->second.get();
    }
    return nullptr;
  }

  // Creates a server context associated with the interpreter context.
  void CreateServerContext(ExecutionContext* context);

  // Erases a server context.
  void EraseServerContext(uint64_t context_id) { contexts_.erase(context_id); }

  // Adds an expression to this context. The expression then waits to be used by another node.
  // The argument root_node should always be false.
  void AddExpression(ServerInterpreterContext* context, std::unique_ptr<Expression> expression,
                     bool root_node);

  // Adds an instruction to this context. If root_node is true, the instruction is added to the
  // interpreter context's pending instruction list.
  // If global_node is false, the instruction waits to be used by another node.
  void AddInstruction(ServerInterpreterContext* context, std::unique_ptr<Instruction> instruction,
                      bool global_node);

  // Adds a object schema definition to this context.  The definition can then be referred to by
  // other nodes. The argument root_node should always be false.
  void AddObjectSchema(ServerInterpreterContext* context, std::shared_ptr<ObjectSchema> definition,
                       bool root_node);

  void AddObjectFieldSchema(ServerInterpreterContext* context,
                            std::shared_ptr<ObjectFieldSchema> definitions, bool root_node);

  // Adds a field to this context.  The definition can then be referred to by
  // other nodes. The argument root_node should always be false.
  void AddObjectField(ServerInterpreterContext* context,
                      std::unique_ptr<ObjectDeclarationField> definition, bool root_node);

  // Retrives the expression for the given context/node id. If the expression is not found, it emits
  // an error.
  std::unique_ptr<Expression> GetNullableExpression(ServerInterpreterContext* context,
                                                    const NodeId& node_id);

  // Retrives the expression for the given context/node id. If the expression is not found, or if
  // the expression is null, it emits an error.
  std::unique_ptr<Expression> GetExpression(ServerInterpreterContext* context,
                                            const NodeId& container_id, const std::string& member,
                                            const NodeId& node_id);

  std::shared_ptr<ObjectFieldSchema> GetObjectFieldSchema(ServerInterpreterContext* context,
                                                          const NodeId& node_id);

 private:
  // The service which currently holds the interpreter.
  Service* service_;
  // All the server contexts.
  std::map<uint64_t, std::unique_ptr<ServerInterpreterContext>> contexts_;
};

// Defines a connection from a client to the interpreter.
class Service final : public fidl::WireServer<fuchsia_shell::Shell> {
 public:
  explicit Service(Server* server)
      : server_(server), interpreter_(std::make_unique<ServerInterpreter>(this)) {}

  ~Service() final;

  Interpreter* interpreter() const { return interpreter_.get(); }

  // Sets the binding related to this connection which allows sending events.
  // It should be called right after message dispatching is scheduled
  // to happen on this Service.
  void set_binding(fidl::ServerBindingRef<fuchsia_shell::Shell> binding) {
    binding_.emplace(std::move(binding));
  }

  fidl::ServerBindingRef<fuchsia_shell::Shell>& binding() { return binding_.value(); }

  // Called by |Server| to notify that the |Server| object will be destroyed.
  // The service should close its connection and schedule its destruction too.
  void OnServerShutdown();

  void CreateExecutionContext(CreateExecutionContextRequestView request,
                              CreateExecutionContextCompleter::Sync& completer) override;
  void AddNodes(AddNodesRequestView request, AddNodesCompleter::Sync& _completer) override;
  void DumpExecutionContext(DumpExecutionContextRequestView request,
                            DumpExecutionContextCompleter::Sync& completer) override;
  void ExecuteExecutionContext(ExecuteExecutionContextRequestView request,
                               ExecuteExecutionContextCompleter::Sync& completer) override;
  void Shutdown(ShutdownRequestView request, ShutdownCompleter::Sync& completer) override;

  // Helpers to be able to send events to the client.
  fidl::Result OnError(uint64_t context_id,
                       fidl::VectorView<fuchsia_shell::wire::Location>&& locations,
                       const std::string& error_message) {
    return fidl::WireSendEvent(binding_.value())
        ->OnError(context_id, std::move(locations), fidl::StringView::FromExternal(error_message));
  }

  fidl::Result OnError(uint64_t context_id, const std::string& error_message) {
    fidl::VectorView<fuchsia_shell::wire::Location> locations;
    return OnError(context_id, std::move(locations), error_message);
  }

  fidl::Result OnDumpDone(uint64_t context_id) {
    return fidl::WireSendEvent(binding_.value())->OnDumpDone(context_id);
  }

  fidl::Result OnExecutionDone(uint64_t context_id, fuchsia_shell::wire::ExecuteResult result) {
    return fidl::WireSendEvent(binding_.value())->OnExecutionDone(context_id, result);
  }

  fidl::Result OnTextResult(uint64_t context_id, const std::string& result, bool partial_result) {
    return fidl::WireSendEvent(binding_.value())
        ->OnTextResult(context_id, fidl::StringView::FromExternal(result), partial_result);
  }

  fidl::Result OnResult(uint64_t context_id, fidl::VectorView<fuchsia_shell::wire::Node>&& nodes,
                        bool partial_result) {
    return fidl::WireSendEvent(binding_.value())
        ->OnResult(context_id, std::move(nodes), partial_result);
  }

 private:
  // Helpers to be able to create AST nodes.
  void AddIntegerLiteral(ServerInterpreterContext* context, uint64_t node_file_id,
                         uint64_t node_node_id, const fuchsia_shell::wire::IntegerLiteral& node,
                         bool root_node);

  void AddVariableDefinition(ServerInterpreterContext* context, uint64_t node_file_id,
                             uint64_t node_node_id,
                             const fuchsia_shell::wire::VariableDefinition& node, bool root_node);

  void AddObjectSchema(ServerInterpreterContext* context, uint64_t node_file_id,
                       uint64_t node_node_id,
                       const fuchsia_shell::wire::ObjectSchemaDefinition& node, bool root_node);

  void AddObjectSchemaField(ServerInterpreterContext* context, uint64_t node_file_id,
                            uint64_t node_node_id,
                            const fuchsia_shell::wire::ObjectFieldSchemaDefinition& field_type,
                            bool root_node);

  void AddObject(ServerInterpreterContext* context, uint64_t node_file_id, uint64_t node_node_id,
                 const fuchsia_shell::wire::ObjectDefinition& node, bool root_node);

  void AddObjectField(ServerInterpreterContext* context, uint64_t node_file_id,
                      uint64_t node_node_id,
                      const fuchsia_shell::wire::ObjectFieldDefinition& field_type, bool root_node);

  void AddStringLiteral(ServerInterpreterContext* context, uint64_t node_file_id,
                        uint64_t node_node_id, const ::fidl::StringView& node, bool root_node);

  void AddVariable(ServerInterpreterContext* context, uint64_t node_file_id, uint64_t node_node_id,
                   const fidl::StringView& name, bool root_node);

  void AddEmitResult(ServerInterpreterContext* context, uint64_t node_file_id,
                     uint64_t node_node_id, const fuchsia_shell::wire::NodeId& node,
                     bool root_node);

  void AddAssignment(ServerInterpreterContext* context, uint64_t node_file_id,
                     uint64_t node_node_id, const fuchsia_shell::wire::Assignment& node,
                     bool root_node);

  void AddAddition(ServerInterpreterContext* context, uint64_t node_file_id, uint64_t node_node_id,
                   const fuchsia_shell::wire::Addition& node, bool root_node);

  // The server which created this service.
  Server* server_;
  // The binding reference which allows controlling the message dispatching
  // of this connection and sending events.
  cpp17::optional<fidl::ServerBindingRef<fuchsia_shell::Shell>> binding_;
  // The interpreter associated with this service. An interpreter can only be associated to one
  // service.
  std::unique_ptr<ServerInterpreter> interpreter_;
};

// Class which accepts connections from clients. Each time a new connection is accepted, a Service
// object is created.
class Server {
 public:
  explicit Server(async::Loop* loop);

  Server() = delete;

  // Shuts down every service connection on this server, and eventually
  // destroying the |Service| instances through unbinding them from the FIDL
  // dispatcher.
  ~Server();

  // Unregisters a service previously created with IncomingConnection.
  // This should be used when the service is being closed down.
  void ForgetService(Service* service) {
    for (auto ref = services_.begin(); ref != services_.end(); ++ref) {
      if ((*ref) == service) {
        services_.erase(ref);
        return;
      }
    }
    ZX_PANIC("Invalid service passed to EraseService");
  }

  bool Listen();

  // Listens for connections on the given channel instead of setting up a service.
  // Returns whether we were able to bind to the given |channel|.  On error, |channel| is closed and
  // we do not bind.
  zx_status_t IncomingConnection(fidl::ServerEnd<fuchsia_shell::Shell> service_request);
  void Run() { loop()->Run(); }

  async::Loop* loop() { return loop_; }

 private:
  async::Loop* loop_;
  std::vector<Service*> services_;
};

}  // namespace server
}  // namespace interpreter
}  // namespace shell

#endif  // SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SERVER_H_
