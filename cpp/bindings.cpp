#include "bindings.h"
#include "DumbHostObject.h"
#include "PreparedStatementHostObject.h"
#include "ThreadPool.h"
#include "bridge.h"
#include "logs.h"
#include "macros.h"
#include "sqlbatchexecutor.h"
#include "utils.h"
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace opsqlite {

namespace jsi = facebook::jsi;

std::string basePath;
std::shared_ptr<react::CallInvoker> invoker;
ThreadPool pool;
std::unordered_map<std::string, std::shared_ptr<jsi::Value>> updateHooks =
    std::unordered_map<std::string, std::shared_ptr<jsi::Value>>();
std::unordered_map<std::string, std::shared_ptr<jsi::Value>> commitHooks =
    std::unordered_map<std::string, std::shared_ptr<jsi::Value>>();
std::unordered_map<std::string, std::shared_ptr<jsi::Value>> rollbackHooks =
    std::unordered_map<std::string, std::shared_ptr<jsi::Value>>();

// React native will try to clean the module on JS context invalidation
// (CodePush/Hot Reload) The clearState function is called and we use this flag
// to prevent any ongoing operations from continuing work and can return early
bool invalidated = false;

void clearState() {
  invalidated = true;
  // Will terminate all operations and database connections
  sqlite_close_all();
  // We then join all the threads before the context gets invalidated
  pool.restartPool();

  updateHooks.clear();
  commitHooks.clear();
  rollbackHooks.clear();
}

void install(jsi::Runtime &rt,
             std::shared_ptr<react::CallInvoker> jsCallInvoker,
             const char *docPath) {
  invalidated = false;
  basePath = std::string(docPath);
  invoker = jsCallInvoker;

  auto open = HOSTFN("open", 3) {
    if (count == 0) {
      throw std::runtime_error("[op-sqlite][open] database name is required");
    }

    if (!args[0].isString()) {
      throw std::runtime_error(
          "[op-sqlite][open] database name must be a string");
    }

    std::string dbName = args[0].asString(rt).utf8(rt);
    std::string path = std::string(basePath);

    if (count > 1 && !args[1].isUndefined() && !args[1].isNull()) {
      if (!args[1].isString()) {
        throw std::runtime_error(
            "[op-sqlite][open] database location must be a string");
      }

      std::string lastPath = args[1].asString(rt).utf8(rt);

      if (lastPath == ":memory:") {
        path = ":memory:";
      } else if (lastPath.rfind("/", 0) == 0) {
        path = lastPath;
      } else {
        path = path + "/" + lastPath;
      }
    }

    BridgeResult result = sqlite_open(dbName, path);

    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }

    return {};
  });

  auto attach = HOSTFN("attach", 4) {
    if (count < 3) {
      throw jsi::JSError(rt,
                         "[op-sqlite][attach] Incorrect number of arguments");
    }
    if (!args[0].isString() || !args[1].isString() || !args[2].isString()) {
      throw jsi::JSError(
          rt, "dbName, databaseToAttach and alias must be a strings");
      return {};
    }

    std::string tempDocPath = std::string(basePath);
    if (count > 3 && !args[3].isUndefined() && !args[3].isNull()) {
      if (!args[3].isString()) {
        throw std::runtime_error(
            "[op-sqlite][attach] database location must be a string");
      }

      tempDocPath = tempDocPath + "/" + args[3].asString(rt).utf8(rt);
    }

    std::string dbName = args[0].asString(rt).utf8(rt);
    std::string databaseToAttach = args[1].asString(rt).utf8(rt);
    std::string alias = args[2].asString(rt).utf8(rt);
    BridgeResult result =
        sqlite_attach(dbName, tempDocPath, databaseToAttach, alias);

    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }

    return {};
  });

  auto detach = HOSTFN("detach", 2) {
    if (count < 2) {
      throw std::runtime_error(
          "[op-sqlite][detach] Incorrect number of arguments");
    }
    if (!args[0].isString() || !args[1].isString()) {
      throw std::runtime_error(
          "dbName, databaseToAttach and alias must be a strings");
      return {};
    }

    std::string dbName = args[0].asString(rt).utf8(rt);
    std::string alias = args[1].asString(rt).utf8(rt);
    BridgeResult result = sqlite_detach(dbName, alias);

    if (result.type == SQLiteError) {
      throw jsi::JSError(rt, result.message.c_str());
    }

    return {};
  });

  auto close = HOSTFN("close", 1) {
    if (count == 0) {
      throw std::runtime_error("[op-sqlite][close] database name is required");
    }

    if (!args[0].isString()) {
      throw std::runtime_error(
          "[op-sqlite][close] database name must be a string");
    }

    std::string dbName = args[0].asString(rt).utf8(rt);

    BridgeResult result = sqlite_close(dbName);

    if (result.type == SQLiteError) {
      throw jsi::JSError(rt, result.message.c_str());
    }

    return {};
  });

  auto remove = HOSTFN("delete", 2) {
    if (count == 0) {
      throw std::runtime_error("[op-sqlite][open] database name is required");
    }

    if (!args[0].isString()) {
      throw std::runtime_error(
          "[op-sqlite][open] database name must be a string");
    }

    std::string dbName = args[0].asString(rt).utf8(rt);

    std::string tempDocPath = std::string(basePath);

    if (count > 1 && !args[1].isUndefined() && !args[1].isNull()) {
      if (!args[1].isString()) {
        throw std::runtime_error(
            "[op-sqlite][open] database location must be a string");
      }

      tempDocPath = tempDocPath + "/" + args[1].asString(rt).utf8(rt);
    }

    BridgeResult result = sqlite_remove(dbName, tempDocPath);

    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }

    return {};
  });

  auto execute = HOSTFN("execute", 3) {
    const std::string dbName = args[0].asString(rt).utf8(rt);
    const std::string query = args[1].asString(rt).utf8(rt);
    std::vector<JSVariant> params;

    if (count == 3) {
      const jsi::Value &originalParams = args[2];
      params = toVariantVec(rt, originalParams);
    }

    std::vector<DumbHostObject> results;
    std::shared_ptr<std::vector<SmartHostObject>> metadata =
        std::make_shared<std::vector<SmartHostObject>>();

    auto status = sqlite_execute(dbName, query, &params, &results, metadata);

    if (status.type == SQLiteError) {
      throw std::runtime_error(status.message);
    }

    auto jsiResult = createResult(rt, status, &results, metadata);
    return jsiResult;
  });

  auto executeAsync = HOSTFN("executeAsync", 3) {
    if (count < 3) {
      throw std::runtime_error(
          "[op-sqlite][executeAsync] Incorrect arguments for executeAsync");
    }

    const std::string dbName = args[0].asString(rt).utf8(rt);
    const std::string query = args[1].asString(rt).utf8(rt);
    const jsi::Value &originalParams = args[2];

    std::vector<JSVariant> params = toVariantVec(rt, originalParams);

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");

    auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, dbName, query, params = std::move(params), resolve,
                   reject]() {
        try {
          std::vector<DumbHostObject> results;
          std::shared_ptr<std::vector<SmartHostObject>> metadata =
              std::make_shared<std::vector<SmartHostObject>>();

          auto status =
              sqlite_execute(dbName, query, &params, &results, metadata);

          if (invalidated) {
            return;
          }

          invoker->invokeAsync(
              [&rt,
               results = std::make_shared<std::vector<DumbHostObject>>(results),
               metadata, status = std::move(status), resolve, reject] {
                if (status.type == SQLiteOk) {
                  auto jsiResult =
                      createResult(rt, status, results.get(), metadata);
                  resolve->asObject(rt).asFunction(rt).call(
                      rt, std::move(jsiResult));
                } else {
                  auto errorCtr =
                      rt.global().getPropertyAsFunction(rt, "Error");
                  auto error = errorCtr.callAsConstructor(
                      rt, jsi::String::createFromUtf8(rt, status.message));
                  reject->asObject(rt).asFunction(rt).call(rt, error);
                }
              });

        } catch (std::exception &exc) {
          invoker->invokeAsync([&rt, exc = std::move(exc), reject] {
            auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
            auto error = errorCtr.callAsConstructor(
                rt, jsi::String::createFromAscii(rt, exc.what()));
            reject->asObject(rt).asFunction(rt).call(rt, error);
          });
        }
      };

      pool.queueWork(task);

      return {};
    }));

    return promise;
  });

  // Execute a batch of SQL queries in a transaction
  // Parameters can be: [[sql: string, arguments: any[] | arguments: any[][] ]]
  auto executeBatch = HOSTFN("executeBatch", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][executeBatch] - Incorrect parameter count");
    }

    const jsi::Value &params = args[1];
    if (params.isNull() || params.isUndefined()) {
      throw std::runtime_error("[op-sqlite][executeBatch] - An array of SQL "
                               "commands or parameters is needed");
    }
    const std::string dbName = args[0].asString(rt).utf8(rt);
    const jsi::Array &batchParams = params.asObject(rt).asArray(rt);
    std::vector<BatchArguments> commands;
    toBatchArguments(rt, batchParams, &commands);

    auto batchResult = sqliteExecuteBatch(dbName, &commands);
    if (batchResult.type == SQLiteOk) {
      auto res = jsi::Object(rt);
      res.setProperty(rt, "rowsAffected", jsi::Value(batchResult.affectedRows));
      return std::move(res);
    } else {
      throw std::runtime_error(batchResult.message);
    }
  });

  auto executeBatchAsync = HOSTFN("executeBatchAsync", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][executeAsyncBatch] Incorrect parameter count");
      return {};
    }

    const jsi::Value &params = args[1];

    if (params.isNull() || params.isUndefined()) {
      throw std::runtime_error(
          "[op-sqlite][executeAsyncBatch] - An array of SQL "
          "commands or parameters is needed");
      return {};
    }

    const std::string dbName = args[0].asString(rt).utf8(rt);
    const jsi::Array &batchParams = params.asObject(rt).asArray(rt);

    std::vector<BatchArguments> commands;
    toBatchArguments(rt, batchParams, &commands);

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
        auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, dbName,
                   commands =
                       std::make_shared<std::vector<BatchArguments>>(commands),
                   resolve, reject]() {
        try {
          auto batchResult = sqliteExecuteBatch(dbName, commands.get());
          invoker->invokeAsync(
              [&rt, batchResult = std::move(batchResult), resolve, reject] {
                if (batchResult.type == SQLiteOk) {
                  auto res = jsi::Object(rt);
                  res.setProperty(rt, "rowsAffected",
                                  jsi::Value(batchResult.affectedRows));
                  resolve->asObject(rt).asFunction(rt).call(rt, std::move(res));
                } else {
                  // TODO replace with reject
                  throw jsi::JSError(rt, batchResult.message);
                }
              });
        } catch (std::exception &exc) {
          invoker->invokeAsync(
              [&rt, reject, &exc] { throw jsi::JSError(rt, exc.what()); });
        }
      };
      pool.queueWork(task);

      return {};
        }));

        return promise;
  });

  auto loadFile = HOSTFN("loadFile", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][loadFileAsync] Incorrect parameter count");
      return {};
    }

    const std::string dbName = args[0].asString(rt).utf8(rt);
    const std::string sqlFileName = args[1].asString(rt).utf8(rt);

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
        auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, dbName, sqlFileName, resolve, reject]() {
        try {
          const auto importResult = importSQLFile(dbName, sqlFileName);

          invoker->invokeAsync(
              [&rt, result = std::move(importResult), resolve, reject] {
                if (result.type == SQLiteOk) {
                  auto res = jsi::Object(rt);
                  res.setProperty(rt, "rowsAffected",
                                  jsi::Value(result.affectedRows));
                  res.setProperty(rt, "commands", jsi::Value(result.commands));
                  resolve->asObject(rt).asFunction(rt).call(rt, std::move(res));
                } else {
                  throw jsi::JSError(rt, result.message);
                }
              });
        } catch (std::exception &exc) {
          invoker->invokeAsync(
              [&rt, err = exc.what(), reject] { throw jsi::JSError(rt, err); });
        }
      };
      pool.queueWork(task);
      return {};
        }));

        return promise;
  });

  auto updateHook = HOSTFN("updateHook", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][loadFileAsync] Incorrect parameters: "
          "dbName and callback needed");
      return {};
    }

    auto dbName = args[0].asString(rt).utf8(rt);
    auto callback = std::make_shared<jsi::Value>(rt, args[1]);

    if (callback->isUndefined() || callback->isNull()) {
      sqlite_deregister_update_hook(dbName);
      return {};
    }

    updateHooks[dbName] = callback;

    auto hook = [&rt, callback](std::string dbName, std::string tableName,
                                std::string operation, int rowId) {
      std::vector<JSVariant> params;
      std::vector<DumbHostObject> results;
      std::shared_ptr<std::vector<SmartHostObject>> metadata =
          std::make_shared<std::vector<SmartHostObject>>();
      ;

      if (operation != "DELETE") {
        std::string query = "SELECT * FROM " + tableName +
                            " where rowid = " + std::to_string(rowId) + ";";
        sqlite_execute(dbName, query, &params, &results, metadata);
      }

      invoker->invokeAsync(
          [&rt,
           results = std::make_shared<std::vector<DumbHostObject>>(results),
           callback, tableName = std::move(tableName),
           operation = std::move(operation), &rowId] {
            auto res = jsi::Object(rt);
            res.setProperty(rt, "table",
                            jsi::String::createFromUtf8(rt, tableName));
            res.setProperty(rt, "operation",
                            jsi::String::createFromUtf8(rt, operation));
            res.setProperty(rt, "rowId", jsi::Value(rowId));
            if (results->size() != 0) {
              res.setProperty(
                  rt, "row",
                  jsi::Object::createFromHostObject(
                      rt, std::make_shared<DumbHostObject>(results->at(0))));
            }

            callback->asObject(rt).asFunction(rt).call(rt, res);
          });
    };

    sqlite_register_update_hook(dbName, std::move(hook));

    return {};
  });

  auto commitHook = HOSTFN("commitHook", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][loadFileAsync] Incorrect parameters: "
          "dbName and callback needed");
      return {};
    }

    auto dbName = args[0].asString(rt).utf8(rt);
    auto callback = std::make_shared<jsi::Value>(rt, args[1]);
    if (callback->isUndefined() || callback->isNull()) {
      sqlite_deregister_commit_hook(dbName);
      return {};
    }
    commitHooks[dbName] = callback;

    auto hook = [&rt, callback](std::string dbName) {
      invoker->invokeAsync(
          [&rt, callback] { callback->asObject(rt).asFunction(rt).call(rt); });
    };

    sqlite_register_commit_hook(dbName, std::move(hook));

    return {};
  });

  auto rollbackHook = HOSTFN("rollbackHook", 2) {
    if (sizeof(args) < 2) {
      throw std::runtime_error(
          "[op-sqlite][loadFileAsync] Incorrect parameters: "
          "dbName and callback needed");
      return {};
    }

    auto dbName = args[0].asString(rt).utf8(rt);
    auto callback = std::make_shared<jsi::Value>(rt, args[1]);

    if (callback->isUndefined() || callback->isNull()) {
      sqlite_deregister_rollback_hook(dbName);
      return {};
    }
    rollbackHooks[dbName] = callback;

    auto hook = [&rt, callback](std::string dbName) {
      invoker->invokeAsync(
          [&rt, callback] { callback->asObject(rt).asFunction(rt).call(rt); });
    };

    sqlite_register_rollback_hook(dbName, std::move(hook));
    return {};
  });

  auto prepareStatement = HOSTFN("prepareStatement", 1) {
    auto dbName = args[0].asString(rt).utf8(rt);
    auto query = args[1].asString(rt).utf8(rt);

    sqlite3_stmt *statement = sqlite_prepare_statement(dbName, query);

    auto preparedStatementHostObject =
        std::make_shared<PreparedStatementHostObject>(dbName, statement);

    return jsi::Object::createFromHostObject(rt, preparedStatementHostObject);
  });

  jsi::Object module = jsi::Object(rt);

  module.setProperty(rt, "open", std::move(open));
  module.setProperty(rt, "close", std::move(close));
  module.setProperty(rt, "attach", std::move(attach));
  module.setProperty(rt, "detach", std::move(detach));
  module.setProperty(rt, "delete", std::move(remove));
  module.setProperty(rt, "execute", std::move(execute));
  module.setProperty(rt, "executeAsync", std::move(executeAsync));
  module.setProperty(rt, "executeBatch", std::move(executeBatch));
  module.setProperty(rt, "executeBatchAsync", std::move(executeBatchAsync));
  module.setProperty(rt, "loadFile", std::move(loadFile));
  module.setProperty(rt, "updateHook", std::move(updateHook));
  module.setProperty(rt, "commitHook", std::move(commitHook));
  module.setProperty(rt, "rollbackHook", std::move(rollbackHook));
  module.setProperty(rt, "prepareStatement", std::move(prepareStatement));

  rt.global().setProperty(rt, "__OPSQLiteProxy", std::move(module));
}

} // namespace opsqlite
